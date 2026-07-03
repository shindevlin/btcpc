/* btcpc_scene_usb.c — USB safety scan scene
 * Shin Devlin — btcpc.network
 *
 * Monitors the USB connection for juice jacking attacks. Triggered by the
 * phone sending a SENSOR_REQ frame with sensor_type=0x0B (BTCPC_MSG_USB_SAFETY).
 * Once dispatched by btcpc_scene_ble.c (pending merge), the scene runs a
 * 10-second USB state poll and pushes a signed BtcpcUsbSafetyResult frame back.
 *
 * USB detection strategy:
 *   - VBUS presence:  furi_hal_power_get_usb_voltage() > 4.0 V
 *   - Enumeration:    event-driven via furi_hal_usb_set_state_callback()
 *       FuriHalUsbStateEventReset            — host is driving D+/D- lines
 *       FuriHalUsbStateEventDescriptorRequest — host actively enumerating
 *   Power-only chargers generate neither event. furi_hal_usb_get_config()
 *   is intentionally NOT used for detection — it returns Flipper's own
 *   configured interface, not what the external host is doing.
 *
 * Frame encoding:
 *   msg_type  = 0x0B  (BTCPC_MSG_USB_SAFETY — to be added to enum by merge)
 *   payload   = BtcpcUsbSafetyResult (5 bytes, packed)
 *   signature = ed25519 over payload using app->sk
 *
 * Custom event:
 *   110 (BtcpcEventUsbSafetyTick) — raw integer; add to BtcpcCustomEvent enum
 *   in btcpc.h once the other agent is done with that file.
 *   See scenes/MERGE_NOTES_USB.md for all required merge steps.
 */

#include "../btcpc.h"
#include "btcpc_scene_usb.h"

#include <gui/modules/text_box.h>
#include <furi.h>
#include <furi_hal_power.h>
#include <furi_hal_usb.h>
#include <notification/notification_messages.h>

#include "../protocol/btcpc_protocol.h"
#include "../ble/btcpc_ble_svc.h"
#include "../ble/btcpc_ble_profile.h"
#include "../crypto/ed25519.h"

#include <string.h>
#include <stdio.h>

/* ─── Constants ─────────────────────────────────────────────────────────── */

#define USB_SCAN_TICKS       10     /* 10-second monitoring window */
#define USB_VBUS_THRESHOLD_V 4.0f  /* VBUS present when voltage exceeds 4.0 V */
#define USB_TEXT_LEN         128

/*
 * BtcpcEventUsbSafetyTick = 110
 * Raw value used because btcpc.h cannot be modified during this agent's run.
 * Merge: add BtcpcEventUsbSafetyTick = 110 to BtcpcCustomEvent enum in btcpc.h.
 */
#define BTCPC_EVENT_USB_SAFETY_TICK 110

/* ─── Module-level state ─────────────────────────────────────────────────── */

/*
 * All mutable state lives here — cannot add fields to BtcpcApp during this run.
 * Follows the s_tx_frame precedent in btcpc_scene_ble.c.
 */
static struct {
    FuriTimer* timer;
    uint8_t    tick_count;
    bool       vbus_present;
    bool       enumeration_seen;
    uint8_t    enumeration_count;
    uint8_t    usb_class;
    uint8_t    verdict;          /* set after 10 ticks */
    bool       scan_done;
    char       text[USB_TEXT_LEN];
} s_usb;

/* Reuse-once TX frame — same pattern as btcpc_scene_ble.c's s_tx_frame. */
static BtcpcFrame s_usb_tx_frame;

/* ─── USB state callback (fires from USB IRQ context) ───────────────────── */

/*
 * btcpc_usb_state_cb()
 *
 * Registered via furi_hal_usb_set_state_callback() during the scan window.
 * Called by the USB HAL when the host changes bus state.
 *
 * FuriHalUsbStateEventReset fires when the host asserts a bus reset.
 * Power-only chargers (or open-circuit D+/D-) never generate a bus reset.
 * Any charger that is actually a data-capable host will issue a Reset before
 * descriptor enumeration.
 *
 * FuriHalUsbStateEventDescriptorRequest fires when the host requests device
 * descriptors — this is the definitive sign of an active data-capable port.
 *
 * We must not call furi_hal_usb_* or access app state from this callback
 * beyond setting the single volatile flag — it runs in IRQ context.
 *
 * The `context` pointer is unused here because s_usb is module-static; we
 * do not dereference app from IRQ context to avoid race conditions.
 */
