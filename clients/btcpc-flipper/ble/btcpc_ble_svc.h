/*
 * btcpc_ble_svc.h — BTCPC signing GATT service
 *
 * Exposes five characteristics over BLE:
 *
 *   SIGN_REQUEST  (write-without-response)
 *     Phone writes exactly 32 bytes (entry hash). Flipper signs with ed25519
 *     and posts the result to SIGN_RESPONSE via notification.
 *
 *   SIGN_RESPONSE  (notify)
 *     Flipper notifies with 64-byte ed25519 signature after each sign request.
 *     No read property — signature is push-only, not pollable.
 *
 *   PUBKEY  (read)
 *     Phone reads the 32-byte ed25519 public key. Read-only, no write.
 *
 *   OTA_WRITE  (write-without-response, authen)
 *     Phone sends OTA update commands. Only bonded device can write.
 *     Protocol: 'O'+u32_LE(size) open, 'D'+bytes chunk, 'C'+u32_LE(sum) commit, 'A' abort.
 *
 *   OTA_STATUS  (notify)
 *     Flipper notifies OTA progress: 'K' ready, 'Z' done, 'E'+u8_code error.
 *
 * Security properties: the private key never leaves this service. Only
 * signatures and the public key are transmitted over BLE. OTA_WRITE requires
 * authentication — only bonded peers can push firmware.
 *
 * Shin Devlin — btcpc.network
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <furi.h>

/* ─── GATT UUIDs ────────────────────────────────────────────────────────────
 *
 * Service:        7559e4f6-e38d-4013-a0a3-506a58adb3cb
 * SIGN_REQUEST:   74ffefa5-0d47-4b27-b4af-a3df9f109556
 * SIGN_RESPONSE:  f40ee8ee-b7a6-4034-8d22-a44357cc4a45
 * PUBKEY:         f0abf355-2cbf-41b1-8493-2f1aa7ec836f
 *
 * All UUIDs are 128-bit (type UUID_TYPE_128). Byte order in the arrays below
 * is little-endian as required by the STM32WB BLE stack (LSB first).
 */

/* Little-endian byte arrays for UUID_TYPE_128 */
#define BTCPC_SVC_UUID \
    { 0xcb, 0xb3, 0xad, 0x58, 0x6a, 0x50, 0xa3, 0xa0, 0x13, 0x40, 0x8d, 0xe3, 0xf6, 0xe4, 0x59, 0x75 }

#define BTCPC_CHAR_SIGN_REQ_UUID \
    { 0x56, 0x95, 0x10, 0x9f, 0xdf, 0xa3, 0xaf, 0xb4, 0x27, 0x4b, 0x47, 0x0d, 0xa5, 0xef, 0xff, 0x74 }

#define BTCPC_CHAR_SIGN_RESP_UUID \
    { 0x45, 0x4a, 0xcc, 0x57, 0x43, 0xa4, 0x22, 0x8d, 0x34, 0x40, 0xa6, 0xb7, 0xee, 0xe8, 0x0e, 0xf4 }

#define BTCPC_CHAR_PUBKEY_UUID \
    { 0x6f, 0x83, 0xec, 0xa7, 0x1a, 0x2f, 0x93, 0x84, 0xb1, 0x41, 0xbf, 0x2c, 0x55, 0xf3, 0xab, 0xf0 }

/*
 * DATA_CHANNEL: a4c8e1b7-5f2d-4380-9e16-d70c1f4a2e85
 *
 * Bidirectional framed data channel. Phone writes BtcpcFrame messages
 * (ClockSync, GPS, SensorReq); Flipper notifies BtcpcFrame responses
 * (Heartbeat, SubGhzObs, RfidScan, etc.). Wire format: btcpc_protocol.h.
 */
#define BTCPC_CHAR_DATA_CHANNEL_UUID \
    { 0x85, 0x2e, 0x4a, 0x1f, 0x0c, 0xd7, 0x16, 0x9e, 0x80, 0x43, 0x2d, 0x5f, 0xb7, 0xe1, 0xc8, 0xa4 }

/*
 * OTA_WRITE: 3c7f1b2a-4d5e-6f80-9a0b-c1d2e3f40516
 *
 * OTA firmware update write channel. Phone sends command frames:
 *   'O' + u32_LE(size)  — open:   start OTA, declare file size
 *   'D' + bytes         — data:   write up to 240 bytes of file data
 *   'C' + u32_LE(sum)   — commit: verify size+checksum, rename .tmp→.fap
 *   'A'                 — abort:  delete tmp file, reset state
 *
 * ATTR_PERMISSION_AUTHEN_WRITE — only bonded peers may write.
 * Destination path is hardcoded on Flipper; not supplied by phone.
 */
#define BTCPC_CHAR_OTA_WRITE_UUID \
    { 0x16, 0x05, 0xf4, 0xe3, 0xd2, 0xc1, 0x0b, 0x9a, 0x80, 0x6f, 0x5e, 0x4d, 0x2a, 0x1b, 0x7f, 0x3c }

/*
 * OTA_STATUS: 5e8d2c3b-6f7a-8b90-0c1d-2e3f4a5b6071
 *
 * Flipper notifies OTA progress to phone:
 *   'K'       — ready / acknowledged (1 byte)
 *   'Z'       — OTA complete (1 byte)
 *   'E' + u8  — error: 0x01=open failed, 0x02=write error,
 *                       0x03=size mismatch, 0x04=checksum mismatch,
 *                       0x05=rename failed (2 bytes)
 */
