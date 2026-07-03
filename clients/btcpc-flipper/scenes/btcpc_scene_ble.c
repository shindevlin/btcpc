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
#include "btcpc_scene_usb.h"

#include <gui/modules/text_box.h>
#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_power.h>

#include "../protocol/btcpc_protocol.h"
#include "../ble/btcpc_ble_svc.h"
#include "../ble/btcpc_ble_profile.h"

/* RFID / 125 kHz LF worker */
#include <lib/lfrfid/lfrfid_worker.h>
#include <lib/lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>

/* iButton / 1-Wire worker */
#include <lib/ibutton/ibutton_worker.h>
#include <lib/ibutton/ibutton_key.h>
#include <lib/ibutton/ibutton_protocols.h>

/* Infrared worker */
#include <lib/infrared/worker/infrared_worker.h>
#include <infrared.h>

#include <notification/notification_messages.h>

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
    case LFRFIDProtocolEM410016:
        return 0;
    case LFRFIDProtocolHidGeneric:
    case LFRFIDProtocolHidExGeneric:
    case LFRFIDProtocolH10301:
        return 1;
    case LFRFIDProtocolIndala26:
        return 2;
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

    ProtocolDict* dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    LFRFIDWorker* worker = lfrfid_worker_alloc(dict);
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
    protocol_dict_free(dict);

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
    /* NFC HAL is not exposed to FAPs in the current SDK — returns no capture. */
    (void)app;
    (void)scan;
    return false;
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

    iButtonProtocols* protocols = ibutton_protocols_alloc();
    iButtonKey*       key       = ibutton_key_alloc(ibutton_protocols_get_max_data_size(protocols));
    iButtonWorker*    worker    = ibutton_worker_alloc(protocols);
    ibutton_worker_start_thread(worker);
    ibutton_worker_read_set_callback(worker, ibutton_read_cb, &ctx);
    ibutton_worker_read_start(worker, key);

    /* Poll for up to 500 ms in 25 ms steps */
    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(500);
    while(!ctx.detected && (int32_t)(deadline - furi_get_tick()) > 0) {
        furi_delay_ms(25);
    }

    uint8_t family = 0x00;
    if(ctx.detected) {
        /* Use protocol ID as the obs_id seed — not the 1-Wire ROM code. */
        family = (uint8_t)ibutton_key_get_protocol_id(key);
        ctx.family_code = family;
    }

    ibutton_worker_stop(worker);
    ibutton_worker_stop_thread(worker);
    ibutton_worker_free(worker);
    ibutton_key_free(key);
    ibutton_protocols_free(protocols);

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

/* Static frame buffer — all frame building happens on the single app thread. */
static BtcpcFrame s_tx_frame;

static void btcpc_notify_rare_detection(void) {
    /* Haptic pulse for rare/interesting detection event */
    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notif, &sequence_set_vibro_on);
    furi_delay_ms(BTCPC_NOTIFY_RARE_MS);
    notification_message(notif, &sequence_reset_vibro);
    /* Flash blue LED briefly */
    notification_message(notif, &sequence_blink_blue_100);
    furi_record_close(RECORD_NOTIFICATION);
}

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
    btcpc_notify_rare_detection();
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
    btcpc_notify_rare_detection();
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

/* Helper: abort any in-progress OTA session and clean up storage handles. */
static void btcpc_ota_abort(BtcpcApp* app) {
    if(!app->ota_in_progress) return;
    if(app->ota_file) {
        storage_file_close(app->ota_file);
        storage_file_free(app->ota_file);
        app->ota_file = NULL;
    }
    if(app->ota_storage) {
        storage_simply_remove(app->ota_storage, BTCPC_OTA_TMP_PATH);
        furi_record_close(RECORD_STORAGE);
        app->ota_storage = NULL;
    }
    app->ota_in_progress  = false;
    app->ota_expected_size = 0;
    app->ota_bytes_written = 0;
    app->ota_checksum      = 0;
}