static void btcpc_usb_state_cb(FuriHalUsbStateEvent state, void* context) {
    UNUSED(context);
    switch(state) {
    case FuriHalUsbStateEventReset:
        /*
         * Bus reset from external host — definitive: a data-capable port is
         * driving the D+ / D- lines. Update enumeration count and set class.
         * s_usb.usb_class = 0xFF signals "host-initiated reset, class unknown".
         * The DescriptorRequest event will follow if enumeration proceeds.
         */
        s_usb.enumeration_seen = true;
        s_usb.enumeration_count++;
        if(s_usb.usb_class == 0x00) s_usb.usb_class = 0xFF;
        break;
    case FuriHalUsbStateEventDescriptorRequest:
        /*
         * Host asked for descriptors — full enumeration attempt. This is the
         * strongest indicator: a power-only charger can never reach this state.
         * usb_class 0x02 = "CDC/data" — placeholder to signal "host enumerated".
         * A system-privileged service could read the actual class from the
         * USB peripheral registers; not available to FAPs.
         */
        s_usb.enumeration_seen = true;
        s_usb.enumeration_count++;
        s_usb.usb_class = 0x02;
        break;
    default:
        break;
    }
}

/* ─── USB state helpers ──────────────────────────────────────────────────── */

/*
 * btcpc_usb_vbus_present()
 *
 * Returns true when USB VBUS voltage exceeds USB_VBUS_THRESHOLD_V.
 * furi_hal_power_get_usb_voltage() is FAP-accessible via furi_hal_power.h.
 */
static bool btcpc_usb_vbus_present(void) {
    float v = furi_hal_power_get_usb_voltage();
    return v >= USB_VBUS_THRESHOLD_V;
}

/* ─── Display helpers ────────────────────────────────────────────────────── */

static void btcpc_usb_refresh(BtcpcApp* app) {
    text_box_reset(app->text_box);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, s_usb.text);
}

static void btcpc_usb_show_scanning(BtcpcApp* app) {
    uint8_t remaining = (uint8_t)(USB_SCAN_TICKS - s_usb.tick_count);
    snprintf(s_usb.text, sizeof(s_usb.text),
             "USB SAFETY SCAN\n"
             "\n"
             "Monitoring USB...\n"
             "[%us remaining]\n"
             "\n"
             "VBUS: %s\n"
             "Data lines: %s",
             (unsigned)remaining,
             s_usb.vbus_present ? "present" : "absent",
             s_usb.enumeration_seen ? "active" : "checking...");
    btcpc_usb_refresh(app);
}

static void btcpc_usb_show_result(BtcpcApp* app) {
    switch(s_usb.verdict) {
    case 0:
        snprintf(s_usb.text, sizeof(s_usb.text),
                 "RESULT: SAFE\n"
                 "\n"
                 "Power-only charger.\n"
                 "No data activity\n"
                 "detected in 10s.");
        break;
    case 1:
        snprintf(s_usb.text, sizeof(s_usb.text),
                 "RESULT: WARNING\n"
                 "\n"
                 "Data lines active.\n"
                 "USB enumeration\n"
                 "detected. Unplug.");
        break;
    case 2:
        snprintf(s_usb.text, sizeof(s_usb.text),
                 "RESULT: ATTACK\n"
                 "\n"
                 "Active USB injection\n"
                 "detected. Unplug\n"
                 "immediately.");
        break;
    default:
        snprintf(s_usb.text, sizeof(s_usb.text), "RESULT: UNKNOWN\n\nScan incomplete.");
        break;
    }
    btcpc_usb_refresh(app);
}

/* ─── Verdict notification (haptic + LED) ───────────────────────────────── */

