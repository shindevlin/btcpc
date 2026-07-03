#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_box.h>
#include <storage/storage.h>
#include <applications/services/bt/bt_service/bt.h>

#include "crypto/ed25519.h"
#include "protocol/btcpc_protocol.h"
#include "ble/btcpc_ble_svc.h"
#include "ble/btcpc_ble_profile.h"

#define TAG               "BTCPC"
#define BTCPC_VERSION     "0.1.0"

#define BTCPC_NOTIFY_RARE_MS  200  /* vibration duration for rare detection */

#define BTCPC_DATA_DIR    EXT_PATH("apps_data/btcpc")
#define BTCPC_KEY_PATH    EXT_PATH("apps_data/btcpc/identity.key")
#define BTCPC_PUB_PATH    EXT_PATH("apps_data/btcpc/identity.pub")
/* BLE bonding keys — stored here so they survive app restarts */
#define BTCPC_BT_KEYS_PATH EXT_PATH("apps_data/btcpc/bt.keys")

/* OTA firmware update paths */
#define BTCPC_FAP_PATH     EXT_PATH("apps/Tools/btcpc.fap")
#define BTCPC_OTA_TMP_PATH EXT_PATH("apps/Tools/btcpc.fap.tmp")

/* Secret key: 64 bytes (seed || public in TweetNaCl expanded form) */
#define BTCPC_SK_LEN      64
/* Public key: 32 bytes */
#define BTCPC_PK_LEN      32

/* Scene IDs */
typedef enum {
    BtcpcSceneMain,
    BtcpcSceneIdentity,
    BtcpcSceneBle,
    BtcpcSceneUsb,
    BtcpcSceneCount,
} BtcpcScene;

/* View IDs */
typedef enum {
    BtcpcViewSubmenu,
    BtcpcViewPopup,
    BtcpcViewTextBox,
    BtcpcViewCount,
} BtcpcViewId;

/* Submenu item IDs */
typedef enum {
    BtcpcMenuIdentity = 0,
    BtcpcMenuBle      = 1,
    BtcpcMenuUsb      = 2,
} BtcpcMenuItem;

/*
 * Custom view-dispatcher events posted from the BLE ISR thread
 * and the BT status callback to the app (GUI) thread.
 */
typedef enum {
    BtcpcEventSignRequest     = 100, /* BLE: sign request arrived, hash in sign_pending_hash */
    BtcpcEventBleConnected    = 101, /* GAP: central connected */
    BtcpcEventBleDisconnected = 102, /* GAP: central disconnected */
    BtcpcEventDataRx          = 103, /* DATA_CHANNEL: framed message received from phone */
    BtcpcEventHeartbeatTimer  = 104, /* periodic: push heartbeat */
    BtcpcEventOtaChunk        = 105, /* OTA_WRITE: command/data from phone, chunk in ota_chunk_buf */
    BtcpcEventUsbSafetyTick   = 110, /* USB safety scene: 1-second poll tick */
} BtcpcCustomEvent;