#define BTCPC_CHAR_OTA_STATUS_UUID \
    { 0x71, 0x60, 0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0c, 0x90, 0x8b, 0x7a, 0x6f, 0x3b, 0x2c, 0x8d, 0x5e }

/* Fixed payload sizes */
#define BTCPC_BLE_SIGN_REQ_LEN   32  /* SHA-256 hash of entry to sign */
#define BTCPC_BLE_SIGN_RESP_LEN  64  /* ed25519 signature */
#define BTCPC_BLE_PUBKEY_LEN     32  /* ed25519 public key */

/* Maximum DATA_CHANNEL frame that fits in a single BLE notification at ATT MTU 244.
 * 244 (ATT data length) - 3 (ATT notification overhead) = 241 usable bytes. */
#define BTCPC_BLE_DATA_CH_MAX_LEN  241

/* Max GATT attribute records for this service:
 *   1 (service decl)
 *   + SIGN_REQ:    2 (decl + value)
 *   + SIGN_RESP:   3 (decl + value + CCCD)
 *   + PUBKEY:      2 (decl + value)
 *   + DATA_CHANNEL: 3 (decl + value + CCCD)
 *   + OTA_WRITE:   2 (decl + value)
 *   + OTA_STATUS:  3 (decl + value + CCCD)
 *   = 16 + 4 spare = 20 */
#define BTCPC_SVC_MAX_ATTR_RECORDS  20

/* Called from BLE ISR when phone writes a sign request (32-byte hash). */
typedef void (*BtcpcBleSvcSignRequestCb)(const uint8_t* hash, void* context);

/* Called from BLE ISR when phone writes a DATA_CHANNEL frame.
 * `frame` points to the raw bytes; `len` is the byte count.
 * Must not block, must not call sign(). */
typedef void (*BtcpcBleSvcDataRxCb)(const uint8_t* frame, uint16_t len, void* context);

/* Called from BLE ISR when phone writes to OTA_WRITE characteristic.
 * `data` is the raw write; first byte is the command ('O','D','C','A').
 * Must not block. Only called for authenticated (bonded) peers. */
typedef void (*BtcpcBleSvcOtaCb)(const uint8_t* data, uint16_t len, void* context);

typedef struct BtcpcBleSvc BtcpcBleSvc;

/*
 * btcpc_ble_svc_start()
 *
 * Register the BTCPC GATT service, add all characteristics, and register the
 * BLE event handler. Returns an opaque service handle, or NULL on failure.
 *
 * `pk`       — 32-byte ed25519 public key (copied; caller retains ownership)
 * `sign_cb`  — called from BLE ISR when phone writes a sign request
 * `data_cb`  — called from BLE ISR when phone writes a DATA_CHANNEL frame
 * `ctx`      — passed through to both callbacks
 */
BtcpcBleSvc* btcpc_ble_svc_start(
    const uint8_t           pk[BTCPC_BLE_PUBKEY_LEN],
    BtcpcBleSvcSignRequestCb sign_cb,
    BtcpcBleSvcDataRxCb      data_cb,
    BtcpcBleSvcOtaCb         ota_cb,
    void*                    ctx);

/*
 * btcpc_ble_svc_stop()
 *
 * Unregister event handler and delete the GATT service.
 * Safe to call with NULL (no-op).
 */
void btcpc_ble_svc_stop(BtcpcBleSvc* svc);

/*
 * btcpc_ble_svc_send_signature()
 *
 * Notify the connected central with a 64-byte ed25519 signature.
 * Must be called from the app (non-ISR) thread after signing is complete.
 * Returns false if not connected or notification not enabled.
 */
bool btcpc_ble_svc_send_signature(BtcpcBleSvc* svc, const uint8_t sig[BTCPC_BLE_SIGN_RESP_LEN]);

/*
 * btcpc_ble_svc_update_pubkey()
 *
 * Update the PUBKEY characteristic value (e.g. after identity regeneration).
 */
void btcpc_ble_svc_update_pubkey(BtcpcBleSvc* svc, const uint8_t pk[BTCPC_BLE_PUBKEY_LEN]);

/*
 * btcpc_ble_svc_push_frame()
 *
 * Notify the connected central with a BtcpcFrame (sensor data, heartbeat, etc.).
 * `data` points to the raw frame bytes; `len` must be ≤ BTCPC_BLE_DATA_CH_MAX_LEN.
 * Must be called from the app (non-ISR) thread.
 * Returns false if not connected, CCCD not enabled, or `len` exceeds maximum.
 */
bool btcpc_ble_svc_push_frame(BtcpcBleSvc* svc, const uint8_t* data, uint16_t len);

/*
 * btcpc_ble_svc_send_ota_status()
 *
 * Notify the connected central with an OTA status byte sequence.
 * `status` must be 1 or 2 bytes: 'K', 'Z', or {'E', error_code}.
 * Must be called from the app (non-ISR) thread.
 * Returns false if not connected or CCCD not enabled.
 */
bool btcpc_ble_svc_send_ota_status(BtcpcBleSvc* svc, const uint8_t* status, uint8_t len);