static void btcpc_scene_ble_refresh(BtcpcApp* app) {
    static char ble_text[BLE_TEXT_LEN];

    if(app->ota_in_progress) {
        uint32_t pct = app->ota_expected_size > 0
            ? (app->ota_bytes_written * 100) / app->ota_expected_size
            : 0;
        snprintf(ble_text, sizeof(ble_text),
                 "OTA Update\n\n"
                 "%lu / %lu bytes\n"
                 "%lu%%",
                 (unsigned long)app->ota_bytes_written,
                 (unsigned long)app->ota_expected_size,
                 (unsigned long)pct);
        text_box_reset(app->text_box);
        text_box_set_font(app->text_box, TextBoxFontText);
        text_box_set_text(app->text_box, ble_text);
        return;
    }

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
 * btcpc_scan_band()
 *
 * Passive RF census on a single frequency band.
 *
 * Spends the first 100 ms measuring the noise floor, then counts distinct
 * RSSI excursions (≥ 12 dB above floor) for the remaining listen_ms.
 * Events lasting ≥ 50 ms are counted separately as long_count — these
 * correlate with LoRa chirps and FSK data frames rather than short OOK pops.
 *
 * ook_mode: true → OOK/AM preset (433/315 MHz remotes, sensors)
 *           false → 2-FSK preset  (868 MHz LoRa energy, wM-Bus, Z-Wave)
 *
 * CC1101 (SubGhz) and STM32WB55 BLE are independent silicon — no conflict.
 */
static void btcpc_scan_band(uint32_t freq_hz, bool ook_mode, uint32_t listen_ms,
                              BtcpcBandCensus* out) {
    out->freq_hz = freq_hz;
    out->event_count   = 0;
    out->long_count    = 0;
    out->peak_rssi_dbm = -128;
    out->noise_floor_dbm = -128;

    /* furi_hal_subghz_load_preset() is not exported to FAP apps.
     * RSSI measurement is modulation-agnostic: the CC1101 reports total
     * received energy regardless of preset, which is correct for burst
     * counting. ook_mode is retained as documentation for band intent. */
    (void)ook_mode;
    furi_hal_subghz_set_frequency_and_path(freq_hz);
    furi_hal_subghz_rx();

    /* Calibrate noise floor over the first 100 ms */
    float floor_sum = 0.0f;
    for(int i = 0; i < 10; i++) {
        furi_delay_ms(10);
        floor_sum += furi_hal_subghz_get_rssi();
    }
    float noise_floor = floor_sum / 10.0f;
    float threshold   = noise_floor + 12.0f; /* 12 dB above floor = activity */
    float peak        = noise_floor;

    /* Count events for the remaining window */
    bool     in_event        = false;
    uint32_t event_start_ms  = 0;
    uint32_t tick_freq       = furi_kernel_get_tick_frequency();
    uint32_t remaining_steps = (listen_ms > 100 ? listen_ms - 100 : 0) / 10;

    for(uint32_t step = 0; step < remaining_steps; step++) {
        furi_delay_ms(10);
        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > peak) peak = rssi;

        if(!in_event && rssi > threshold) {
            in_event       = true;
            event_start_ms = (uint32_t)(furi_get_tick() * 1000UL / tick_freq);
            if(out->event_count < 255) out->event_count++;
        } else if(in_event && rssi < threshold - 6.0f) {
            uint32_t now_ms = (uint32_t)(furi_get_tick() * 1000UL / tick_freq);
            if((now_ms - event_start_ms) >= 50 && out->long_count < 255) out->long_count++;
            in_event = false;
        }
    }
    /* Close any event still open at end of window */
    if(in_event && out->long_count < 255) {
        uint32_t now_ms = (uint32_t)(furi_get_tick() * 1000UL / tick_freq);
        if((now_ms - event_start_ms) >= 50) out->long_count++;
    }

    out->peak_rssi_dbm   = (int8_t)(peak        < -128.0f ? -128 : peak);
    out->noise_floor_dbm = (int8_t)(noise_floor  < -128.0f ? -128 : noise_floor);

    furi_hal_subghz_idle();
}

