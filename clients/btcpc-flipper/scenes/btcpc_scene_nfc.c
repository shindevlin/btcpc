/*
 * btcpc_scene_nfc.c — NFC (ISO14443 A/B/F/V) presence-scan capture
 *
 * Polls for a card in the field using the Flipper's ST25R3916 NFC front-end,
 * packs the result into a BtcpcNfcScan, computes data_hash, signs it with the
 * device key, and sends the framed observation to the paired phone over BLE.
 * The phone verifies the device signature, then re-signs it as a chain
 * SensorDataCommit with the owner's posting key (see
 * clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md, Option B).
 *
 * Presence-only class: value = 1.0 ("a card was read"), identity fields
 * (tech, uid, atqa, sak) carry the payload, matching the Phase 1.3 ratified
 * mapping (docs/PLATFORM_PRD.md, `nfc` row).
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "../btcpc_ble.h"
#include "../btcpc_data_hash.h"
#include "btcpc_scene_nfc.h"

#include <furi_hal_nfc.h>
#include <gui/modules/text_box.h>
#include <string.h>

#define NFC_TEXT_LEN 256
/* Short poll window: the auto-rotation scheduler calls this once per cycle
 * and must not block the UI/scene loop for long when no card is present. */
#define BTCPC_NFC_POLL_MS 150

/*
 * btcpc_nfc_poll_once()
 *
 * ON-DEVICE VERIFICATION NEEDED (see header doc comment). Structured so the
 * only thing that can be firmware-SDK-version-sensitive is the detect/poll
 * call itself; everything else here (mapping the result into BtcpcNfcScan)
 * is plain data marshalling.
 */
bool btcpc_nfc_poll_once(BtcpcNfcScan* scan_out) {
    if(!scan_out) return false;
    memset(scan_out, 0, sizeof(*scan_out));

    if(!furi_hal_nfc_is_hal_ready()) {
        return false;
    }

    FuriHalNfcDevData dev_data = {0};
    bool detected = furi_hal_nfc_detect(&dev_data, BTCPC_NFC_POLL_MS);
    if(!detected) {
        return false;
    }

    /* Map the detected device type to our compact tech enum (0=A,1=B,2=F,3=V).
     * furi_hal_nfc_detect() targets ISO14443-3A by default on most firmware
     * revisions; this defaults to tech A and copies whatever UID/ATQA/SAK the
     * HAL populated. If a future SDK exposes the technology explicitly, wire
     * it through here — the payload struct already reserves the field. */
    scan_out->tech = 0; /* A */

    uint8_t uid_len = dev_data.uid_len;
    if(uid_len > sizeof(scan_out->uid)) uid_len = sizeof(scan_out->uid);
    memcpy(scan_out->uid, dev_data.uid, uid_len);
    scan_out->uid_len = uid_len;

    scan_out->atqa[0] = dev_data.atqa[0];
    scan_out->atqa[1] = dev_data.atqa[1];
    scan_out->sak     = dev_data.sak;

    return true;
}

void btcpc_scene_nfc_on_enter(void* context) {
    BtcpcApp* app = context;
    static char text[NFC_TEXT_LEN];

    if(!app->has_identity) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "No device key.\nOpen Identity / Key\nfirst to generate one.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    BtcpcNfcScan scan;
    bool found = btcpc_nfc_poll_once(&scan);

    if(!found) {
        text_box_reset(app->text_box);
        text_box_set_text(app->text_box,
            "NFC scan\n\nNo card detected.\nHold a tag near the\nback of the Flipper.");
        view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
        return;
    }

    static BtcpcFrame frame;
    size_t frame_len = btcpc_build_nfc(&frame, &scan, app->sk);

    bool sig_ok = btcpc_frame_verify(&frame.hdr, frame.payload, app->pk);

    char data_hash_hex[65];
    btcpc_data_hash_hex(BTCPC_MSG_NFC_SCAN, frame.payload,
                        (size_t)frame.hdr.payload_len, data_hash_hex);

    bool sent = btcpc_ble_send(app, (const uint8_t*)&frame, frame_len);

    snprintf(text, sizeof(text),
        "NFC scan\n\n"
        "Tech: %c\n"
        "UID len: %u\n"
        "SAK: 0x%02x\n"
        "data_hash: %.16s...\n"
        "Signature: %s\n"
        "BLE TX: %s",
        "ABFV"[scan.tech & 3],
        (unsigned)scan.uid_len,
        (unsigned)scan.sak,
        data_hash_hex,
        sig_ok ? "valid" : "FAILED",
        sent ? "sent" : (app->ble_connected ? "tx failed" : "no phone"));

    text_box_reset(app->text_box);
    text_box_set_text(app->text_box, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_nfc_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void btcpc_scene_nfc_on_exit(void* context) {
    BtcpcApp* app = context;
    text_box_reset(app->text_box);
}
