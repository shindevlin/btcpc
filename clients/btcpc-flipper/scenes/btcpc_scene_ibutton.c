/*
 * btcpc_scene_ibutton.c — iButton (1-Wire/Dallas 64-bit ROM code) presence read
 *
 * Reads a Dallas/Maxim iButton over 1-Wire contact using the Flipper's
 * `ibutton` worker, packs the ROM code into a BtcpcIButton, computes
 * data_hash, signs it with the device key, and sends the framed observation
 * to the paired phone over BLE. The phone verifies the device signature,
 * then re-signs it as a chain SensorDataCommit with the owner's posting key
 * (see clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md, Option B).
 *
 * Presence-only class: value = 1.0 ("a device made contact"), identity
 * fields (rom_code, family) carry the payload, matching the Phase 1.3
 * ratified mapping (docs/PLATFORM_PRD.md, `ibutton` row).
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "../btcpc_ble.h"
#include "../btcpc_data_hash.h"
#include "btcpc_scene_ibutton.h"

#include <ibutton/ibutton_worker.h>
#include <gui/modules/text_box.h>
#include <string.h>

#define IBUTTON_TEXT_LEN 256
#define BTCPC_IBUTTON_POLL_MS 200

typedef struct {
    volatile bool done;
    volatile bool found;
    uint8_t       rom[8];
} IButtonReadCtx;

static void ibutton_read_cb(iButtonWorkerReadResult result, void* context) {
    IButtonReadCtx* ctx = context;
    if(result == iButtonWorkerReadOk) {
        ctx->found = true;
    }
    ctx->done = true;
}

/*
 * btcpc_ibutton_read_once()
 *
 * ON-DEVICE VERIFICATION NEEDED (see header doc comment). The
 * ibutton_worker_* call sequence (alloc/start_thread/read_start/stop/free
 * and how the read key's ROM bytes are pulled out after a successful read)
 * is written against the documented worker API shape but has not been
 * compiled against a real Flipper SDK in this environment — isolate any
 * fix here if `ufbt` reports a signature mismatch.
 */
bool btcpc_ibutton_read_once(BtcpcIButton* btn_out) {
    if(!btn_out) return false;
    memset(btn_out, 0, sizeof(*btn_out));

    iButtonWorker* worker = ibutton_worker_alloc();
    if(!worker) return false;

    IButtonReadCtx ctx = {0};
    ibutton_worker_start_thread(worker);
    ibutton_worker_read_start(worker, ibutton_read_cb, &ctx);

    uint32_t waited_ms = 0;
    while(!ctx.done && waited_ms < BTCPC_IBUTTON_POLL_MS) {
        furi_delay_ms(10);
        waited_ms += 10;
    }

    ibutton_worker_stop(worker);
    ibutton_worker_stop_thread(worker);

    bool ok = false;
    if(ctx.found) {
        iButtonKey* key = ibutton_worker_get_read_key(worker);
        if(key) {
            const uint8_t* rom = ibutton_key_get_data_p(key);
            memcpy(btn_out->id, rom, sizeof(btn_out->id));
            btn_out->family = btn_out->id[0];
            ok = true;
        }
    }

    ibutton_worker_free(worker);
    return ok;
}

void btcpc_scene_ibutton_on_enter(void* context) {
    BtcpcApp* app = context;
    static char text[IBUTTON_TEXT_LEN];

    if(!app->has_identity) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "No device key.\nOpen Identity / Key\nfirst to generate one.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    BtcpcIButton btn;
    bool found = btcpc_ibutton_read_once(&btn);

    if(!found) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "iButton read\n\nNo contact detected.\nTouch the probe to the\niButton/1-Wire pin.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    static BtcpcFrame frame;
    size_t frame_len = btcpc_build_ibutton(&frame, &btn, app->sk);

    bool sig_ok = btcpc_frame_verify(&frame.hdr, frame.payload, app->pk);

    char data_hash_hex[65];
    btcpc_data_hash_hex(BTCPC_MSG_IBUTTON, frame.payload,
                        (size_t)frame.hdr.payload_len, data_hash_hex);

    bool sent = btcpc_ble_send(app, (const uint8_t*)&frame, frame_len);

    snprintf(text, sizeof(text),
        "iButton read\n\n"
        "Family: 0x%02x\n"
        "ROM: %02x%02x%02x%02x%02x%02x%02x%02x\n"
        "data_hash: %.16s...\n"
        "Signature: %s\n"
        "BLE TX: %s",
        (unsigned)btn.family,
        btn.id[0], btn.id[1], btn.id[2], btn.id[3],
        btn.id[4], btn.id[5], btn.id[6], btn.id[7],
        data_hash_hex,
        sig_ok ? "valid" : "FAILED",
        sent ? "sent" : (app->ble_connected ? "tx failed" : "no phone"));

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_ibutton_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void btcpc_scene_ibutton_on_exit(void* context) {
    BtcpcApp* app = context;
    text_box_reset(app->text_box);
}