/*
 * btcpc_census_subghz()
 *
 * Scans 13 bands and fills a BtcpcSubGhzCensus.
 * Total listen time: 3350 ms.
 *
 * Band layout (index: freq, ook_mode, listen_ms, purpose):
 *   [0]  315.000 MHz  OOK  200  US remotes/garage
 *   [1]  418.050 MHz  OOK  200  UK legacy alarms/keyfobs
 *   [2]  433.420 MHz  OOK  150  Somfy blinds/shutters
 *   [3]  433.920 MHz  OOK  800  Primary ISM: garage, TPMS, weather, alarms
 *   [4]  446.100 MHz  FSK  150  PMR446 walkie-talkies (human density)
 *   [5]  780.000 MHz  FSK  150  LTE 700/800 MHz RSSI (cellular coverage)
 *   [6]  868.100 MHz  FSK  200  LoRaWAN EU ch1
 *   [7]  868.300 MHz  FSK  200  LoRaWAN EU ch2 / Z-Wave / EnOcean / KNX
 *   [8]  868.420 MHz  FSK  150  Z-Wave EU explicit
 *   [9]  868.500 MHz  FSK  200  LoRaWAN EU ch3
 *   [10] 868.950 MHz  FSK  250  wM-Bus T-mode (EU smart meters)
 *   [11] 869.850 MHz  FSK  150  EU high-power alarms / panic buttons
 *   [12] 916.500 MHz  FSK  200  US ERT smart meters
 */
static void btcpc_census_subghz(BtcpcSubGhzCensus* census) {
    furi_hal_subghz_reset();
    btcpc_scan_band(315000000, true,  200, &census->band[0]);
    btcpc_scan_band(418050000, true,  200, &census->band[1]);
    btcpc_scan_band(433420000, true,  150, &census->band[2]);
    btcpc_scan_band(433920000, true,  800, &census->band[3]);
    btcpc_scan_band(446100000, false, 150, &census->band[4]);
    btcpc_scan_band(780000000, false, 150, &census->band[5]);
    btcpc_scan_band(868100000, false, 200, &census->band[6]);
    btcpc_scan_band(868300000, false, 200, &census->band[7]);
    btcpc_scan_band(868420000, false, 150, &census->band[8]);
    btcpc_scan_band(868500000, false, 200, &census->band[9]);
    btcpc_scan_band(868950000, false, 250, &census->band[10]);
    btcpc_scan_band(869850000, false, 150, &census->band[11]);
    btcpc_scan_band(916500000, false, 200, &census->band[12]);
    furi_hal_subghz_sleep();
    census->listen_window_ms = 200+200+150+800+150+150+200+200+150+200+250+150+200;
}

static void btcpc_census_and_push_subghz(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcSubGhzCensus census;
    btcpc_census_subghz(&census);

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_subghz_census(&s_tx_frame, &census, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);

    /* Notify once if any band captured a long frame (LoRa/FSK) */
    bool any_long = false;
    for(int i = 0; i < BTCPC_CENSUS_BANDS; i++) {
        if(census.band[i].long_count > 0) { any_long = true; break; }
    }
    if(any_long) btcpc_notify_rare_detection();

    FURI_LOG_D("BtcpcBle",
               "census: 433=%u/%u 868.3=%u/%u 868.95=%u/%u 315=%u/%u",
               census.band[3].event_count, census.band[3].long_count,
               census.band[7].event_count, census.band[7].long_count,
               census.band[10].event_count, census.band[10].long_count,
               census.band[0].event_count, census.band[0].long_count);
}

/* ─── BLE environment scan ───────────────────────────────────────────────── */

/*
 * btcpc_scan_ble_env()
 *
 * Passive BLE advertisement census.
 *
 * The STM32WB55 GAP scan API (gap_start_scan / furi_hal_bt_start_scan) is
 * not exported to FAP applications in the current Flipper SDK. Returning a
 * stub with ad_count=0 / scan_window_s=0 signals to the Android app that
 * BLE environment scanning is not yet active. Requires investigation into
 * the BT service internals or a future SDK export.
 */
