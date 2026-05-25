/*
 * btcpc_scene_ble.c — BLE signing + data channel scene
 *
 * Handles five custom events:
 *
 *   BtcpcEventSignRequest
 *     Signs the pending hash with ed25519 and notifies the phone via
 *     SIGN_RESPONSE.
 *
 *   BtcpcEventBleConnected / BtcpcEventBleDisconnected
 *     Refreshes the display and resets the heartbeat counter on disconnect.
 *
 *   BtcpcEventDataRx
 *     Parses an incoming DATA_CHANNEL frame from the phone and dispatches
 *     by message type: ClockSync (store anchor + show synced status), GPS
 *     (store coordinates), SensorReq (trigger sensor capture on demand).
 *
 *   BtcpcEventHeartbeatTimer
 *     Fired every 30 seconds while BLE is running. Pushes a BtcpcHeartbeat
 *     frame via DATA_CHANNEL. Every second heartbeat also pushes a
 *     BtcpcSubGhzObs frame (i.e. a SubGhz scan every 60 seconds).
 *
 * Privacy design — "what, not which":
 *   RFID, NFC, iButton, and IR captures never transmit card IDs, UIDs, ROM
 *   codes, or IR address/command codes. Only the protocol/technology TYPE and
 *   an ephemeral obs_id (derived from signing "obs:"+type+proto+epoch_minute
 *   with the Flipper's identity key) are sent. The obs_id is time-bounded,
 *   keyed to device identity, and not reversible to any credential.
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "btcpc_scene_ble.h"

#include <gui/modules/text_box.h>
#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_power.h>

#include "../protocol/btcpc_protocol.h"
#include "../ble/btcpc_ble_svc.h"
#include "../ble/btcpc_ble_profile.h"

/* RFID / 125 kHz LF worker */
#include <lib/lfrfid/lfrfid_worker.h>
#include <lib/lfrfid/protocols/lfrfid_protocol.h>

/* iButton / 1-Wire worker */
#include <lib/ibutton/ibutton_worker.h>
#include <lib/ibutton/ibutton_key.h>

/* Infrared worker */
#include <lib/infrared/worker/infrared_worker.h>
#include <infrared.h>

/* NFC HAL */
#include <furi_hal_nfc.h>

#include <string.h>
#include <stdio.h>

#define BLE_TEXT_LEN 256

/* ─── obs_id helpers ─────────────────────────────────────────────────────── */

/*
 * btcpc_epoch_minute()
 *
 * Returns the current minute counter for obs_id generation.
 *
 * If a ClockSync has been received from the phone, we compute:
 *   now_ms = last_clock_unix_ms + elapsed_ms_since_sync
 * and return now_ms / 60000.
 *
 * Without a ClockSync we fall back to local ticks, which gives a
 * device-relative minute counter. The obs_id is still bound to the
 * Flipper's identity key — it just won't be wall-clock aligned until
 * the phone syncs us.
 */
static uint32_t btcpc_epoch_minute(BtcpcApp* app) {
    if(app->clock_synced) {
        uint32_t now_tick   = furi_get_tick();
        uint32_t tick_freq  = furi_kernel_get_tick_frequency();
        /* elapsed_ms: safe cast — ticks wrap at 32-bit; subtraction is still correct */
        uint64_t elapsed_ms = (uint64_t)(now_tick - app->last_clock_tick)
                              * 1000ULL / (uint64_t)tick_freq;
        return (uint32_t)((app->last_clock_unix_ms + elapsed_ms) / 60000ULL);
    }
    /* Fallback: local tick / 60 000 ms */
    return (uint32_t)(furi_get_tick() / furi_ms_to_ticks(60000));
}

