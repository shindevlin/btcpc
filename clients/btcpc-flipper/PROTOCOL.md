# BTCPC Flipper Zero — BLE Signing Protocol

This document is the Android developer's reference for the BTCPC Flipper
Zero BLE signing service. It covers GATT layout, UUID values, pairing
procedure, the signing request/response wire format, security properties,
and the on-chain registration flow.


## 1. Overview

The Flipper Zero acts as a hardware signing device. It holds a persistent
ed25519 keypair on its microSD card. The phone never has access to the
private key — only to signatures and the public key.

```
Phone                       Flipper Zero
  │                              │
  │── pair once ────────────────▶│  bonding, PIN confirmation
  │                              │
  │── read PUBKEY ──────────────▶│  32-byte ed25519 public key
  │                              │
  │── submit DeviceRegister ────▶│  (via BTCPC HTTP API, not BLE)
  │                              │
  │  [for each entry to sign]    │
  │── write SIGN_REQUEST ───────▶│  32-byte SHA-256 hash
  │◀─ SIGN_RESPONSE notify ──────│  64-byte ed25519 signature
  │                              │
  │── submit signed entry ──────▶│  (via BTCPC HTTP API, not BLE)
```


## 2. GATT Service Layout

### Service

| Field       | Value                                    |
|-------------|------------------------------------------|
| UUID        | `7559e4f6-e38d-4013-a0a3-506a58adb3cb`  |
| UUID type   | 128-bit                                  |
| Service type| Primary                                  |
| GAP name    | `BTCPC`                                  |

### Characteristic: PUBKEY

| Field       | Value                                    |
|-------------|------------------------------------------|
| UUID        | `f0abf355-2cbf-41b1-8493-2f1aa7ec836f`  |
| Properties  | Read                                     |
| Length      | 32 bytes (fixed)                         |
| Description | ed25519 public key (raw, not hex)        |

Read this once after connecting. Cache it — it does not change unless the
user explicitly regenerates the identity key on the Flipper.

### Characteristic: SIGN_REQUEST

| Field       | Value                                    |
|-------------|------------------------------------------|
| UUID        | `74ffefa5-0d47-4b27-b4af-a3df9f109556`  |
| Properties  | Write Without Response                   |
| Length      | 32 bytes (fixed, rejected otherwise)     |
| Description | SHA-256 hash of the BTCPC entry to sign  |

Write exactly 32 bytes. The Flipper rejects writes of any other length
without sending a SIGN_RESPONSE.

Do not queue multiple sign requests. Wait for the SIGN_RESPONSE notification
before writing the next request. The Flipper drops back-to-back requests
that arrive before the previous signature is sent.

### Characteristic: SIGN_RESPONSE

| Field       | Value                                    |
|-------------|------------------------------------------|
| UUID        | `f40ee8ee-b7a6-4034-8d22-a44357cc4a45`  |
| Properties  | Notify                                   |
| Length      | 64 bytes (fixed)                         |
| Description | ed25519 signature over the last hash     |

Subscribe to notifications (enable CCCD) immediately after connecting.
The Flipper only sends SIGN_RESPONSE as a notification — there is no
readable value. If notifications are not enabled, signatures are silently
discarded.


## 3. Pairing Procedure

1. Phone scans for BLE advertisements from device named `BTCPC`.
2. Phone initiates pairing. The Flipper displays a six-digit PIN on screen.
3. Phone user confirms the PIN matches. This prevents passive eavesdrop of
   the pairing exchange on the signing channel.
4. After bonding, the Flipper stores the long-term key in
   `/ext/apps_data/btcpc/bt.keys` on its microSD.
5. On subsequent app launches the Flipper auto-reconnects to the bonded
   phone without re-displaying the PIN.

**Android implementation notes:**

- Use `BluetoothDevice.createBond()` before connecting, or initiate pairing
  via the OS settings screen.
- After bonding, connect with `connectGatt()` and discover services.
- The Flipper advertises the BTCPC service UUID in its advertisement packet,
  so you can filter by service UUID during scan:
  `ScanFilter.Builder().setServiceUuid(ParcelUuid.fromString("7559e4f6-e38d-4013-a0a3-506a58adb3cb"))`
- Request MTU >= 100 via `BluetoothGatt.requestMtu(100)` immediately after
  connection. The 64-byte SIGN_RESPONSE fits in one notification at this MTU.