static void btcpc_usb_notify_result(uint8_t verdict) {
    NotificationApp* notif = furi_record_open(RECORD_NOTIFICATION);
    switch(verdict) {
    case 0: /* SAFE — green blink */
        notification_message(notif, &sequence_blink_green_100);
        break;
    case 1: /* WARNING — yellow blink + short vibration */
        notification_message(notif, &sequence_set_vibro_on);
        furi_delay_ms(100);
        notification_message(notif, &sequence_reset_vibro);
        notification_message(notif, &sequence_blink_yellow_100);
        break;
    case 2: /* ATTACK — red blink + long vibration */
        notification_message(notif, &sequence_set_vibro_on);
        furi_delay_ms(500);
        notification_message(notif, &sequence_reset_vibro);
        notification_message(notif, &sequence_blink_red_100);
        break;
    default:
        break;
    }
    furi_record_close(RECORD_NOTIFICATION);
}

/* ─── Frame builder ──────────────────────────────────────────────────────── */

/*
 * btcpc_usb_push_result()
 *
 * Builds and pushes a signed USB safety result frame via DATA_CHANNEL.
 *
 * Frame format:
 *   magic[4]      = {'B','T','P','C'}
 *   msg_type      = 0x0B  (BTCPC_MSG_USB_SAFETY — pending enum addition)
 *   payload_len   = sizeof(BtcpcUsbSafetyResult)
 *   sig[64]       = ed25519 over payload using app->sk
 *   payload[5]    = BtcpcUsbSafetyResult (packed)
 *
 * Pattern mirrors btcpc_build_rfid / btcpc_build_heartbeat in btcpc_protocol.c.
 * A proper btcpc_build_usb_safety() builder will be added to btcpc_protocol.c
 * once the enum is updated — see MERGE_NOTES_USB.md.
 */
static void btcpc_usb_push_result(BtcpcApp* app) {
    BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
    if(!svc) return;

    BtcpcUsbSafetyResult result;
    result.verdict           = s_usb.verdict;
    result.usb_class         = s_usb.usb_class;
    result.enumeration_count = s_usb.enumeration_count;
    result.monitor_ms        = (uint16_t)(USB_SCAN_TICKS * 1000u);

    /* Build frame manually — btcpc_build_usb_safety() is pending merge into
     * btcpc_protocol.c; use the same pattern as the existing builders. */
    static const uint8_t frame_magic[BTCPC_MAGIC_LEN] = BTCPC_FRAME_MAGIC;
    memcpy(s_usb_tx_frame.hdr.magic, frame_magic, BTCPC_MAGIC_LEN);
    s_usb_tx_frame.hdr.msg_type    = 0x0Bu;
    s_usb_tx_frame.hdr.payload_len = (uint16_t)sizeof(BtcpcUsbSafetyResult);
    memset(s_usb_tx_frame.hdr.sig, 0, BTCPC_SIG_LEN);
    memcpy(s_usb_tx_frame.payload, &result, sizeof(result));

    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    btcpc_ed25519_sign(
        s_usb_tx_frame.hdr.sig,
        s_usb_tx_frame.payload,
        s_usb_tx_frame.hdr.payload_len,
        app->sk);
    furi_mutex_release(app->sign_mutex);

    uint16_t total = (uint16_t)(BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcUsbSafetyResult));
    btcpc_ble_svc_push_frame(svc, (const uint8_t*)&s_usb_tx_frame, total);

    FURI_LOG_I("BtcpcUsb",
               "usb_safety: verdict=%u class=0x%02x enum_count=%u",
               (unsigned)result.verdict,
               (unsigned)result.usb_class,
               (unsigned)result.enumeration_count);
}

/* ─── Timer callback ─────────────────────────────────────────────────────── */

/*
 * btcpc_usb_timer_cb()
 *
 * Fires from the timer thread every 1000 ms. Posts custom event 110
 * (BtcpcEventUsbSafetyTick) to wake the app thread for state sampling.
 * Must not block, must not access hardware directly.
 */
static void btcpc_usb_timer_cb(void* context) {
    BtcpcApp* app = context;
    /* Raw 110 = BtcpcEventUsbSafetyTick (add to enum on merge — see MERGE_NOTES_USB.md) */
    view_dispatcher_send_custom_event(app->view_dispatcher, 110);
}

/* ─── Scene lifecycle ────────────────────────────────────────────────────── */