/*
 * btcpc_make_obs_id()
 *
 * Generate a 16-byte ephemeral proof-of-presence.
 *
 * Signs the 10-byte message  "obs:" || sensor_type || protocol || epoch_minute
 * using the Flipper's ed25519 identity key and copies the first 16 bytes of
 * the 64-byte signature into obs_id[].
 *
 * The result changes every minute, is keyed to this specific Flipper's identity,
 * and is not reversible to any card/tag/remote identifier.
 *
 * Must NOT be called while sign_mutex is already held — it acquires the mutex
 * internally. Call btcpc_make_obs_id(), then separately acquire sign_mutex for
 * the frame builder call.
 */
static void btcpc_make_obs_id(BtcpcApp* app, uint8_t sensor_type, uint8_t protocol,
                               uint8_t obs_id[16]) {
    uint32_t epoch_min = btcpc_epoch_minute(app);
    uint8_t msg[10] = {
        'o', 'b', 's', ':',
        sensor_type,
        protocol,
        (uint8_t)(epoch_min),
        (uint8_t)(epoch_min >> 8),
        (uint8_t)(epoch_min >> 16),
        (uint8_t)(epoch_min >> 24),
    };
    uint8_t sig[BTCPC_ED25519_SIG_LEN];
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    btcpc_ed25519_sign(sig, msg, sizeof(msg), app->sk);
    furi_mutex_release(app->sign_mutex);
    memcpy(obs_id, sig, 16);
    /* Zero the rest of the sig so the seed doesn't sit in stack memory */
    memset(sig, 0, sizeof(sig));
}

/* ─── RFID capture ───────────────────────────────────────────────────────── */

/* Context for the lfrfid_worker read callback */
typedef struct {
    volatile bool detected;
    uint8_t       protocol_code; /* our BTCPC protocol byte */
} LfrfidCtx;

/*
 * Map the lfrfid protocol ID to our compact BTCPC byte:
 *   0=EM4100, 1=HID (any variant), 2=Indala (any variant), 3=Hitag, 0xFF=unknown.
 *
 * LFRFIDProtocol is an enum in the Flipper SDK. The numeric values are stable
 * across firmware releases; we guard with default to handle additions.
 */
static uint8_t lfrfid_protocol_to_btcpc(LFRFIDProtocol proto) {
    switch(proto) {
    case LFRFIDProtocolEM4100:
    case LFRFIDProtocolEM410032:
        return 0;
    case LFRFIDProtocolHIDGeneric:
    case LFRFIDProtocolHID26:
    case LFRFIDProtocolHID34:
    case LFRFIDProtocolHID37:
    case LFRFIDProtocolHIDCorporate1000:
        return 1;
    case LFRFIDProtocolIndala26:
    case LFRFIDProtocolIndala40:
        return 2;
    case LFRFIDProtocolHitag1:
    case LFRFIDProtocolHitag2:
    case LFRFIDProtocolHitagS:
        return 3;
    default:
        return 0xFF;
    }
}

/*
 * lfrfid_worker fires this callback from its thread when a card is detected.
 * We record the protocol and set the detected flag; the main thread polls.
 */
static void lfrfid_read_cb(LFRFIDWorkerReadResult result,
                            ProtocolId              protocol,
                            void*                   ctx) {
    if(result != LFRFIDWorkerReadDone) return;
    LfrfidCtx* c    = ctx;
    c->protocol_code = lfrfid_protocol_to_btcpc((LFRFIDProtocol)protocol);
    c->detected      = true;
}

/*
 * btcpc_capture_rfid()
 *
 * Spins up the LF RFID worker for up to 1 second. If a card is detected,
 * fills `scan` (protocol + obs_id) and returns true.
 * Returns false on timeout with no detection — caller must not push a frame.
 *
 * Card IDs are NEVER read or stored. Protocol type only.
 */
