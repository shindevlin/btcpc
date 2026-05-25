/*
 * btcpc_ble_svc.c — BTCPC GATT service implementation
 *
 * Registers a custom 128-bit UUID primary service with four characteristics:
 *
 *   SIGN_REQUEST   (write-without-response): phone writes 32-byte hash
 *   SIGN_RESPONSE  (notify):                 Flipper notifies 64-byte signature
 *   PUBKEY         (read):                   phone reads 32-byte public key
 *   DATA_CHANNEL   (write-without-response + notify):
 *                  Bidirectional framed data channel.
 *                  Phone writes BtcpcFrame messages (ClockSync, GPS, SensorReq).
 *                  Flipper notifies BtcpcFrame responses (Heartbeat, SubGhzObs, …).
 *
 * Thread safety:
 *   Sign and data-rx callbacks are called from the BLE ISR thread.
 *   btcpc_ble_svc_send_signature() and btcpc_ble_svc_push_frame() must be
 *   called from the app (non-ISR) thread.
 *
 * Shin Devlin — btcpc.network
 */

#include "btcpc_ble_svc.h"

#include <furi.h>
#include <furi_ble/gatt.h>
#include <furi_ble/event_dispatcher.h>
#include <ble/core/auto/ble_types.h>
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>

#include <string.h>

#define TAG "BtcpcBleSvc"

/* ─── Internal state ────────────────────────────────────────────────────── */

struct BtcpcBleSvc {
    uint16_t svc_handle;

    BleGattCharacteristicInstance sign_req_char;
    BleGattCharacteristicInstance sign_resp_char;
    BleGattCharacteristicInstance pubkey_char;
    BleGattCharacteristicInstance data_channel_char;

    /* Cached public key for PUBKEY read callbacks */
    uint8_t pk[BTCPC_BLE_PUBKEY_LEN];

    /* Sign-request callback (called from BLE ISR) */
    BtcpcBleSvcSignRequestCb sign_cb;

    /* DATA_CHANNEL receive callback (called from BLE ISR) */
    BtcpcBleSvcDataRxCb data_rx_cb;

    void* ctx; /* shared context passed to both callbacks */

    /* Outgoing DATA_CHANNEL frame — written on app thread before push_frame() */
    uint8_t  tx_buf[BTCPC_BLE_DATA_CH_MAX_LEN];
    uint16_t tx_len;

    GapSvcEventHandler* evt_handler;
};

/* ─── Characteristic data callbacks ────────────────────────────────────── */

static bool btcpc_pubkey_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    const BtcpcBleSvc* svc = context;
    *data_len = BTCPC_BLE_PUBKEY_LEN;
    if(data != NULL) *data = svc->pk;
    return false;
}

static bool btcpc_sign_resp_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    UNUSED(context);
    *data_len = BTCPC_BLE_SIGN_RESP_LEN;
    if(data != NULL) *data = NULL;
    return false;
}

/* DATA_CHANNEL TX callback.
 * At init: `data` is NULL — reports max length so the stack allocates the buffer.
 * At send: `data` is non-NULL — returns current tx_buf and tx_len. */
static bool btcpc_data_ch_tx_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    const BtcpcBleSvc* svc = context;
    *data_len = svc->tx_len > 0 ? svc->tx_len : BTCPC_BLE_DATA_CH_MAX_LEN;
    if(data != NULL) *data = svc->tx_buf;
    return false;
}

/* ─── Characteristic descriptors ────────────────────────────────────────── */

static const BleGattCharacteristicParams btcpc_sign_req_char_desc = {
    .name              = "BTCPC_SIGN_REQ",
    .descriptor_params = NULL,
    .data              = { .fixed = { .ptr = NULL, .length = 0 } },
    .uuid              = { .Char_UUID_128 = BTCPC_CHAR_SIGN_REQ_UUID },
    .data_prop_type    = FlipperGattCharacteristicDataFixed,
    .is_variable       = 0,
    .uuid_type         = UUID_TYPE_128,
    .char_properties   = CHAR_PROP_WRITE_WITHOUT_RESP,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask     = GATT_NOTIFY_ATTRIBUTE_WRITE,
};

