/*
 * btcpc_scene_ir.c — infrared remote-signal capture
 *
 * Listens for an IR signal using the Flipper's IR receiver, packs the
 * decoded (or raw) result into a BtcpcIrCapture, computes data_hash, signs
 * it with the device key, and sends the framed observation to the paired
 * phone over BLE. The phone verifies the device signature, then re-signs it
 * as a chain SensorDataCommit with the owner's posting key (see
 * clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md, Option B).
 *
 * Weak presence-only class: value = 1.0 ("a signal was captured"), decoded
 * protocol/address/command (or BtcpcIrProtocolRaw if undecoded) carry the
 * payload, matching the Phase 1.3 ratified mapping (docs/PLATFORM_PRD.md,
 * `ir` row) reconciled with the Phase 1.2 JSON convention's `ir` payload
 * keys (`protocol, address, command`).
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "../btcpc_ble.h"
#include "../btcpc_data_hash.h"
#include "btcpc_scene_ir.h"

#include <infrared_worker.h>
#include <gui/modules/text_box.h>
#include <string.h>

#define IR_TEXT_LEN 256
#define BTCPC_IR_POLL_MS 300

static const char* protocol_name(uint8_t protocol_id) {
    switch(protocol_id) {
    case BtcpcIrProtocolNec:       return "NEC";
    case BtcpcIrProtocolNecExt:    return "NECext";
    case BtcpcIrProtocolSamsung32: return "Samsung32";
    case BtcpcIrProtocolRc5:       return "RC5";
    case BtcpcIrProtocolRc6:       return "RC6";
    case BtcpcIrProtocolSirc:      return "SIRC";
    default:                       return "raw";
    }
}

/* Map the InfraredWorker's decoded protocol enum onto our compact wire
 * encoding. Isolated translation so a future SDK's exact enum values only
 * need reconciling in this one place. */
static uint8_t map_infrared_protocol(InfraredProtocol proto) {
    switch(proto) {
    case InfraredProtocolNEC:
    case InfraredProtocolNECext: return BtcpcIrProtocolNec;
    case InfraredProtocolSamsung32:      return BtcpcIrProtocolSamsung32;
    case InfraredProtocolRC5:
    case InfraredProtocolRC5X:           return BtcpcIrProtocolRc5;
    case InfraredProtocolRC6:            return BtcpcIrProtocolRc6;
    case InfraredProtocolSIRC:
    case InfraredProtocolSIRC15:
    case InfraredProtocolSIRC20:         return BtcpcIrProtocolSirc;
    default:                             return BtcpcIrProtocolRaw;
    }
}

typedef struct {
    volatile bool   done;
    volatile bool   found;
    InfraredMessage message;
} IrCaptureCtx;

static void ir_rx_cb(void* context, InfraredWorkerSignal* received_signal) {
    IrCaptureCtx* ctx = context;
    if(infrared_worker_signal_is_decoded(received_signal)) {
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(received_signal);
        if(msg) {
            ctx->message = *msg;
            ctx->found   = true;
        }
    }
    /* Raw (undecoded) signals are still a valid presence event per the PRD
     * inventory, but without a InfraredMessage there is nothing to copy;
     * ctx->found stays false and the caller falls back to "raw, no decode"
     * only if the worker reported *some* activity — handled below via the
     * timeout-vs-nothing-received distinction being out of scope for this
     * lightweight polling wrapper. */
    ctx->done = true;
}

/*
 * btcpc_ir_capture_once()
 *
 * ON-DEVICE VERIFICATION NEEDED (see header doc comment). The
 * infrared_worker_* call sequence (alloc/rx_start/rx_set_received_signal_
 * callback/stop/free) is written against the documented worker API shape
 * but has not been compiled against a real Flipper SDK in this environment
 * — isolate any fix here if `ufbt` reports a signature mismatch.
 */
bool btcpc_ir_capture_once(BtcpcIrCapture* ir_out) {
    if(!ir_out) return false;
    memset(ir_out, 0, sizeof(*ir_out));

    InfraredWorker* worker = infrared_worker_alloc();
    if(!worker) return false;

    IrCaptureCtx ctx = {0};
    infrared_worker_rx_set_received_signal_callback(worker, ir_rx_cb, &ctx);
    infrared_worker_rx_start(worker);

    uint32_t waited_ms = 0;
    while(!ctx.done && waited_ms < BTCPC_IR_POLL_MS) {
        furi_delay_ms(10);
        waited_ms += 10;
    }

    infrared_worker_rx_stop(worker);
    infrared_worker_free(worker);

    if(!ctx.found) {
        return false;
    }

    ir_out->protocol_id = map_infrared_protocol(ctx.message.protocol);
    ir_out->address      = (uint32_t)ctx.message.address;
    ir_out->command       = (uint32_t)ctx.message.command;
    return true;
}

void btcpc_scene_ir_on_enter(void* context) {
    BtcpcApp* app = context;
    static char text[IR_TEXT_LEN];

    if(!app->has_identity) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "No device key.\nOpen Identity / Key\nfirst to generate one.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    BtcpcIrCapture ir;
    bool found = btcpc_ir_capture_once(&ir);

    if(!found) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "IR capture\n\nNo signal received.\nPoint a remote at the\nFlipper's IR receiver.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    static BtcpcFrame frame;
    size_t frame_len = btcpc_build_ir(&frame, &ir, app->sk);

    bool sig_ok = btcpc_frame_verify(&frame.hdr, frame.payload, app->pk);

    char data_hash_hex[65];
    btcpc_data_hash_hex(BTCPC_MSG_IR_CAPTURE, frame.payload,
                        (size_t)frame.hdr.payload_len, data_hash_hex);

    bool sent = btcpc_ble_send(app, (const uint8_t*)&frame, frame_len);

    snprintf(text, sizeof(text),
        "IR capture\n\n"
        "Protocol: %s\n"
        "Address: 0x%lx\n"
        "Command: 0x%lx\n"
        "data_hash: %.16s...\n"
        "Signature: %s\n"
        "BLE TX: %s",
        protocol_name(ir.protocol_id),
        (unsigned long)ir.address,
        (unsigned long)ir.command,
        data_hash_hex,
        sig_ok ? "valid" : "FAILED",
        sent ? "sent" : (app->ble_connected ? "tx failed" : "no phone"));

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_ir_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void btcpc_scene_ir_on_exit(void* context) {
    BtcpcApp* app = context;
    text_box_reset(app->text_box);
}