static bool btcpc_capture_rfid(BtcpcApp* app, BtcpcRfidScan* scan) {
    LfrfidCtx ctx = {.detected = false, .protocol_code = 0xFF};

    LFRFIDWorker* worker = lfrfid_worker_alloc(lfrfid_worker_get_protocols_manager());
    lfrfid_worker_start_thread(worker);
    /* LFRFIDWorkerReadTypeAuto tries all supported protocols */
    lfrfid_worker_read_start(worker, LFRFIDWorkerReadTypeAuto, lfrfid_read_cb, &ctx);

    /* Poll for up to 1000 ms in 50 ms steps */
    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(1000);
    while(!ctx.detected && (int32_t)(deadline - furi_get_tick()) > 0) {
        furi_delay_ms(50);
    }

    lfrfid_worker_stop(worker);
    lfrfid_worker_stop_thread(worker);
    lfrfid_worker_free(worker);

    if(!ctx.detected) return false;

    scan->protocol = ctx.protocol_code;
    btcpc_make_obs_id(app, (uint8_t)BTCPC_MSG_RFID_SCAN, ctx.protocol_code, scan->obs_id);
    return true;
}

/* ─── NFC capture ────────────────────────────────────────────────────────── */

/*
 * btcpc_capture_nfc()
 *
 * Attempts NFC Type A detection via furi_hal_nfc_detect() for ~500 ms.
 * Fills `scan` with tech + tag_family + obs_id.
 *
 * Tag UIDs, ATQA, and SAK are intentionally NOT stored or transmitted.
 *
 * If furi_hal_nfc_detect() is not available in the target SDK build,
 * the #else branch falls back to a simple "TypeA present, family unknown"
 * result — still safe, still useful for presence detection.
 *
 * Returns false if no tag was detected within the window.
 */
static bool btcpc_capture_nfc(BtcpcApp* app, BtcpcNfcScan* scan) {
    furi_hal_nfc_acquire();
    furi_hal_nfc_low_power_mode_stop();

    bool detected = false;
    uint8_t tech       = 0;   /* default TypeA */
    uint8_t tag_family = 0;   /* default unknown */

#if defined(FURI_HAL_NFC_DETECT_AVAILABLE)
    /* Full detect with ATQA/SAK — derive family without storing uid */
    FuriHalNfcDevData nfc_data;
    memset(&nfc_data, 0, sizeof(nfc_data));

    if(furi_hal_nfc_detect(&nfc_data, furi_ms_to_ticks(500))) {
        detected = true;
        /* Map tech from detection type (NfcDeviceDataA, B, F, V) */
        switch(nfc_data.type) {
        case FuriHalNfcTypeA: tech = 0; break;
        case FuriHalNfcTypeB: tech = 1; break;
        case FuriHalNfcTypeF: tech = 2; break;
        case FuriHalNfcTypeV: tech = 3; break;
        default:              tech = 0; break;
        }
        if(nfc_data.type == FuriHalNfcTypeA) {
            /* Derive tag family from ATQA/SAK — public standard classification */
            uint8_t atqa0 = nfc_data.atqa[0];
            uint8_t sak   = nfc_data.sak;
            if((atqa0 & 0x20) && (sak == 0x20)) {
                tag_family = 3; /* DESFire (ISO 14443-4 compliant) */
            } else if(atqa0 == 0x44 || atqa0 == 0x00) {
                tag_family = 2; /* NTAG / MIFARE Ultralight */
            } else if(atqa0 == 0x04 || atqa0 == 0x02) {
                tag_family = 1; /* MIFARE Classic */
            } else {
                tag_family = 0;
            }
        } else if(nfc_data.type == FuriHalNfcTypeF) {
            tag_family = 4; /* FeliCa */
        } else if(nfc_data.type == FuriHalNfcTypeV) {
            tag_family = 5; /* ISO 15693 */
        }
    }
#else
    /* Fallback: simple presence detect, TypeA, family unknown.
     * furi_hal_nfc_is_present() or equivalent lightweight poll. */
    if(furi_hal_nfc_is_present(furi_ms_to_ticks(500))) {
        detected   = true;
        tech       = 0;
        tag_family = 0;
    }
#endif

    furi_hal_nfc_low_power_mode_start();
    furi_hal_nfc_release();

    if(!detected) return false;

    scan->tech       = tech;
    scan->tag_family = tag_family;
    btcpc_make_obs_id(app, (uint8_t)BTCPC_MSG_NFC_SCAN, tag_family, scan->obs_id);
    return true;
}

