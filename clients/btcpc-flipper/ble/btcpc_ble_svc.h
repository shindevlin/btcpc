/*
 * btcpc_ble_svc.h — BTCPC signing GATT service
 *
 * Exposes three characteristics over BLE:
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
 * Security properties: the private key never leaves this service. Only
 * signatures and the public key are transmitted over BLE.
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

/* Fixed payload sizes */
#define BTCPC_BLE_SIGN_REQ_LEN   32  /* SHA-256 hash of entry to sign */
#define BTCPC_BLE_SIGN_RESP_LEN  64  /* ed25519 signature */
#define BTCPC_BLE_PUBKEY_LEN     32  /* ed25519 public key */

/* Max GATT attribute records for this service:
 *   1 (service decl) + 3 chars × 2 (decl + value) + 1 (SIGN_RESP CCCD) = 8
 *   Add 2 spare = 10 */
#define BTCPC_SVC_MAX_ATTR_RECORDS  10

/*
 * Callback type: fired when the phone writes a sign request.
 * `hash` points to exactly BTCPC_BLE_SIGN_REQ_LEN bytes.
 * `context` is whatever was passed to btcpc_ble_svc_start().
 */
typedef void (*BtcpcBleSvcSignRequestCb)(const uint8_t* hash, void* context);

typedef struct BtcpcBleSvc BtcpcBleSvc;

/*
 * btcpc_ble_svc_start()
 *
 * Register the BTCPC GATT service, add characteristics, and register the
 * BLE event handler. Returns an opaque service handle, or NULL on failure.
 *
 * `pk`  — 32-byte ed25519 public key (copied internally; caller retains ownership)
 * `cb`  — called from the BLE ISR context when a sign request arrives
 * `ctx` — passed through to `cb`
 */
BtcpcBleSvc* btcpc_ble_svc_start(
    const uint8_t pk[BTCPC_BLE_PUBKEY_LEN],
    BtcpcBleSvcSignRequestCb cb,
    void* ctx);

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