void btcpc_scene_usb_on_enter(void* context) {
    BtcpcApp* app = context;

    /* Zero module state */
    memset(&s_usb, 0, sizeof(s_usb));

    snprintf(s_usb.text, sizeof(s_usb.text),
             "USB SAFETY SCAN\n"
             "\n"
             "Monitoring USB...\n"
             "[10s remaining]\n"
             "\n"
             "VBUS: checking...\n"
             "Data lines: checking...");

    text_box_reset(app->text_box);
    text_box_set_font(app->text_box, TextBoxFontText);
    text_box_set_text(app->text_box, s_usb.text);

    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);

    /*
     * Register USB state callback BEFORE starting the timer so no Reset or
     * DescriptorRequest events are missed during the first tick window.
     * Unregistered in on_exit.
     */
    furi_hal_usb_set_state_callback(btcpc_usb_state_cb, app);

    /* Allocate one-shot periodic timer — freed in on_exit */
    s_usb.timer = furi_timer_alloc(btcpc_usb_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(s_usb.timer, furi_ms_to_ticks(1000));
}

bool btcpc_scene_usb_on_event(void* context, SceneManagerEvent event) {
    BtcpcApp* app = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false;

    /* Raw 110 = BtcpcEventUsbSafetyTick — see comment in btcpc_usb_timer_cb */
    if(event.event != 110) return false;

    consumed = true;

    if(s_usb.scan_done) return consumed;

    s_usb.tick_count++;

    /* Sample USB state on each tick */
    s_usb.vbus_present = btcpc_usb_vbus_present();

    if(!s_usb.vbus_present) {
        /* No VBUS — charger not connected or cable is power-only without VBUS */
        snprintf(s_usb.text, sizeof(s_usb.text),
                 "USB SAFETY SCAN\n"
                 "\n"
                 "Not connected.\n"
                 "No USB power\n"
                 "detected.");
        btcpc_usb_refresh(app);
        /* Continue polling — VBUS may appear after plug-in */
    }

    /*
     * USB enumeration state is updated exclusively by btcpc_usb_state_cb()
     * which fires from the USB HAL on FuriHalUsbStateEventReset and
     * FuriHalUsbStateEventDescriptorRequest. Those are the only reliable
     * indicators that a host is actively driving the data lines — polling
     * furi_hal_usb_get_config() returns Flipper's *own* configured interface
     * and cannot detect what an external host is doing.
     */

    if(s_usb.tick_count < USB_SCAN_TICKS) {
        /* Still within monitoring window — update display */
        btcpc_usb_show_scanning(app);
        return consumed;
    }

    /* 10 seconds elapsed — deliver verdict */
    furi_timer_stop(s_usb.timer);
    s_usb.scan_done = true;

    if(!s_usb.enumeration_seen) {
        /*
         * No bus Reset or DescriptorRequest fired during the 10-second window.
         * The port is power-only — the D+ / D- lines were never driven by a host.
         */
        s_usb.verdict = 0; /* SAFE */
    } else if(s_usb.usb_class == 0x02) {
        /*
         * FuriHalUsbStateEventDescriptorRequest fired — host completed descriptor
         * enumeration. Port is definitely data-capable. Rate as WARNING so the
         * user can disconnect; the phone app should offer chain reporting.
         */
        s_usb.verdict = 1; /* WARNING — full USB enumeration observed */
    } else {
        /*
         * FuriHalUsbStateEventReset fired (usb_class == 0xFF) but no
         * DescriptorRequest followed, or the bus was reset more than once
         * (repeated probing pattern). Treat repeated resets as an active
         * injection attempt.
         */
        s_usb.verdict = (s_usb.enumeration_count > 1) ? 2 : 1;
    }

    btcpc_usb_show_result(app);
    btcpc_usb_notify_result(s_usb.verdict);

    /* Push signed result frame to phone via DATA_CHANNEL */
    if(app->ble_connected) {
        btcpc_usb_push_result(app);
    }

    return consumed;
}

void btcpc_scene_usb_on_exit(void* context) {
    BtcpcApp* app = context;

    /* Unregister USB state callback before freeing the timer */
    furi_hal_usb_set_state_callback(NULL, NULL);

    if(s_usb.timer) {
        furi_timer_stop(s_usb.timer);
        furi_timer_free(s_usb.timer);
        s_usb.timer = NULL;
    }

    text_box_reset(app->text_box);
}
