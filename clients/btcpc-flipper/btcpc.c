/*
 * btcpc.c — BTCPC Flipper Zero identity node
 *
 * Entry point and application lifecycle. Manages the scene/view stack,
 * ed25519 identity, and BLE signing service lifecycle.
 *
 * Signing flow:
 *   1. Phone writes 32-byte hash to SIGN_REQUEST characteristic.
 *   2. btcpc_ble_sign_req_cb() fires on the BLE ISR thread:
 *        - acquires sign_mutex
 *        - copies hash into app->sign_pending_hash
 *        - sets app->sign_pending = true
 *        - releases sign_mutex
 *        - posts BtcpcEventSignRequest to the view dispatcher
 *   3. App thread receives the event in btcpc_scene_ble_on_event():
 *        - acquires sign_mutex
 *        - calls btcpc_ed25519_sign(sig, hash, 32, sk)
 *        - clears sign_pending
 *        - releases sign_mutex
 *        - calls btcpc_ble_svc_send_signature() to notify phone
 *
 * The private key (sk[]) never appears in any BLE callback or handler.
 *
 * Shin Devlin — btcpc.network
 */

#include "btcpc.h"
#include "scenes/btcpc_scene_main.h"
#include "scenes/btcpc_scene_identity.h"
#include "scenes/btcpc_scene_ble.h"

#include <furi.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>

/* ─── Scene handlers table ─────────────────────────────────────────────── */

static const AppSceneOnEnterCallback btcpc_on_enter_handlers[] = {
    [BtcpcSceneMain]     = btcpc_scene_main_on_enter,
    [BtcpcSceneIdentity] = btcpc_scene_identity_on_enter,
    [BtcpcSceneBle]      = btcpc_scene_ble_on_enter,
};

static const AppSceneOnEventCallback btcpc_on_event_handlers[] = {
    [BtcpcSceneMain]     = btcpc_scene_main_on_event,
    [BtcpcSceneIdentity] = btcpc_scene_identity_on_event,
    [BtcpcSceneBle]      = btcpc_scene_ble_on_event,
};

static const AppSceneOnExitCallback btcpc_on_exit_handlers[] = {
    [BtcpcSceneMain]     = btcpc_scene_main_on_exit,
    [BtcpcSceneIdentity] = btcpc_scene_identity_on_exit,
    [BtcpcSceneBle]      = btcpc_scene_ble_on_exit,
};

static const SceneManagerHandlers btcpc_scene_handlers = {
    .on_enter_handlers = btcpc_on_enter_handlers,
    .on_event_handlers = btcpc_on_event_handlers,
    .on_exit_handlers  = btcpc_on_exit_handlers,
    .scene_num         = BtcpcSceneCount,
};

/* ─── Custom event dispatcher ───────────────────────────────────────────── */

static bool btcpc_custom_event_callback(void* context, uint32_t event) {
    BtcpcApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool btcpc_back_event_callback(void* context) {
    BtcpcApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

/* ─── App alloc / free ──────────────────────────────────────────────────── */

BtcpcApp* btcpc_app_alloc(void) {
    BtcpcApp* app = malloc(sizeof(BtcpcApp));
    memset(app, 0, sizeof(BtcpcApp));

    app->sign_mutex     = furi_mutex_alloc(FuriMutexTypeNormal);
    app->data_rx_mutex  = furi_mutex_alloc(FuriMutexTypeNormal);
    app->heartbeat_timer = furi_timer_alloc(btcpc_heartbeat_timer_cb, FuriTimerTypePeriodic, app);

    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, btcpc_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, btcpc_back_event_callback);

    app->scene_manager = scene_manager_alloc(&btcpc_scene_handlers, app);

    /* Submenu */
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BtcpcViewSubmenu, submenu_get_view(app->submenu));

    /* Popup */
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BtcpcViewPopup, popup_get_view(app->popup));

    /* TextBox */
    app->text_box = text_box_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BtcpcViewTextBox, text_box_get_view(app->text_box));

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

