/*
 * btcpc_scene_ble.c — BLE signing service scene
 *
 * Displays the current BLE connection state. When the user is on this
 * screen the app is advertising and ready to accept a phone connection.
 *
 * Handles three custom events posted from background threads:
 *
 *   BtcpcEventSignRequest
 *     Posted by btcpc_ble_sign_req_cb() when the phone writes a 32-byte
 *     hash to the SIGN_REQUEST characteristic. This scene acquires the
 *     sign_mutex, runs ed25519 sign, clears sign_pending, releases the
 *     mutex, then sends the 64-byte signature via notification.
 *
 *   BtcpcEventBleConnected
 *     Posted by the GAP status callback on connection. Refreshes display.
 *
 *   BtcpcEventBleDisconnected
 *     Posted by the GAP status callback on disconnection. Refreshes display.
 *
 * Shin Devlin — btcpc.network
 */

#include "../btcpc.h"
#include "btcpc_scene_ble.h"

#include <gui/modules/text_box.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

#define BLE_TEXT_LEN 160

/* Refresh the text_box with the current BLE status */
static void btcpc_scene_ble_refresh(BtcpcApp* app) {
    static char ble_text[BLE_TEXT_LEN];

    if(app->ble_connected) {
        snprintf(ble_text, sizeof(ble_text),
                 "BLE: Connected\n\n"
                 "Ready to sign.\n"
                 "Send hash from\n"
                 "BTCPC app.");
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

void btcpc_scene_ble_on_enter(void* context) {
    BtcpcApp* app = context;
    btcpc_scene_ble_refresh(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, BtcpcViewTextBox);
}

bool btcpc_scene_ble_on_event(void* context, SceneManagerEvent event) {
    BtcpcApp* app = context;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) {
        return false;
    }

    switch((BtcpcCustomEvent)event.event) {

    case BtcpcEventSignRequest: {
        /*
         * Sign the pending hash on the app thread.
         * sk[] is only touched here and in btcpc_identity_load_or_create().
         * The BLE ISR thread never touches sk[].
         */
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
        /* Sign with sk[] — takes ~50–150 ms on STM32WB55 */
        btcpc_ed25519_sign(sig, hash_copy, BTCPC_BLE_SIGN_REQ_LEN, app->sk);
        furi_mutex_release(app->sign_mutex);

        /* Send notification — BLE GATT update on app thread is safe */
        BtcpcBleSvc* svc = btcpc_ble_profile_get_svc(app->ble_profile);
        if(svc) {
            bool sent = btcpc_ble_svc_send_signature(svc, sig);
            FURI_LOG_D("BtcpcBle", "signature sent: %s", sent ? "ok" : "fail");
        } else {
            FURI_LOG_W("BtcpcBle", "sign response: no active GATT service");
        }

        /* Wipe local sig and hash copies — not strictly required but good hygiene */
        memset(sig,       0, sizeof(sig));
        memset(hash_copy, 0, sizeof(hash_copy));

        consumed = true;
        break;
    }

    case BtcpcEventBleConnected:
    case BtcpcEventBleDisconnected:
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
    /* Keep advertising and the GATT service running when navigating away —
     * the signing service must stay active for the phone to use it. */
}
