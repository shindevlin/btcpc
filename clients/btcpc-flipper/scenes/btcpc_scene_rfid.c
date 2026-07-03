/*
 * btcpc_scene_rfid.c — 125 kHz RFID (EM4100 / HID Prox / Indala) presence-scan
 *
 * Reads a low-frequency card using the Flipper's `lfrfid` worker, packs the
 * result into a BtcpcRfidScan, computes data_hash, signs it with the device
 * key, and sends the framed observation to the paired phone over BLE. The
 * phone verifies the device signature, then re-signs it as a chain
 * SensorDataCommit with the owner's posting key (see
 * clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md, Option B).
 *
 * Presence-only class: value = 1.0 ("a card was read"), identity fields
 * (protocol, card_id) carry the payload, matching the Phase 1.3 ratified
 * mapping (docs/PLATFORM_PRD.md, `rfid125` row).
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "../btcpc_ble.h"
#include "../btcpc_data_hash.h"
#include "btcpc_scene_rfid.h"

#include <lfrfid/lfrfid_worker.h>
#include <gui/modules/text_box.h>
#include <string.h>

#define RFID_TEXT_LEN 256
/* Short poll window per scheduler cycle, mirrors the sub-GHz/NFC pattern. */
#define BTCPC_RFID_POLL_MS 200

static const char* protocol_name(uint8_t protocol_id) {
    switch(protocol_id) {
    case 0: return "EM4100";
    case 1: return "HID";
    case 2: return "Indala";
    default: return "raw";
    }
}

/* Map LFRFIDProtocol (the worker's decoded protocol enum) onto our compact
 * wire encoding (0=EM4100, 1=HID, 2=Indala, 0xFF=raw/unknown). Kept as a
 * small isolated translation so a future SDK's exact enum values only need
 * to be reconciled in this one place. */
static uint8_t map_lfrfid_protocol(LFRFIDProtocol proto) {
    switch(proto) {
    case LFRFIDProtocolEM4100:  return 0;
    case LFRFIDProtocolHIDGeneric: return 1;
    case LFRFIDProtocolIndala26: return 2;
    default: return 0xFF;
    }
}

typedef struct {
    volatile bool     done;
    volatile bool     found;
    LFRFIDWorkerReadResult result;
} RfidReadCtx;

static void rfid_read_cb(LFRFIDWorkerReadResult result, void* context) {
    RfidReadCtx* ctx = context;
    if(result == LFRFIDWorkerReadDone) {
        ctx->found = true;
    }
    ctx->result = result;
    ctx->done = true;
}

/*
 * btcpc_rfid_read_once()
 *
 * ON-DEVICE VERIFICATION NEEDED (see header doc comment). The
 * lfrfid_worker_* call sequence (alloc/start/read_start/stop/free, the
 * exact ProtocolDict access pattern for the decoded card ID bytes) is
 * written against the documented worker API shape but has not been
 * compiled against a real Flipper SDK in this environment — isolate any
 * fix here if `ufbt` reports a signature mismatch.
 */
bool btcpc_rfid_read_once(BtcpcRfidScan* scan_out) {
    if(!scan_out) return false;
    memset(scan_out, 0, sizeof(*scan_out));

    LFRFIDWorker* worker = lfrfid_worker_alloc();
    if(!worker) return false;

    RfidReadCtx ctx = {0};
    lfrfid_worker_start_thread(worker);
    lfrfid_worker_read_start(worker, LFRFIDWorkerReadTypeAuto, rfid_read_cb, &ctx);

    /* Bounded synchronous wait for the callback — the worker runs on its own
     * thread, so this just polls a flag it sets. Keeps this function's
     * contract synchronous like the sub-GHz/NFC sampling helpers, which the
     * rotation scheduler relies on. */
    uint32_t waited_ms = 0;
    while(!ctx.done && waited_ms < BTCPC_RFID_POLL_MS) {
        furi_delay_ms(10);
        waited_ms += 10;
    }

    lfrfid_worker_stop(worker);
    lfrfid_worker_stop_thread(worker);

    bool ok = false;
    if(ctx.found) {
        ProtocolDict* dict = lfrfid_worker_get_read_data(worker);
        if(dict) {
            LFRFIDProtocol proto = protocol_dict_get_read_data_protocol(dict);
            uint8_t raw[LFRFID_PROTOCOL_MAX_DATA_SIZE];
            size_t  raw_len = protocol_dict_get_data(dict, proto, raw, sizeof(raw));

            scan_out->protocol = map_lfrfid_protocol(proto);
            uint8_t copy_len = (uint8_t)(raw_len > sizeof(scan_out->id) ?
                                          sizeof(scan_out->id) : raw_len);
            memcpy(scan_out->id, raw, copy_len);
            scan_out->id_len = copy_len;
            ok = true;
        }
    }

    lfrfid_worker_free(worker);
    return ok;
}

void btcpc_scene_rfid_on_enter(void* context) {
    BtcpcApp* app = context;
    static char text[RFID_TEXT_LEN];

    if(!app->has_identity) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "No device key.\nOpen Identity / Key\nfirst to generate one.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    BtcpcRfidScan scan;
    bool found = btcpc_rfid_read_once(&scan);

    if(!found) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "125kHz RFID scan\n\nNo card detected.\nHold a card near the\nback of the Flipper.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    static BtcpcFrame frame;
    size_t frame_len = btcpc_build_rfid(&frame, &scan, app->sk);

    bool sig_ok = btcpc_frame_verify(&frame.hdr, frame.payload, app->pk);

    char data_hash_hex[65];
    btcpc_data_hash_hex(BTCPC_MSG_RFID_SCAN, frame.payload,
                        (size_t)frame.hdr.payload_len, data_hash_hex);

    bool sent = btcpc_ble_send(app, (const uint8_t*)&frame, frame_len);

    snprintf(text, sizeof(text),
        "125kHz RFID scan\n\n"
        "Protocol: %s\n"
        "ID len: %u\n"
        "data_hash: %.16s...\n"
        "Signature: %s\n"
        "BLE TX: %s",
        protocol_name(scan.protocol),
        (unsigned)scan.id_len,
        data_hash_hex,
        sig_ok ? "valid" : "FAILED",
        sent ? "sent" : (app->ble_connected ? "tx failed" : "no phone"));

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_rfid_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void btcpc_scene_rfid_on_exit(void* context) {
    BtcpcApp* app = context;
    text_box_reset(app->text_box);
}