static const BleGattCharacteristicParams btcpc_sign_resp_char_desc = {
    .name              = "BTCPC_SIGN_RESP",
    .descriptor_params = NULL,
    .data              = { .callback = { .fn = btcpc_sign_resp_data_cb, .context = NULL } },
    .uuid              = { .Char_UUID_128 = BTCPC_CHAR_SIGN_RESP_UUID },
    .data_prop_type    = FlipperGattCharacteristicDataCallback,
    .is_variable       = 0,
    .uuid_type         = UUID_TYPE_128,
    .char_properties   = CHAR_PROP_NOTIFY,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask     = GATT_NOTIFY_ATTRIBUTE_WRITE,
};

static const BleGattCharacteristicParams btcpc_pubkey_char_desc = {
    .name              = "BTCPC_PUBKEY",
    .descriptor_params = NULL,
    .data              = { .callback = { .fn = btcpc_pubkey_data_cb, .context = NULL } },
    .uuid              = { .Char_UUID_128 = BTCPC_CHAR_PUBKEY_UUID },
    .data_prop_type    = FlipperGattCharacteristicDataCallback,
    .is_variable       = 0,
    .uuid_type         = UUID_TYPE_128,
    .char_properties   = CHAR_PROP_READ,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask     = 0,
};

/* DATA_CHANNEL: write-without-response (phone→Flipper) + notify (Flipper→phone).
 * Variable-length; callback declares max and supplies current tx buffer. */
static const BleGattCharacteristicParams btcpc_data_ch_char_desc = {
    .name              = "BTCPC_DATA",
    .descriptor_params = NULL,
    .data              = { .callback = { .fn = btcpc_data_ch_tx_cb, .context = NULL } },
    .uuid              = { .Char_UUID_128 = BTCPC_CHAR_DATA_CHANNEL_UUID },
    .data_prop_type    = FlipperGattCharacteristicDataCallback,
    .is_variable       = 1,
    .uuid_type         = UUID_TYPE_128,
    .char_properties   = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_NOTIFY,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask     = GATT_NOTIFY_ATTRIBUTE_WRITE,
};

/* ─── BLE event handler ──────────────────────────────────────────────────── */

#define EVT_BLUE_GATT_ATTRIBUTE_MODIFIED  0x0C01U