typedef struct {
    /* GUI */
    Gui*              gui;
    ViewDispatcher*   view_dispatcher;
    SceneManager*     scene_manager;
    Submenu*          submenu;
    Popup*            popup;
    TextBox*          text_box;

    /* Crypto
     *
     * sk[] is protected by sign_mutex: any code that reads or uses sk[]
     * must hold sign_mutex. The BLE GATT handler never touches sk[]; it
     * only reads the incoming hash and posts a custom event. Signing is
     * done on the app thread under the mutex.
     */
    FuriMutex* sign_mutex;
    bool        has_identity;
    uint8_t     sk[BTCPC_SK_LEN]; /* ed25519 secret key — NEVER sent over BLE */
    uint8_t     pk[BTCPC_PK_LEN]; /* ed25519 public key */

    /* Pending sign request — written by BLE ISR, read by app thread.
     * sign_pending is set under sign_mutex before posting BtcpcEventSignRequest.
     * The app thread clears it after signing. */
    bool    sign_pending;
    uint8_t sign_pending_hash[BTCPC_BLE_SIGN_REQ_LEN];

    /* Incoming DATA_CHANNEL frame — written by BLE ISR, consumed by app thread.
     * Protected by data_rx_mutex. */
    FuriMutex* data_rx_mutex;
    uint8_t    data_rx_buf[BTCPC_BLE_DATA_CH_MAX_LEN];
    uint16_t   data_rx_len;

    /* OTA chunk — written by BLE ISR (ota_cb), consumed by app thread.
     * Protected by ota_mutex. State fields below are app-thread-only. */
    FuriMutex* ota_mutex;
    uint8_t    ota_chunk_buf[BTCPC_BLE_DATA_CH_MAX_LEN];
    uint16_t   ota_chunk_len;

    /* OTA session state (app thread only — no mutex needed) */
    bool       ota_in_progress;
    uint32_t   ota_expected_size;
    uint32_t   ota_bytes_written;
    uint32_t   ota_checksum;
    Storage*   ota_storage;
    File*      ota_file;

    /* BLE */
    Bt*                    bt;
    FuriHalBleProfileBase* ble_profile;
    bool                   ble_connected;

    /* Periodic heartbeat timer — fires BtcpcEventHeartbeatTimer while BLE is up */
    FuriTimer* heartbeat_timer;
    uint32_t   heartbeat_count;

    /* Last GPS received from phone via DATA_CHANNEL */
    bool    has_gps;
    int32_t last_lat_1e7;
    int32_t last_lon_1e7;

    /* True once a ClockSync has been received from the phone */
    bool    clock_synced;
    /* Clock sync anchor — used to derive epoch_minute for obs_id generation.
     * last_clock_unix_ms: unix time in ms from the most recent CLOCK_SYNC frame.
     * last_clock_tick:    furi_get_tick() value at the moment that frame arrived.
     * Together they let us estimate the current unix time as:
     *   now_ms = last_clock_unix_ms + (furi_get_tick() - last_clock_tick) * 1000 / tick_freq */
    uint64_t last_clock_unix_ms;
    uint32_t last_clock_tick;

    /* Scratch buffer for hex-encoding public key (64 hex chars + NUL) */
    char     pub_hex[BTCPC_PK_LEN * 2 + 1];
} BtcpcApp;

/* Called from scene files */
BtcpcApp* btcpc_app_alloc(void);
void      btcpc_app_free(BtcpcApp* app);
bool      btcpc_identity_load_or_create(BtcpcApp* app);
void      btcpc_pub_to_hex(const uint8_t pk[BTCPC_PK_LEN], char out[BTCPC_PK_LEN * 2 + 1]);

/* BLE lifecycle — called from btcpc_app (main thread) */
bool btcpc_ble_start(BtcpcApp* app);
void btcpc_ble_stop(BtcpcApp* app);

/*
 * Sign-request callback — fired from the BLE ISR thread.
 * Copies hash into app->sign_pending_hash under sign_mutex, then posts
 * BtcpcEventSignRequest to the view dispatcher so the app thread handles it.
 */
void btcpc_ble_sign_req_cb(const uint8_t* hash, void* context);

/*
 * DATA_CHANNEL receive callback — fired from the BLE ISR thread.
 * Copies frame bytes into app->data_rx_buf under data_rx_mutex, then posts
 * BtcpcEventDataRx to the view dispatcher so the app thread handles it.
 */
void btcpc_ble_data_rx_cb(const uint8_t* frame, uint16_t len, void* context);

/*
 * OTA_WRITE receive callback — fired from the BLE ISR thread.
 * Copies command/data into app->ota_chunk_buf under ota_mutex, then posts
 * BtcpcEventOtaChunk to the view dispatcher so the app thread handles it.
 */
void btcpc_ble_ota_cb(const uint8_t* data, uint16_t len, void* context);