void btcpc_app_free(BtcpcApp* app) {
    furi_assert(app);

    view_dispatcher_remove_view(app->view_dispatcher, BtcpcViewTextBox);
    view_dispatcher_remove_view(app->view_dispatcher, BtcpcViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, BtcpcViewSubmenu);

    text_box_free(app->text_box);
    popup_free(app->popup);
    submenu_free(app->submenu);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_timer_free(app->heartbeat_timer);
    furi_mutex_free(app->data_rx_mutex);
    furi_mutex_free(app->sign_mutex);

    furi_record_close(RECORD_GUI);
    free(app);
}

/* ─── Identity helpers ──────────────────────────────────────────────────── */

bool btcpc_identity_load_or_create(BtcpcApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    /* Ensure data directory exists */
    storage_simply_mkdir(storage, BTCPC_DATA_DIR);

    bool loaded = false;

    /* Try to load existing keys */
    if(storage_file_exists(storage, BTCPC_KEY_PATH) &&
       storage_file_exists(storage, BTCPC_PUB_PATH)) {
        File* f = storage_file_alloc(storage);

        bool sk_ok = false, pk_ok = false;
        if(storage_file_open(f, BTCPC_KEY_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            sk_ok = (storage_file_read(f, app->sk, BTCPC_SK_LEN) == BTCPC_SK_LEN);
            storage_file_close(f);
        }
        if(storage_file_open(f, BTCPC_PUB_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
            pk_ok = (storage_file_read(f, app->pk, BTCPC_PK_LEN) == BTCPC_PK_LEN);
            storage_file_close(f);
        }

        storage_file_free(f);
        loaded = sk_ok && pk_ok;
    }

    if(!loaded) {
        /* Generate new keypair from secure random seed */
        FURI_LOG_I(TAG, "No identity found — generating ed25519 keypair");

        /* Generate: writes sk (64 bytes) and pk (32 bytes) */
        btcpc_ed25519_keypair(app->pk, app->sk);

        /* Persist */
        File* f = storage_file_alloc(storage);

        if(storage_file_open(f, BTCPC_KEY_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_write(f, app->sk, BTCPC_SK_LEN);
            storage_file_close(f);
        } else {
            FURI_LOG_E(TAG, "Failed to write identity.key");
        }

        if(storage_file_open(f, BTCPC_PUB_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_write(f, app->pk, BTCPC_PK_LEN);
            storage_file_close(f);
        } else {
            FURI_LOG_E(TAG, "Failed to write identity.pub");
        }

        storage_file_free(f);
        loaded = true;
    }

    app->has_identity = loaded;
    furi_record_close(RECORD_STORAGE);
    return loaded;
}

void btcpc_pub_to_hex(const uint8_t pk[BTCPC_PK_LEN], char out[BTCPC_PK_LEN * 2 + 1]) {
    static const char hex[] = "0123456789abcdef";
    for(size_t i = 0; i < BTCPC_PK_LEN; i++) {
        out[i * 2]     = hex[(pk[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[pk[i] & 0x0F];
    }
    out[BTCPC_PK_LEN * 2] = '\0';
}

/* ─── Heartbeat timer callback (timer thread) ──────────────────────────── */

static void btcpc_heartbeat_timer_cb(void* context) {
    BtcpcApp* app = context;
    if(app->ble_connected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, BtcpcEventHeartbeatTimer);
    }
}

/* ─── BLE sign-request callback (BLE ISR thread) ───────────────────────── */

/*
 * Called by the GATT event handler when SIGN_REQUEST is written.
 * Must not block, must not touch sk[], must not call sign().
 *
 * Posts BtcpcEventSignRequest so the app thread does the actual signing.
 */
void btcpc_ble_sign_req_cb(const uint8_t* hash, void* context) {
    BtcpcApp* app = context;

    furi_mutex_acquire(app->sign_mutex, FuriWaitForever);
    if(!app->sign_pending) {
        memcpy(app->sign_pending_hash, hash, BTCPC_BLE_SIGN_REQ_LEN);
        app->sign_pending = true;
    } else {
        /* A sign request is already queued; drop this one. The phone should
         * wait for the SIGN_RESPONSE notification before sending another. */
        FURI_LOG_W(TAG, "sign request dropped — previous not yet processed");
    }
    furi_mutex_release(app->sign_mutex);

    /* Wake the app thread — safe to call from ISR context */
    view_dispatcher_send_custom_event(app->view_dispatcher, BtcpcEventSignRequest);
}

/* ─── DATA_CHANNEL receive callback (BLE ISR thread) ──────────────────── */

void btcpc_ble_data_rx_cb(const uint8_t* frame, uint16_t len, void* context) {
    BtcpcApp* app = context;
    furi_mutex_acquire(app->data_rx_mutex, FuriWaitForever);
    if(len > BTCPC_BLE_DATA_CH_MAX_LEN) len = BTCPC_BLE_DATA_CH_MAX_LEN;
    memcpy(app->data_rx_buf, frame, len);
    app->data_rx_len = len;
    furi_mutex_release(app->data_rx_mutex);
    view_dispatcher_send_custom_event(app->view_dispatcher, BtcpcEventDataRx);
}

/* ─── BLE GAP status callback (BT service thread) ──────────────────────── */

static void btcpc_bt_status_cb(BtStatus status, void* context) {
    BtcpcApp* app = context;
    switch(status) {
    case BtStatusConnected:
        app->ble_connected = true;
        view_dispatcher_send_custom_event(app->view_dispatcher, BtcpcEventBleConnected);
        FURI_LOG_I(TAG, "BLE connected");
        break;
    case BtStatusAdvertising:
    case BtStatusOff:
    case BtStatusUnavailable:
        if(app->ble_connected) {
            app->ble_connected = false;
            view_dispatcher_send_custom_event(app->view_dispatcher, BtcpcEventBleDisconnected);
            FURI_LOG_I(TAG, "BLE disconnected");
        }
        break;
    }
}

/* ─── BLE lifecycle (app thread) ────────────────────────────────────────── */

bool btcpc_ble_start(BtcpcApp* app) {
    app->bt = furi_record_open(RECORD_BT);

    /* Point the BT service at our app-specific key storage file.
     * This persists bonding keys across app launches without affecting
     * the Flipper's main BLE key store. */
    bt_keys_storage_set_storage_path(app->bt, BTCPC_BT_KEYS_PATH);

    /* Register connection-state callback */
    bt_set_status_changed_callback(app->bt, btcpc_bt_status_cb, app);

    /* Start the BTCPC custom profile — restarts Core2 */
    app->ble_profile = bt_profile_start(app->bt, ble_profile_btcpc, app);
    if(!app->ble_profile) {
        FURI_LOG_E(TAG, "bt_profile_start failed");
        bt_keys_storage_set_default_path(app->bt);
        furi_record_close(RECORD_BT);
        app->bt = NULL;
        return false;
    }

    /* bt_profile_start() starts advertising automatically in SDK 1.4+ */
    FURI_LOG_I(TAG, "BLE advertising as '%s'", BTCPC_BLE_ADV_NAME);

    furi_timer_start(app->heartbeat_timer, furi_ms_to_ticks(30000));
    return true;
}

void btcpc_ble_stop(BtcpcApp* app) {
    if(!app->bt) return;

    furi_timer_stop(app->heartbeat_timer);

    if(app->ble_profile) {
        bt_profile_restore_default(app->bt);
        app->ble_profile = NULL;
    }

    bt_set_status_changed_callback(app->bt, NULL, NULL);
    bt_keys_storage_set_default_path(app->bt);

    furi_record_close(RECORD_BT);
    app->bt = NULL;
    app->ble_connected = false;
}

/* ─── App entry point ───────────────────────────────────────────────────── */

int32_t btcpc_app(void* p) {
    UNUSED(p);

    BtcpcApp* app = btcpc_app_alloc();

    btcpc_identity_load_or_create(app);
    btcpc_pub_to_hex(app->pk, app->pub_hex);

    /* Start BLE signing service */
    if(!btcpc_ble_start(app)) {
        FURI_LOG_E(TAG, "BLE start failed — continuing without BLE");
    }

    scene_manager_next_scene(app->scene_manager, BtcpcSceneMain);
    view_dispatcher_run(app->view_dispatcher);

    /* Teardown */
    btcpc_ble_stop(app);
    btcpc_app_free(app);
    return 0;
}