/* ─── iButton capture ────────────────────────────────────────────────────── */

/* Context for the ibutton_worker read callback */
typedef struct {
    volatile bool detected;
    uint8_t       family_code;
} IButtonCtx;

static void ibutton_read_cb(void* ctx) {
    /* This callback fires when ibutton_worker_get_key() has fresh data.
     * We set the flag; the main thread reads the key. */
    IButtonCtx* c = ctx;
    c->detected = true;
}

/*
 * btcpc_capture_ibutton()
 *
 * Spins up the iButton worker for up to 500 ms. If a key is detected,
 * extracts the family code (ROM byte 0) and fills `btn` with family + obs_id.
 *
 * The 64-bit ROM code is NEVER stored or transmitted — family byte only.
 * Returns false on timeout with no detection.
 */
static bool btcpc_capture_ibutton(BtcpcApp* app, BtcpcIButton* btn) {
    IButtonCtx ctx = {.detected = false, .family_code = 0x00};

    iButtonKey*    key    = ibutton_key_alloc(IBUTTON_KEY_DATA_SIZE);
    iButtonWorker* worker = ibutton_worker_alloc();
    ibutton_worker_start_thread(worker);
    ibutton_worker_read_start(worker, key);
    ibutton_worker_set_callback(worker, ibutton_read_cb, &ctx);

    /* Poll for up to 500 ms in 25 ms steps */
    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(500);
    while(!ctx.detected && (int32_t)(deadline - furi_get_tick()) > 0) {
        furi_delay_ms(25);
    }

    uint8_t family = 0x00;
    if(ctx.detected) {
        /* First byte of the 1-Wire ROM code is the family code (public spec) */
        const uint8_t* data = ibutton_key_get_data_p(key);
        family = data[0];
        ctx.family_code = family;
    }

    ibutton_worker_stop(worker);
    ibutton_worker_stop_thread(worker);
    ibutton_worker_free(worker);
    ibutton_key_free(key);

    if(!ctx.detected) return false;

    btn->family = family;
    btcpc_make_obs_id(app, (uint8_t)BTCPC_MSG_IBUTTON, family, btn->obs_id);
    return true;
}

/* ─── IR capture ─────────────────────────────────────────────────────────── */

/* Context for the infrared_worker_rx callback */
typedef struct {
    volatile bool detected;
    uint8_t       protocol_code; /* BTCPC IR protocol byte */
} IrCtx;

/*
 * Map InfraredProtocol enum to our compact BTCPC byte:
 *   0=NEC, 1=Samsung32, 2=RC6, 3=RC5, 4=SIRC, 5=Kaseikyo, 0xFF=unknown/raw.
 *
 * InfraredProtocol is defined in <infrared.h>. Values are stable across
 * releases; default handles additions gracefully.
 */
static uint8_t ir_protocol_to_btcpc(InfraredProtocol proto) {
    switch(proto) {
    case InfraredProtocolNEC:
    case InfraredProtocolNECext:
        return 0;
    case InfraredProtocolSamsung32:
        return 1;
    case InfraredProtocolRC6:
        return 2;
    case InfraredProtocolRC5:
    case InfraredProtocolRC5X:
        return 3;
    case InfraredProtocolSIRC:
    case InfraredProtocolSIRC15:
    case InfraredProtocolSIRC20:
        return 4;
    case InfraredProtocolKaseikyo:
        return 5;
    default:
        return 0xFF;
    }
}

/*
 * The infrared_worker fires this callback from its thread each time a
 * decoded signal arrives. We capture the first decoded signal only —
 * address and command are discarded, protocol type only.
 */
