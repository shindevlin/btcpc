/*
 * btcpc_ble_svc.c — BTCPC signing GATT service implementation
 *
 * Registers a custom 128-bit UUID primary service with three characteristics:
 *
 *   SIGN_REQUEST  (write-without-response): phone writes 32-byte hash
 *   SIGN_RESPONSE (notify):                 Flipper notifies 64-byte signature
 *   PUBKEY        (read):                   phone reads 32-byte public key
 *
 * The STM32WB55 BLE stack runs on Core2. Events arrive via the IPCC
 * interrupt and are dispatched through ble_event_dispatcher. This handler
 * intercepts EVT_BLUE_GATT_WRITE_PERMIT_REQ and EVT_BLUE_GATT_ATTRIBUTE_MODIFIED
 * for the SIGN_REQUEST characteristic and fires the sign callback.
 *
 * Thread safety: the sign callback is called from the BLE ISR thread.
 * The caller (btcpc.c) posts a message to the app thread; signing and
 * btcpc_ble_svc_send_signature() are called from the app thread.
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

    /* Characteristic instances (handle + runtime copy of descriptor) */
    BleGattCharacteristicInstance sign_req_char;
    BleGattCharacteristicInstance sign_resp_char;
    BleGattCharacteristicInstance pubkey_char;

    /* Cached public key for the PUBKEY characteristic data callback */
    uint8_t pk[BTCPC_BLE_PUBKEY_LEN];

    /* App callback fired when a sign request arrives */
    BtcpcBleSvcSignRequestCb sign_cb;
    void*                    sign_ctx;

    /* BLE event handler registration */
    GapSvcEventHandler* evt_handler;
};

/* ─── Characteristic data callbacks ────────────────────────────────────── */

/*
 * PUBKEY data callback — called by the GATT layer:
 *   (a) at init time with data==NULL to query the max value length
 *   (b) on each update to get the current bytes to write
 *
 * Returns false: ownership stays with the GATT layer (no free needed).
 */
static bool btcpc_pubkey_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    const BtcpcBleSvc* svc = context;
    *data_len = BTCPC_BLE_PUBKEY_LEN;
    if(data != NULL) {
        *data = svc->pk;
    }
    return false;
}

/*
 * SIGN_RESPONSE data callback — called at init to report the max length.
 * Actual notification data is supplied directly in btcpc_ble_svc_send_signature().
 */
static bool btcpc_sign_resp_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    UNUSED(context);
    *data_len = BTCPC_BLE_SIGN_RESP_LEN;
    if(data != NULL) {
        /* No persistent buffer — caller supplies data explicitly */
        *data = NULL;
    }
    return false;
}

/* ─── Characteristic descriptors (static, shared across instances) ──────── */

/*
 * SIGN_REQUEST: write-without-response, 32 bytes fixed length.
 * GATT_NOTIFY_ATTRIBUTE_WRITE fires our event handler when written.
 * No read or notify property — prevents the signature engine from being
 * queried or replayed.
 */
static const BleGattCharacteristicParams btcpc_sign_req_char_desc = {
    .name              = "BTCPC_SIGN_REQ",
    .descriptor_params = NULL,
    .data              = { .fixed = { .ptr = NULL, .length = 0 } },
    .uuid              = { .Char_UUID_128 = BTCPC_CHAR_SIGN_REQ_UUID },
    .data_prop_type    = FlipperGattCharacteristicDataFixed,
    .is_variable       = 0,  /* fixed length */
    .uuid_type         = UUID_TYPE_128,
    .char_properties   = CHAR_PROP_WRITE_WITHOUT_RESP,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask     = GATT_NOTIFY_ATTRIBUTE_WRITE,
};

/*
 * SIGN_RESPONSE: notify only, 64 bytes max.
 * Callback-based so the GATT layer knows the max length at init time.
 * No read property — the central must subscribe to notifications.
 */
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
    .gatt_evt_mask     = GATT_NOTIFY_ATTRIBUTE_WRITE,  /* CCCD write events */
};

/*
 * PUBKEY: read only, 32 bytes.
 * Callback-based so the live pk[] buffer is always served.
 * No write property — the key is authoritative on the device.
 */
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

/* ─── BLE event handler ──────────────────────────────────────────────────── */

/*
 * Handle raw BLE stack events delivered via IPCC from Core2.
 *
 * We intercept EVT_BLUE_GATT_ATTRIBUTE_MODIFIED events targeting our
 * SIGN_REQUEST characteristic handle. When we see one with exactly
 * BTCPC_BLE_SIGN_REQ_LEN bytes, we fire the sign callback.
 *
 * All other events are passed through (BleEventNotAck).
 *
 * This runs in the BLE ISR thread — keep it fast, no blocking.
 */