static BleEventAckStatus btcpc_ble_svc_event_handler(void* event, void* context) {
    BtcpcBleSvc* svc = context;

    const uint8_t* pkt = (const uint8_t*)event;
    if(pkt[0] != HCI_EVENT_PKT_TYPE) return BleEventNotAck;
    if(pkt[1] != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    uint16_t subevent = (uint16_t)pkt[3] | ((uint16_t)pkt[4] << 8);
    if(subevent != EVT_BLUE_GATT_ATTRIBUTE_MODIFIED) return BleEventNotAck;

    const aci_gatt_attribute_modified_event_rp0* ev =
        (const aci_gatt_attribute_modified_event_rp0*)(pkt + 5);

    if(ev->Attr_Handle == svc->sign_req_char.handle) {
        /* SIGN_REQUEST write: must be exactly 32 bytes */
        if(ev->Attr_Data_Length != BTCPC_BLE_SIGN_REQ_LEN) {
            FURI_LOG_W(TAG, "SIGN_REQ bad length: %u", ev->Attr_Data_Length);
        } else if(svc->sign_cb != NULL) {
            svc->sign_cb(ev->Attr_Data, svc->ctx);
        }
        return BleEventAckFlowEnable;

    } else if(ev->Attr_Handle == svc->data_channel_char.handle) {
        /* DATA_CHANNEL value write: validate magic before firing callback */
        if(ev->Attr_Data_Length >= 4 &&
           ev->Attr_Data[0] == 'B' && ev->Attr_Data[1] == 'T' &&
           ev->Attr_Data[2] == 'P' && ev->Attr_Data[3] == 'C') {
            if(svc->data_rx_cb != NULL) {
                svc->data_rx_cb(ev->Attr_Data, ev->Attr_Data_Length, svc->ctx);
            }
        }
        return BleEventAckFlowEnable;
    }

    /* CCCD writes and everything else: pass through to the BLE stack */
    return BleEventNotAck;
}

/* ─── Service lifecycle ──────────────────────────────────────────────────── */

BtcpcBleSvc* btcpc_ble_svc_start(
    const uint8_t           pk[BTCPC_BLE_PUBKEY_LEN],
    BtcpcBleSvcSignRequestCb sign_cb,
    BtcpcBleSvcDataRxCb      data_cb,
    void*                    ctx) {

    BtcpcBleSvc* svc = malloc(sizeof(BtcpcBleSvc));
    if(!svc) return NULL;
    memset(svc, 0, sizeof(BtcpcBleSvc));

    svc->sign_cb    = sign_cb;
    svc->data_rx_cb = data_cb;
    svc->ctx        = ctx;
    memcpy(svc->pk, pk, BTCPC_BLE_PUBKEY_LEN);

    /* Register GATT service */
    const uint8_t svc_uuid_bytes[16] = BTCPC_SVC_UUID;
    Service_UUID_t svc_uuid;
    memcpy(svc_uuid.Service_UUID_128, svc_uuid_bytes, 16);

    bool ok = ble_gatt_service_add(
        UUID_TYPE_128,
        &svc_uuid,
        PRIMARY_SERVICE,
        BTCPC_SVC_MAX_ATTR_RECORDS,
        &svc->svc_handle);

    if(!ok) {
        FURI_LOG_E(TAG, "ble_gatt_service_add failed");
        free(svc);
        return NULL;
    }

    /* SIGN_REQUEST — no context needed (no callback data) */
    ble_gatt_characteristic_init(svc->svc_handle, &btcpc_sign_req_char_desc, &svc->sign_req_char);

    /* SIGN_RESPONSE — context not needed (data supplied explicitly in send_signature) */
    ble_gatt_characteristic_init(svc->svc_handle, &btcpc_sign_resp_char_desc, &svc->sign_resp_char);

    /* PUBKEY — patch context to point at this instance for live pk[] reads */
    BleGattCharacteristicParams pubkey_desc = btcpc_pubkey_char_desc;
    pubkey_desc.data.callback.context = svc;
    ble_gatt_characteristic_init(svc->svc_handle, &pubkey_desc, &svc->pubkey_char);
    ble_gatt_characteristic_update(svc->svc_handle, &svc->pubkey_char, svc);

    /* DATA_CHANNEL — patch context for tx callbacks */
    BleGattCharacteristicParams data_ch_desc = btcpc_data_ch_char_desc;
    data_ch_desc.data.callback.context = svc;
    ble_gatt_characteristic_init(svc->svc_handle, &data_ch_desc, &svc->data_channel_char);

    /* Register BLE event handler */
    svc->evt_handler = ble_event_dispatcher_register_svc_handler(
        btcpc_ble_svc_event_handler, svc);

    FURI_LOG_I(TAG, "GATT service started, svc_handle=0x%04x", svc->svc_handle);
    return svc;
}

void btcpc_ble_svc_stop(BtcpcBleSvc* svc) {
    if(!svc) return;

    if(svc->evt_handler) {
        ble_event_dispatcher_unregister_svc_handler(svc->evt_handler);
        svc->evt_handler = NULL;
    }

    ble_gatt_characteristic_delete(svc->svc_handle, &svc->data_channel_char);
    ble_gatt_characteristic_delete(svc->svc_handle, &svc->pubkey_char);
    ble_gatt_characteristic_delete(svc->svc_handle, &svc->sign_resp_char);
    ble_gatt_characteristic_delete(svc->svc_handle, &svc->sign_req_char);
    ble_gatt_service_delete(svc->svc_handle);

    free(svc);
}

/* ─── Runtime operations ────────────────────────────────────────────────── */

bool btcpc_ble_svc_send_signature(BtcpcBleSvc* svc, const uint8_t sig[BTCPC_BLE_SIGN_RESP_LEN]) {
    if(!svc) return false;
    return ble_gatt_characteristic_update(svc->svc_handle, &svc->sign_resp_char, sig);
}

void btcpc_ble_svc_update_pubkey(BtcpcBleSvc* svc, const uint8_t pk[BTCPC_BLE_PUBKEY_LEN]) {
    if(!svc) return;
    memcpy(svc->pk, pk, BTCPC_BLE_PUBKEY_LEN);
    ble_gatt_characteristic_update(svc->svc_handle, &svc->pubkey_char, svc);
}

bool btcpc_ble_svc_push_frame(BtcpcBleSvc* svc, const uint8_t* data, uint16_t len) {
    if(!svc || !data || len == 0 || len > BTCPC_BLE_DATA_CH_MAX_LEN) return false;
    memcpy(svc->tx_buf, data, len);
    svc->tx_len = len;
    return ble_gatt_characteristic_update(svc->svc_handle, &svc->data_channel_char, svc);
}