static void ir_rx_cb(void* ctx, InfraredWorkerSignal* received_signal) {
    IrCtx* c = ctx;
    if(c->detected) return; /* already have one — ignore duplicates */

    if(infrared_worker_signal_is_decoded(received_signal)) {
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(received_signal);
        c->protocol_code = ir_protocol_to_btcpc(msg->protocol);
        /* address and command are intentionally NOT read */
    } else {
        /* Raw (undecoded) signal — mark as unknown protocol */
        c->protocol_code = 0xFF;
    }
    c->detected = true;
}

/*
 * btcpc_capture_ir()
 *
 * Listens for an IR signal for up to 1 second. If any signal is received,
 * fills `ir` with protocol + obs_id and returns true.
 *
 * IR address and command codes are NEVER stored or transmitted.
 * Returns false on timeout with no signal.
 */
static bool btcpc_capture_ir(BtcpcApp* app, BtcpcIrCapture* ir) {
    IrCtx ctx = {.detected = false, .protocol_code = 0xFF};

    InfraredWorker* worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(worker, ir_rx_cb, &ctx);
    infrared_worker_rx_start(worker);

    /* Poll for up to 1000 ms in 50 ms steps */
    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(1000);
    while(!ctx.detected && (int32_t)(deadline - furi_get_tick()) > 0) {
        furi_delay_ms(50);
    }

    infrared_worker_rx_stop(worker);
    infrared_worker_free(worker);

    if(!ctx.detected) return false;

    ir->protocol = ctx.protocol_code;
    btcpc_make_obs_id(app, (uint8_t)BTCPC_MSG_IR_CAPTURE, ctx.protocol_code, ir->obs_id);
    return true;
}

/* ─── capture-and-push wrappers ──────────────────────────────────────────── */

static void btcpc_capture_and_push_rfid(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcRfidScan scan;
    if(!btcpc_capture_rfid(app, &scan)) {
        FURI_LOG_D("BtcpcBle", "rfid: no card detected");
        return;
    }

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_rfid(&s_tx_frame, &scan, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "rfid sent: proto=0x%02x", (unsigned)scan.protocol);
}

static void btcpc_capture_and_push_nfc(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcNfcScan scan;
    if(!btcpc_capture_nfc(app, &scan)) {
        FURI_LOG_D("BtcpcBle", "nfc: no tag detected");
        return;
    }

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_nfc(&s_tx_frame, &scan, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "nfc sent: tech=%u family=%u", (unsigned)scan.tech, (unsigned)scan.tag_family);
}

static void btcpc_capture_and_push_ibutton(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcIButton btn;
    if(!btcpc_capture_ibutton(app, &btn)) {
        FURI_LOG_D("BtcpcBle", "ibutton: no key detected");
        return;
    }

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_ibutton(&s_tx_frame, &btn, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "ibutton sent: family=0x%02x", (unsigned)btn.family);
}

static void btcpc_capture_and_push_ir(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcIrCapture ir;
    if(!btcpc_capture_ir(app, &ir)) {
        FURI_LOG_D("BtcpcBle", "ir: no signal received");
        return;
    }

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_ir(&s_tx_frame, &ir, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "ir sent: proto=0x%02x", (unsigned)ir.protocol);
}

/* ─── Display refresh ────────────────────────────────────────────────────── */