/* ACI GATT attribute-modified event structure (from STM32WB BLE stack) */
typedef struct __attribute__((packed)) {
    uint8_t  Status;
    uint16_t Connection_Handle;
    uint16_t Attr_Handle;
    uint16_t Offset;
    uint16_t Attr_Data_Length;
    uint8_t  Attr_Data[1]; /* variable length */
} aci_gatt_attribute_modified_event_rp0;

#define EVT_BLUE_GATT_ATTRIBUTE_MODIFIED  0x0C01U

static BleEventAckStatus btcpc_ble_svc_event_handler(void* event, void* context) {
    BtcpcBleSvc* svc = context;

    /* event is hci_uart_pckt* — extract the event code */
    const uint8_t* pkt = (const uint8_t*)event;
    /* HCI event packet: type(1) + code(1) + param_len(1) + params */
    if(pkt[0] != HCI_EVENT_PKT_TYPE) return BleEventNotAck;
    /* Vendor-specific events use code 0xFF; subevent is first two bytes */
    if(pkt[1] != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    /* Subevent code is little-endian at pkt[3..4] */
    uint16_t subevent = (uint16_t)pkt[3] | ((uint16_t)pkt[4] << 8);
    if(subevent != EVT_BLUE_GATT_ATTRIBUTE_MODIFIED) return BleEventNotAck;

    /* Parse the attribute-modified payload (starts at pkt[5]) */
    const aci_gatt_attribute_modified_event_rp0* ev =
        (const aci_gatt_attribute_modified_event_rp0*)(pkt + 5);

    /* Check: is this our SIGN_REQUEST characteristic handle? */
    if(ev->Attr_Handle != svc->sign_req_char.handle) return BleEventNotAck;

    /* Reject anything that isn't exactly 32 bytes */
    if(ev->Attr_Data_Length != BTCPC_BLE_SIGN_REQ_LEN) {
        FURI_LOG_W(TAG, "SIGN_REQ bad length: %u", ev->Attr_Data_Length);
        return BleEventAckFlowEnable;
    }

    /* Fire the sign callback with the hash bytes */
    if(svc->sign_cb != NULL) {
        svc->sign_cb(ev->Attr_Data, svc->sign_ctx);
    }

    return BleEventAckFlowEnable;
}

/* ─── Service lifecycle ──────────────────────────────────────────────────── */

BtcpcBleSvc* btcpc_ble_svc_start(
    const uint8_t pk[BTCPC_BLE_PUBKEY_LEN],
    BtcpcBleSvcSignRequestCb cb,
    void* ctx) {

    BtcpcBleSvc* svc = malloc(sizeof(BtcpcBleSvc));
    if(!svc) return NULL;
    memset(svc, 0, sizeof(BtcpcBleSvc));

    svc->sign_cb  = cb;
    svc->sign_ctx = ctx;
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

    /* Patch the PUBKEY callback context to point at this instance */
    BleGattCharacteristicParams pubkey_desc = btcpc_pubkey_char_desc;
    pubkey_desc.data.callback.context = svc;

    /* Add SIGN_REQUEST characteristic */
    ble_gatt_characteristic_init(svc->svc_handle, &btcpc_sign_req_char_desc, &svc->sign_req_char);

    /* Add SIGN_RESPONSE characteristic (context not needed — data supplied explicitly) */
    ble_gatt_characteristic_init(svc->svc_handle, &btcpc_sign_resp_char_desc, &svc->sign_resp_char);

    /* Add PUBKEY characteristic */
    ble_gatt_characteristic_init(svc->svc_handle, &pubkey_desc, &svc->pubkey_char);

    /* Write initial pubkey value so a central can read it immediately */
    ble_gatt_characteristic_update(svc->svc_handle, &svc->pubkey_char, svc);

    /* Register event handler */
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

    ble_gatt_characteristic_delete(svc->svc_handle, &svc->pubkey_char);
    ble_gatt_characteristic_delete(svc->svc_handle, &svc->sign_resp_char);
    ble_gatt_characteristic_delete(svc->svc_handle, &svc->sign_req_char);
    ble_gatt_service_delete(svc->svc_handle);

    free(svc);
}

/* ─── Runtime operations ────────────────────────────────────────────────── */

bool btcpc_ble_svc_send_signature(BtcpcBleSvc* svc, const uint8_t sig[BTCPC_BLE_SIGN_RESP_LEN]) {
    if(!svc) return false;
    /* ble_gatt_characteristic_update with a non-NULL source uses that pointer
     * directly as the data to push — bypasses the callback */
    return ble_gatt_characteristic_update(svc->svc_handle, &svc->sign_resp_char, sig);
}

void btcpc_ble_svc_update_pubkey(BtcpcBleSvc* svc, const uint8_t pk[BTCPC_BLE_PUBKEY_LEN]) {
    if(!svc) return;
    memcpy(svc->pk, pk, BTCPC_BLE_PUBKEY_LEN);
    ble_gatt_characteristic_update(svc->svc_handle, &svc->pubkey_char, svc);
}