- Enable notifications on SIGN_RESPONSE (write `0x0100` to the CCCD
  descriptor) before sending any sign requests.

**Forget and re-pair:**

If you need to re-pair (e.g. phone factory reset), navigate to the Identity
screen on the Flipper and hold the Back button for 3 seconds to forget
bonded devices. This clears `/ext/apps_data/btcpc/bt.keys`.


## 4. Signing Protocol — Byte-Level Detail

### Sign Request (phone → Flipper)

Write to SIGN_REQUEST characteristic, Write Without Response:

```
Bytes 0–31: SHA-256 hash of the serialized BTCPC chain entry
```

The hash is the canonical SHA-256 of the entry bytes as they would appear
in the chain's pending pool. The Flipper signs these exact 32 bytes — it
does not re-hash them.

### Sign Response (Flipper → phone, notification)

Notification value on SIGN_RESPONSE characteristic:

```
Bytes 0–63: ed25519 signature (RFC 8032 format)
```

This is the raw 64-byte ed25519 signature produced by TweetNaCl's
`crypto_sign()` over the 32-byte input, using the Flipper's private key.

**Verification (Android / server side):**

```
verify(signature[64], message[32], pubkey[32])
```

Using any standard ed25519 library (e.g. Bouncy Castle, libsodium-jni,
or Tink). The signature scheme is standard RFC 8032 — no domain separation
prefix, no prehash.

### Timing

The STM32WB55 at 32 MHz completes ed25519 sign in approximately 50–150 ms.
The phone should display a "signing..." state and wait up to 500 ms before
treating the request as timed out and retrying.


## 5. Security Properties

- **Private key non-exportable**: The `sk[]` array in the Flipper firmware is
  never passed to any BLE handler or characteristic. The GATT event handler
  receives only the raw hash bytes. Signing happens on the application thread
  under a mutex, not in the BLE interrupt context.

- **No raw key read**: SIGN_REQUEST is write-without-response only (no Read
  property). SIGN_RESPONSE is notify only (no Read property). The central
  cannot poll for the private key or for previous signatures.

- **Replay protection at the chain layer**: The chain validates that entry
  hashes are not reused. The Flipper's role is only to attest authorship,
  not to enforce uniqueness. The chain node enforces uniqueness.

- **Bonding required**: The GAP configuration requires bonding before a
  central can write to SIGN_REQUEST or enable SIGN_RESPONSE notifications.
  Anonymous connections cannot trigger signing.

- **One bonded device**: The Flipper bonds to one central at a time. To
  switch phones, the old bond must be cleared first.


## 6. On-Chain Registration Flow

The Flipper's public key must be registered on the BTCPC chain before
signatures from it are accepted as device attestations.

1. Connect to the Flipper and read the PUBKEY characteristic (32 raw bytes).
2. Hex-encode the public key (64 hex characters).
3. Submit a `DeviceRegister` chain entry via the BTCPC HTTP API:

```http
POST http://<node>:4242/api/entries
Content-Type: application/json

{
  "type": "DeviceRegister",
  "pubkey": "<64-hex-public-key>",
  "device_type": "flipper-zero"
}
```

4. Once the entry is sealed in an epoch, the Flipper's signatures are
   accepted by the network for subsequent `DeviceSensor` entries.

The public key is also displayed on the Flipper's Identity screen as a
64-character hex string (split across two lines). Users can transcribe it
manually if the phone app is unavailable.


## 7. Rust Test Vectors

The canonical test vector set for this protocol will live in a Rust crate
at `rust/btcpc-device-identity/`. It will generate:

- Known-answer ed25519 sign/verify vectors using the same key derivation
  as the Flipper's TweetNaCl implementation
- BLE characteristic encode/decode round-trip tests
- Reject-on-wrong-length vectors (non-32-byte SIGN_REQUEST inputs)

Android developers should validate their BLE framing and signature
verification against these vectors before release. The crate is not yet
published; contact the maintainer for pre-release test data.


## 8. File Locations on Flipper microSD

```
/ext/apps_data/btcpc/
  identity.key    64-byte ed25519 secret key  (binary, never export)
  identity.pub    32-byte ed25519 public key   (binary)
  bt.keys         BLE bonding key storage      (managed by BT service)
```

All files are created on first app launch. The data directory
`/ext/apps_data/btcpc/` is created if it does not exist.