static void btcpc_scene_ble_refresh(BtcpcApp* app) {
    static char ble_text[BLE_TEXT_LEN];

    if(app->ble_connected) {
        if(app->has_gps) {
            /* Show GPS degrees as integer.decimal parts (avoid float printf) */
            int32_t lat_deg  = app->last_lat_1e7 / 10000000;
            int32_t lat_abs  = app->last_lat_1e7 < 0 ? -app->last_lat_1e7 : app->last_lat_1e7;
            int32_t lat_frac = (lat_abs % 10000000) / 1000;
            int32_t lon_deg  = app->last_lon_1e7 / 10000000;
            int32_t lon_abs  = app->last_lon_1e7 < 0 ? -app->last_lon_1e7 : app->last_lon_1e7;
            int32_t lon_frac = (lon_abs % 10000000) / 1000;
            snprintf(ble_text, sizeof(ble_text),
                     "BLE: Connected\n\n"
                     "GPS: %ld.%04ld\n"
                     "     %ld.%04ld\n"
                     "Clock: %s\n"
                     "HB: %lu",
                     (long)lat_deg, (long)lat_frac,
                     (long)lon_deg, (long)lon_frac,
                     app->clock_synced ? "synced" : "—",
                     (unsigned long)app->heartbeat_count);
        } else {
            snprintf(ble_text, sizeof(ble_text),
                     "BLE: Connected\n\n"
                     "Clock: %s\n"
                     "HB sent: %lu\n\n"
                     "Ready for signing\n"
                     "and sensor data.",
                     app->clock_synced ? "synced" : "waiting",
                     (unsigned long)app->heartbeat_count);
        }
    } else if(app->ble_profile != NULL) {
        snprintf(ble_text, sizeof(ble_text),
                 "BLE: Advertising\n\n"
                 "Open BTCPC app\n"
                 "on phone to pair.\n\n"
                 "Name: " BTCPC_BLE_ADV_NAME);
    } else {
        snprintf(ble_text, sizeof(ble_text),
                 "BLE: Unavailable\n\n"
                 "BLE profile\n"
                 "failed to start.");
    }

    text_box_reset(app->text_box);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, ble_text);
}

/* ─── Sensor helpers (app thread only) ──────────────────────────────────── */

/* Static frame buffer — all frame building happens on the single app thread. */
static BtcpcFrame s_tx_frame;

static void btcpc_push_heartbeat(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcHeartbeat hb;
    hb.battery_pct = furi_hal_power_get_pct();
    hb.uptime_s    = (uint32_t)(furi_get_tick() / furi_kernel_get_tick_frequency());
    memset(hb.fw_version, 0, sizeof(hb.fw_version));
    strncpy((char*)hb.fw_version, BTCPC_VERSION, sizeof(hb.fw_version) - 1);

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_heartbeat(&s_tx_frame, &hb, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "heartbeat sent: bat=%u%%, uptime=%lus",
               (unsigned)hb.battery_pct, (unsigned long)hb.uptime_s);
}

/*
 * btcpc_scan_subghz()
 *
 * Quick passive RSSI scan on three common IoT frequencies.
 * Fills `obs` with the frequency that had the strongest signal.
 * Returns true on success; false if SubGhz hardware is unavailable.
 *
 * CC1101 (SubGhz) and STM32WB55 BLE are independent silicon — no conflict.
 */
static bool btcpc_scan_subghz(BtcpcSubGhzObs* obs) {
    static const uint32_t freqs[] = {
        433920000,  /* 433.92 MHz — most common global IoT/remote */
        868350000,  /* 868.35 MHz — EU IoT, LoRa */
        315000000,  /* 315 MHz    — US remotes */
    };
    static const size_t NFREQS = sizeof(freqs) / sizeof(freqs[0]);

    furi_hal_subghz_acquire();
    furi_hal_subghz_reset();

    uint32_t best_freq = freqs[0];
    float    best_rssi = -150.0f;

    for(size_t i = 0; i < NFREQS; i++) {
        furi_hal_subghz_set_frequency_and_path(freqs[i]);
        furi_hal_subghz_rx();
        furi_delay_ms(60);
        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > best_rssi) {
            best_rssi = rssi;
            best_freq = freqs[i];
        }
    }

    furi_hal_subghz_sleep();
    furi_hal_subghz_release();

    obs->freq_hz    = best_freq;
    obs->rssi_dbm   = (int8_t)(best_rssi < -128.0f ? -128 : best_rssi);
    obs->modulation = 2; /* OOK — most common for 433/315 MHz IoT */
    obs->bandwidth  = 200; /* ~200 kHz */
    return true;
}