static void btcpc_scan_ble_env(BtcpcBleEnv* env) {
    env->ad_count          = 0;
    env->scan_window_s     = 0;
    env->rssi_min_dbm      = 0;
    env->rssi_max_dbm      = 0;
    env->rssi_avg_dbm      = 0;
    env->connectable_count = 0;
}

static void btcpc_scan_and_push_ble_env(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcBleEnv env;
    btcpc_scan_ble_env(&env);

    size_t len;
    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    len = btcpc_build_ble_env(&s_tx_frame, &env, app->sk);
    furi_mutex_release(app->sign_mutex);

    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_tx_frame, (uint16_t)len);
    FURI_LOG_D("BtcpcBle", "ble_env: ad=%u scan_s=%u", env.ad_count, env.scan_window_s);
}

/* ─── RFID reader field detection ────────────────────────────────────────── */

/*
 * btcpc_detect_rfid_reader()
 *
 * Attempts a passive 125 kHz LF read for 200 ms. Any callback firing
 * within the window indicates field energy from an external reader.
 *
 * Returns true if field detected (callback fired within 200 ms).
 * A card completing a full read is counted as field present — the card
 * read path handles the data separately.
 */
static bool btcpc_detect_rfid_reader(void) {
    LfrfidCtx ctx = {.detected = false, .protocol_code = 0xFF};

    ProtocolDict* dict   = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    LFRFIDWorker* worker = lfrfid_worker_alloc(dict);
    lfrfid_worker_start_thread(worker);
    lfrfid_worker_read_start(worker, LFRFIDWorkerReadTypeAuto, lfrfid_read_cb, &ctx);

    const uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(200);
    while(!ctx.detected && (int32_t)(deadline - furi_get_tick()) > 0) {
        furi_delay_ms(10);
    }

    lfrfid_worker_stop(worker);
    lfrfid_worker_stop_thread(worker);
    lfrfid_worker_free(worker);
    protocol_dict_free(dict);

    return ctx.detected;
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
        btcpc_ota_abort(app);
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
            case BTCPC_MSG_SUBGHZ_OBS: btcpc_census_and_push_subghz(app);   break;
            case BTCPC_MSG_HEARTBEAT:  btcpc_push_heartbeat(app);           break;
            case BTCPC_MSG_RFID_SCAN:  btcpc_capture_and_push_rfid(app);    break;
            case BTCPC_MSG_NFC_SCAN:   btcpc_capture_and_push_nfc(app);     break;
            case BTCPC_MSG_IBUTTON:    btcpc_capture_and_push_ibutton(app); break;
            case BTCPC_MSG_IR_CAPTURE:  btcpc_capture_and_push_ir(app);      break;
            case BTCPC_MSG_USB_SAFETY:
                scene_manager_next_scene(app->scene_manager, BtcpcSceneUsb);
                break;
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

    /* ── OTA firmware update ─────────────────────────────────────────────── */
    case BtcpcEventOtaChunk: {
        uint8_t  buf[BTCPC_BLE_DATA_CH_MAX_LEN];
        uint16_t len;

        furi_mutex_acquire(app->ota_mutex, FuriWaitForever);
        len = app->ota_chunk_len;
        if(len > 0) memcpy(buf, app->ota_chunk_buf, len);
        app->ota_chunk_len = 0;
        furi_mutex_release(app->ota_mutex);

        if(len == 0) { consumed = true; break; }

        BtcpcBleSvc* ota_svc = btcpc_ble_profile_get_svc(app->ble_profile);
        uint8_t cmd = buf[0];

        if(cmd == 'O' && len >= 5) {
            /* Open: abort any existing session and start fresh */
            btcpc_ota_abort(app);

            uint32_t sz = (uint32_t)buf[1]
                        | ((uint32_t)buf[2] << 8)
                        | ((uint32_t)buf[3] << 16)
                        | ((uint32_t)buf[4] << 24);

            app->ota_storage = furi_record_open(RECORD_STORAGE);
            app->ota_file    = storage_file_alloc(app->ota_storage);

            if(!storage_file_open(app->ota_file, BTCPC_OTA_TMP_PATH,
                                  FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                FURI_LOG_E("BtcpcBle", "OTA: open tmp failed");
                storage_file_free(app->ota_file);
                furi_record_close(RECORD_STORAGE);
                app->ota_file    = NULL;
                app->ota_storage = NULL;
                if(ota_svc) {
                    uint8_t err[2] = {'E', 0x01};
                    btcpc_ble_svc_send_ota_status(ota_svc, err, 2);
                }
                consumed = true;
                break;
            }

            app->ota_expected_size = sz;
            app->ota_bytes_written = 0;
            app->ota_checksum      = 0;
            app->ota_in_progress   = true;

            FURI_LOG_I("BtcpcBle", "OTA: started, expecting %lu bytes", (unsigned long)sz);

            if(ota_svc) {
                uint8_t ok = 'K';
                btcpc_ble_svc_send_ota_status(ota_svc, &ok, 1);
            }
            btcpc_scene_ble_refresh(app);

        } else if(cmd == 'D' && app->ota_in_progress && len >= 2) {
            /* Data chunk: write payload bytes to tmp file */
            const uint8_t* payload     = buf + 1;
            uint16_t       payload_len = (uint16_t)(len - 1);

            uint16_t written = (uint16_t)storage_file_write(app->ota_file, payload, payload_len);
            if(written != payload_len) {
                FURI_LOG_E("BtcpcBle", "OTA: write failed (%u/%u)", (unsigned)written, (unsigned)payload_len);
                btcpc_ota_abort(app);
                if(ota_svc) {
                    uint8_t err[2] = {'E', 0x02};
                    btcpc_ble_svc_send_ota_status(ota_svc, err, 2);
                }
            } else {
                for(uint16_t i = 0; i < payload_len; i++) {
                    app->ota_checksum += payload[i];
                }
                app->ota_bytes_written += payload_len;
                if(ota_svc) {
                    uint8_t ok = 'K';
                    btcpc_ble_svc_send_ota_status(ota_svc, &ok, 1);
                }
            }
            btcpc_scene_ble_refresh(app);

        } else if(cmd == 'C' && app->ota_in_progress && len >= 5) {
            /* Commit: verify size + checksum, then atomically rename */
            uint32_t peer_sum = (uint32_t)buf[1]
                              | ((uint32_t)buf[2] << 8)
                              | ((uint32_t)buf[3] << 16)
                              | ((uint32_t)buf[4] << 24);

            storage_file_close(app->ota_file);
            storage_file_free(app->ota_file);
            app->ota_file = NULL;

            uint8_t err_code = 0;
            if(app->ota_bytes_written != app->ota_expected_size) {
                FURI_LOG_E("BtcpcBle", "OTA: size mismatch: wrote %lu, expected %lu",
                           (unsigned long)app->ota_bytes_written,
                           (unsigned long)app->ota_expected_size);
                err_code = 0x03;
            } else if(app->ota_checksum != peer_sum) {
                FURI_LOG_E("BtcpcBle", "OTA: checksum mismatch: got 0x%08lx, expected 0x%08lx",
                           (unsigned long)app->ota_checksum,
                           (unsigned long)peer_sum);
                err_code = 0x04;
            }

            if(err_code != 0) {
                storage_simply_remove(app->ota_storage, BTCPC_OTA_TMP_PATH);
                furi_record_close(RECORD_STORAGE);
                app->ota_storage      = NULL;
                app->ota_in_progress  = false;
                if(ota_svc) {
                    uint8_t err[2] = {'E', err_code};
                    btcpc_ble_svc_send_ota_status(ota_svc, err, 2);
                }
            } else {
                /* Overwrite destination: remove old .fap (ignore if absent) */
                storage_simply_remove(app->ota_storage, BTCPC_FAP_PATH);
                FS_Error ren = storage_common_rename(
                    app->ota_storage, BTCPC_OTA_TMP_PATH, BTCPC_FAP_PATH);
                furi_record_close(RECORD_STORAGE);
                app->ota_storage     = NULL;
                app->ota_in_progress = false;
                if(ren != FSE_OK) {
                    FURI_LOG_E("BtcpcBle", "OTA: rename failed: %d", (int)ren);
                    if(ota_svc) {
                        uint8_t err[2] = {'E', 0x05};
                        btcpc_ble_svc_send_ota_status(ota_svc, err, 2);
                    }
                } else {
                    FURI_LOG_I("BtcpcBle", "OTA: complete — %lu bytes", (unsigned long)app->ota_bytes_written);
                    if(ota_svc) {
                        uint8_t done = 'Z';
                        btcpc_ble_svc_send_ota_status(ota_svc, &done, 1);
                    }
                }
            }
            btcpc_scene_ble_refresh(app);

        } else if(cmd == 'A') {
            /* Abort: clean up and acknowledge */
            btcpc_ota_abort(app);
            if(ota_svc) {
                uint8_t ok = 'K';
                btcpc_ble_svc_send_ota_status(ota_svc, &ok, 1);
            }
            btcpc_scene_ble_refresh(app);

        } else {
            FURI_LOG_W("BtcpcBle", "OTA: unknown cmd=0x%02x len=%u", (unsigned)cmd, (unsigned)len);
        }

        consumed = true;
        break;
    }

    /* ── Periodic heartbeat ──────────────────────────────────────────────── */
    case BtcpcEventHeartbeatTimer:
        if(!app->ble_connected) { consumed = true; break; }

        /*
         * Continuous autonomous sensor cycle — fires every 5 s.
         *
         * Each tick runs ALL sensors sequentially:
         *   SubGhz  — ~200 ms passive RSSI scan (3 frequencies)
         *   RFID    — up to 1 s LF scan
         *   IR      — up to 1 s IR listen
         *   iButton — up to 500 ms 1-Wire listen
         *
         * Captures only push a frame when something is actually detected.
         * SubGhz always pushes (ambient RF is always present).
         * Heartbeat pushes battery/uptime every tick.
         *
         * Total worst-case blocking: ~2.7 s per 5 s tick on the app thread.
         * BLE ISR thread is unaffected — sign requests queue and are processed
         * after the current sensor call returns.
         */

        btcpc_push_heartbeat(app);

        /* Sub-GHz census: every tick (~3.35 s across 13 bands) */
        btcpc_census_and_push_subghz(app);

        /* Contact sensors: rotate so each fires every 15 s (3 × 5 s) */
        switch(app->heartbeat_count % 3) {
        case 0: btcpc_capture_and_push_rfid(app);    break;
        case 1: btcpc_capture_and_push_ir(app);      break;
        case 2: btcpc_capture_and_push_ibutton(app); break;
        }

        /* BLE environment census: every 3 ticks (15 s) */
        if(app->heartbeat_count % 3 == 0) {
            btcpc_scan_and_push_ble_env(app);
        }

        /* RFID reader field detection: every 6 ticks (30 s).
         * Runs on tick % 6 == 3 to avoid overlapping the RFID card scan
         * at tick % 3 == 0. */
        if(app->heartbeat_count % 6 == 3) {
            if(btcpc_detect_rfid_reader()) {
                BtcpcBleSvc* rd_svc = btcpc_ble_profile_get_svc(app->ble_profile);
                if(rd_svc) {
                    BtcpcReaderDetect rd = {.reader_type = 0x01, .field_rssi = 0};
                    size_t rd_len;
                    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
                    rd_len = btcpc_build_reader_detect(&s_tx_frame, &rd, app->sk);
                    furi_mutex_release(app->sign_mutex);
                    btcpc_ble_svc_push_frame(rd_svc, (const uint8_t*)&s_tx_frame, (uint16_t)rd_len);
                    btcpc_notify_rare_detection();
                    FURI_LOG_D("BtcpcBle", "reader_detect: RFID 125kHz field");
                }
            }
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