static void btcpc_scan_and_push_subghz(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcSubGhzObs obs;
    if(!btcpc_scan_subghz(&obs)) return;

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_subghz(&s_tx_frame, &obs, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "subghz sent: %lu Hz, %d dBm",
               (unsigned long)obs.freq_hz, (int)obs.rssi_dbm);
}

/* ─── Scene lifecycle ────────────────────────────────────────────────────── */

void btcpc_scene_ble_on_enter(void* context) {
    BtcpcApp* app = context;
    btcpc_scene_ble_refresh(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_ble_on_event(void* context, SceneManagerEvent event) {
    BtcpcApp* app = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    switch((BtcpcCustomEvent)event.event) {

    /* ── Signing ─────────────────────────────────────────────────────────── */
    case BtcpcEventSignRequest: {
        uint8_t sig[BTCPC_ED25519_SIG_LEN];
        uint8_t hash_copy[BTCPC_BLE_SIGN_REQ_LEN];

        furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
        if(app->sign_pending) {
            memcpy(hash_copy, app->sign_pending_hash, BTCPC_BLE_SIGN_REQ_LEN);
            app->sign_pending = false;
        } else {
            furi_mutex_release(app->sign_mutex);
            consumed = true;
            break;
        }
        btcpc_ed25519_sign(sig, hash_copy, BTCPC_BLE_SIGN_REQ_LEN, app->sk);
        furi_mutex_release(app->sign_mutex);

        BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
        if(svc) {
            bool sent = btcpc_ble_svc_send_signature(svc, sig);
            FURI_LOG_D("BtcpcBle", "signature sent: %s", sent ? "ok" : "fail");
        }

        memset(sig,       0, sizeof(sig));
        memset(hash_copy, 0, sizeof(hash_copy));
        consumed = true;
        break;
    }

    /* ── Connection state ────────────────────────────────────────────────── */
    case BtcpcEventBleConnected:
        btcpc_scene_ble_refresh(app);
        consumed = true;
        break;

    case BtcpcEventBleDisconnected:
        app->heartbeat_count = 0;
        app->clock_synced    = false;
        app->has_gps         = false;
        btcpc_scene_ble_refresh(app);
        consumed = true;
        break;

    /* ── Incoming frame from phone ───────────────────────────────────────── */
    case BtcpcEventDataRx: {
        uint8_t  buf[BTCPC_BLE_DATA_CH_MAX_LEN];
        uint16_t len;

        furi_mutex_acquire(app->data_rx_mutex, FuriWaitForever);
        len = app->data_rx_len;
        if(len > 0) memcpy(buf, app->data_rx_buf, len);
        furi_mutex_release(app->data_rx_mutex);

        if(len == 0) { consumed = true; break; }

        BtcpcFrameHeader hdr;
        const uint8_t* payload;
        if(!btcpc_parse_frame(buf, len, &hdr, &payload)) {
            FURI_LOG_W("BtcpcBle", "bad DATA_CHANNEL frame (len=%u)", (unsigned)len);
            consumed = true;
            break;
        }

        switch((BtcpcMsgType)hdr.msg_type) {
        case BTCPC_MSG_CLOCK_SYNC: {
            uint64_t unix_ms;
            if(btcpc_parse_clock_sync(payload, hdr.payload_len, &unix_ms)) {
                app->clock_synced        = true;
                app->last_clock_unix_ms  = unix_ms;
                app->last_clock_tick     = furi_get_tick();
                btcpc_scene_ble_refresh(app);
                FURI_LOG_I("BtcpcBle", "clock synced: %llu ms", (unsigned long long)unix_ms);
            }
            break;
        }
        case BTCPC_MSG_GPS: {
            BtcpcGps gps;
            if(btcpc_parse_gps(payload, hdr.payload_len, &gps)) {
                app->has_gps      = true;
                app->last_lat_1e7 = gps.lat_1e7;
                app->last_lon_1e7 = gps.lon_1e7;
                btcpc_scene_ble_refresh(app);
            }
            break;
        }
        case BTCPC_MSG_SENSOR_REQ: {
            if(hdr.payload_len < sizeof(BtcpcSensorReq)) break;
            BtcpcSensorReq req;
            memcpy(&req, payload, sizeof(req));
            switch(req.sensor_type) {
            case BTCPC_MSG_SUBGHZ_OBS: btcpc_scan_and_push_subghz(app);    break;
            case BTCPC_MSG_HEARTBEAT:  btcpc_push_heartbeat(app);           break;
            case BTCPC_MSG_RFID_SCAN:  btcpc_capture_and_push_rfid(app);    break;
            case BTCPC_MSG_NFC_SCAN:   btcpc_capture_and_push_nfc(app);     break;
            case BTCPC_MSG_IBUTTON:    btcpc_capture_and_push_ibutton(app); break;
            case BTCPC_MSG_IR_CAPTURE: btcpc_capture_and_push_ir(app);      break;
            default:
                FURI_LOG_W("BtcpcBle", "SENSOR_REQ unknown type 0x%02x", req.sensor_type);
                break;
            }
            break;
        }
        default:
            FURI_LOG_D("BtcpcBle", "DATA_CH msg_type=0x%02x len=%u",
                       hdr.msg_type, hdr.payload_len);
            break;
        }

        consumed = true;
        break;
    }

    /* ── Periodic auto-cycle: all 6 sensor types on a staggered schedule ─── */
    case BtcpcEventHeartbeatTimer:
        if(!app->ble_connected) { consumed = true; break; }

        /*
         * Modulo checks happen BEFORE incrementing heartbeat_count so that
         * tick 0 (the first tick after connect) fires all sensors immediately
         * and gives a complete initial reading without waiting for the cycle
         * to accumulate.
         *
         * Tick schedule (30 s per tick):
         *   every tick   (always)          → heartbeat
         *   tick % 2 == 0  (every 60 s)    → SubGhz + IR   (passive, ~1 s each)
         *   tick % 3 == 0  (every 90 s)    → RFID           (1 s scan)
         *   tick % 4 == 0  (every 120 s)   → NFC            (500 ms scan)
         *   tick % 5 == 0  (every 150 s)   → iButton        (500 ms scan)
         *
         * If multiple moduli fire on the same tick (e.g. tick 0 or tick 60)
         * the captures run sequentially. Worst case ~3 s of blocking per tick
         * inside a 30 s interval — acceptable on the app thread.
         *
         * SENSOR_REQ phone-side on-demand dispatch is unchanged above.
         */

        /* Every tick — heartbeat */
        btcpc_push_heartbeat(app);

        /* Every 2nd tick — SubGhz passive RSSI scan + IR passive listen */
        if(app->heartbeat_count % 2 == 0) {
            btcpc_scan_and_push_subghz(app);
            btcpc_capture_and_push_ir(app);
        }

        /* Every 3rd tick — RFID 1 s scan, skips silently if no card */
        if(app->heartbeat_count % 3 == 0) {
            btcpc_capture_and_push_rfid(app);
        }

        /* Every 4th tick — NFC 500 ms scan, skips silently if no tag */
        if(app->heartbeat_count % 4 == 0) {
            btcpc_capture_and_push_nfc(app);
        }

        /* Every 5th tick — iButton 500 ms scan, skips silently if no key */
        if(app->heartbeat_count % 5 == 0) {
            btcpc_capture_and_push_ibutton(app);
        }

        app->heartbeat_count++;

        btcpc_scene_ble_refresh(app);
        consumed = true;
        break;

    default:
        break;
    }

    return consumed;
}

void btcpc_scene_ble_on_exit(void* context) {
    BtcpcApp* app = context;
    text_box_reset(app->text_box);
    /* BLE profile stays active across scene navigation */
}
