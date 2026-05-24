# BTCPC Android Client — Architecture

## Overview

The Android client is a Capacitor app (`clients/btcpc-android/`) that wraps
the BTCPC web UI and provides native Android capabilities through a thin UniFFI
Rust bridge. The web UI loads from `https://btcpc.net/app` (see `capacitor.config.ts`).

```
clients/btcpc-android/          ← Capacitor app (this directory)
  android/                      ← Android Studio project
  www/                          ← web assets (currently remote via server.url)
  capacitor.config.ts           ← Capacitor configuration

rust/btcpc-mobile-core/         ← UniFFI Rust library (new, in rust/ workspace)
  src/
    btcpc_mobile_core.udl       ← UniFFI interface definition
    lib.rs                      ← public API surface
    derive.rs                   ← SLIP-10 posting key derivation
    sign.rs                     ← Ed25519 canonical entry signing
    http.rs                     ← Node HTTP API client
    ble.rs                      ← BLE frame parser for Flipper Zero

rust/btcpc-android-legacy/      ← Archived JNI/candle/libp2p crate (see below)
  src/                          ← kept for historical reference; do not modify

clients/btcpc-flipper/
  protocol/btcpc_protocol.h     ← BLE wire format (shared contract with firmware)
```

---

## Why This Split

The original `rust/btcpc-android/` crate (now archived as `rust/btcpc-android-legacy/`)
was a JNI-based cdylib that embedded a full libp2p node, an LLM inference engine
(candle/GGUF), a sled state store, and on-device mining. That approach had fundamental
problems on Android:

- Full libp2p gossipsub on mobile violates Android's background execution limits.
  Doze mode and App Standby will kill the P2P stack within minutes of backgrounding.
- On-device LLM inference (candle + GGUF) is impractical for all but the highest-end
  phones and drains battery in minutes.
- The JNI bridge bypassed Capacitor entirely, making the web layer a thin shell with
  no access to the native functionality.
- OpenSSL vendored builds (`openssl = { features = ["vendored"] }`) are fragile across
  Android ABI targets (aarch64, armv7, x86_64) with cargo-ndk.

The new `rust/btcpc-mobile-core/` is narrow by design:
- UniFFI bindings (not raw JNI) — generates clean Kotlin/Swift from `.udl`
- Pure-Rust TLS (rustls, no OpenSSL)
- No P2P networking, no LLM, no local chain state
- The phone connects to a gateway node; the node does the heavy lifting

---

## btcpc-mobile-core: What It Does

### Key Derivation (derive.rs)

SLIP-10 ed25519 derivation matching `rust/btcpc-node/src/wallet.rs` exactly:

```
Master:    HMAC-SHA512("ed25519 seed", bip39_seed_bytes)
Per-index: HMAC-SHA512(chain_code, 0x00 || key || (idx | 0x80000000).to_be_bytes())
No passphrase: mnemonic.to_seed("")
```

BTCPC role keys — path `m/44'/6942'/<role>'/0'`:

| Role | Index | Name    | Notes                                         |
|------|-------|---------|-----------------------------------------------|
| 0    | 0'    | owner   | Key rotation, governance. Keep cold, not here.|
| 1    | 1'    | active  | Transfers, staking. Require biometric/PIN.    |
| 2    | 2'    | posting | Daily operations. Safe to keep on device.     |
| 3    | 3'    | memo    | Encrypted messages, selective disclosure.     |

The mobile core exposes `derive_posting_pubkey()` (returns public key only) and
`sign_transfer()` / `sign_with_role()` (signs without ever returning the private key).

### Signing (sign.rs)

The canonical signing message matches `canonical_signing_message()` in
`rust/btcpc-node/src/tx.rs` exactly. Transfer message format:

```json
{"chain_id":"btcpc-satoshi","type":"TRANSFER","from":"alice","to":"bob",
 "amount":100000000000,"token":"dreams","nonce":1}
```

The signature is `ed25519.sign(message.as_bytes())`. The node verifies with
`verifying_key.verify_strict(message.as_bytes(), &sig)`.

Note: `Transfer` is validated against the **active** key on-chain (not posting).
`sign_transfer()` uses the posting key (role 2) — callers must use `sign_with_role(1, ...)`
if the account requires active-key signing. See tx.rs lines 174–176.

### HTTP (http.rs)

Verified against `rust/btcpc-node/src/api.rs`. There is NO generic `/api/entries`
endpoint — each entry type has its own route with a flat JSON body.

- `fetch_node_info(base_url)` → `GET /api/node/info` → raw JSON string
- `post_transfer(base_url, from, to, amount, token, memo, signed_by, nonce, sig_hex)`
  → `POST /api/transfer` with flat body `{from, to, amount, token, memo, signed_by, nonce, signature}`
  → response: `{"hash": "<64-hex>" | null, "accepted": bool, "error": "<str>" | null}`

Other entry routes (not yet in mobile-core, add as needed):
  `POST /api/stake`    — `{account, amount, nonce, signed_by, signature}`
  `POST /api/unstake`  — `{account, amount, nonce, signed_by, signature}`

Uses `reqwest` with `rustls-tls` (no OpenSSL). 15-second timeout.

HARDLINE: the node returns `{"accepted": false, "error": "not connected to network..."}` when
`peer_count == 0`. The mobile client MUST surface this to the user and must NOT show the
entry as confirmed. This is enforced in `post_transfer()` as a `MobileCoreError::ParseError`.

### BLE Frame Parser (ble.rs)

Parses frames from the Flipper Zero per `btcpc_protocol.h`:

```
[magic: 4][msg_type: 1][payload_len: 2 LE][sig: 64][payload: payload_len]
  Total header: 71 bytes
  Max payload:  472 bytes (BLE ATT MTU 512 − 71 = 441, spec says 472)
```

`parse_ble_frame()` validates magic + length, returns `BleFrame { msg_type, payload }`.
`verify_ble_frame_sig()` verifies the ed25519 sig (covers payload bytes only) against
the Flipper's on-chain public key.

---

## Android Build Path

### cargo-ndk Target Triples

```bash
rustup target add aarch64-linux-android   # ARM64 (modern phones)
rustup target add armv7-linux-androideabi # ARM32 (older phones)
rustup target add x86_64-linux-android    # Emulator

cargo ndk -t arm64-v8a -t armeabi-v7a -t x86_64 \
    -o clients/btcpc-android/android/app/src/main/jniLibs \
    build -p btcpc-mobile-core --release
```

The `.so` files land at:
```
clients/btcpc-android/android/app/src/main/jniLibs/
  arm64-v8a/libbtcpc_mobile_core.so
  armeabi-v7a/libbtcpc_mobile_core.so
  x86_64/libbtcpc_mobile_core.so
```

### UniFFI Kotlin Binding Generation

```bash
cargo run --bin uniffi-bindgen generate \
    rust/btcpc-mobile-core/src/btcpc_mobile_core.udl \
    --language kotlin \
    --out-dir clients/btcpc-android/android/app/src/main/java/net/btcpc/uniffi/
```

This generates `BtcpcMobileCore.kt` with the Kotlin wrappers that the Capacitor
plugin calls. The Capacitor plugin layer (not yet built) sits between this Kotlin
class and the JS web layer.

### NDK Version

Tested with NDK r26 (LTS). Do not use NDK r27+ until uniffi 0.29+ is verified with it.
Set `ANDROID_NDK_HOME` before running cargo-ndk.

---

## Archived Crate: rust/btcpc-android-legacy/

The original crate is preserved at `rust/btcpc-android-legacy/` with full git history.
It is NOT a member of `rust/Cargo.toml` workspace. It has its own `[workspace]` root
and its own `Cargo.lock`. Do not add it to the workspace — the candle + libp2p
dependency tree conflicts with the node's workspace dependencies.

To reference it: `cd rust/btcpc-android-legacy && cargo check`.

It will not be deleted until the new mobile-core path is fully wired and tested on
a physical device.

---

## Trust Boundary

The Capacitor config currently sets `server.url: https://btcpc.net/app`, which means
the app loads web content from the network at runtime. This creates a critical trust
boundary concern for any feature that involves key derivation or signing:

**If key derivation is ever triggered from JS running in the WebView, then a
compromise of btcpc.net (CDN, DNS, TLS MITM) is equivalent to wallet compromise.**

Current mitigations:
- Key derivation and signing are in the native Rust layer (`btcpc-mobile-core`),
  not in JS.
- The Capacitor plugin bridge is the gating point — it must validate that all signing
  calls include user confirmation (biometric or PIN) before invoking `sign_transfer`.
- The UDL never exposes private key bytes directly to the JS layer.

Required before shipping any signing feature:
- Security auditor review of the Capacitor plugin bridge.
- Biometric/PIN gate on every call to `sign_with_role()` or `sign_transfer()`.
- Consider switching `server.url` to a locally bundled app (`webDir: 'www'`) for the
  signing flow, so it cannot be replaced by a remote server.

This decision must be made explicitly before any wallet feature ships.

---

## Flipper Signing Delegation Gap

### Current State

The Flipper Zero can currently:
- Send sensor data to the phone (SubGhz, RFID, NFC, iButton, heartbeat, IR)
- Receive chain entry hashes for Sub-GHz rebroadcast
- Receive clock sync and GPS from phone

The Flipper Zero cannot currently:
- Sign BTCPC entries — it has no access to the posting key
- Act as a hardware signing device / cold key store

### Protocol Extension Required

To enable Flipper-delegated signing, two new message types must be added to
`clients/btcpc-flipper/protocol/btcpc_protocol.h`. The firmware agent must
implement these message types. Both sides must implement together — the phone
side stub is documented here, the firmware side is a separate implementation task.

**Do not implement the firmware side yet. This section defines the contract only.**

#### New Message Types

```c
/* Phone → Flipper */
BTCPC_MSG_SIGN_REQ  = 0x13,  /* phone requests Flipper to sign a 32-byte digest */

/* Flipper → phone */
BTCPC_MSG_SIGN_RESP = 0x07,  /* Flipper returns sig or rejection */
```

Rationale for these IDs:
- `0x07` extends the Flipper→phone range (0x01–0x06) naturally.
- `0x13` extends the phone→Flipper range (0x10–0x12) naturally.
- Both values fit in a u8 and avoid collisions with all current types.

#### SIGN_REQ Payload (phone → Flipper, msg_type = 0x13)

```c
typedef struct __attribute__((packed)) {
    uint32_t request_id;   /* caller-assigned ID for matching async responses */
    uint8_t  purpose;      /* 0x00 = sign chain entry, 0x01 = auth challenge */
    uint8_t  digest[32];   /* SHA-256 of the canonical signing message bytes  */
} BtcpcSignReq;            /* total: 37 bytes */
```

- `request_id`: 32-bit monotonic counter assigned by the phone. Required because
  BLE is async — the phone may pipeline multiple requests and must match responses
  to the right caller. Without it, a delayed SIGN_RESP cannot be correlated.
- `purpose`: distinguishes signing contexts so the Flipper's UI can show the user
  the correct confirmation screen ("Sign transaction" vs "Authenticate").
- `digest`: the SHA-256 of the canonical signing message string (UTF-8 bytes),
  NOT the entry JSON. This matches how the node verifies: `verify_strict(message.as_bytes(), sig)`.

The Flipper signs `digest` with its stored ed25519 posting key and returns SIGN_RESP.

#### SIGN_RESP Payload (Flipper → phone, msg_type = 0x07)

```c
typedef struct __attribute__((packed)) {
    uint32_t request_id;   /* mirrors the request_id from SIGN_REQ */
    uint8_t  status;       /* 0x00 = signed OK, nonzero = rejected (see codes) */
    uint8_t  sig[64];      /* ed25519 signature; zero-filled if status != 0x00 */
} BtcpcSignResp;           /* total: 69 bytes — fits within BTCPC_MAX_PAYLOAD (472) */
```

Status codes:
```c
#define BTCPC_SIGN_OK           0x00  /* signed successfully */
#define BTCPC_SIGN_REJECTED     0x01  /* user pressed back / cancelled */
#define BTCPC_SIGN_LOCKED       0x02  /* Flipper is locked, cannot sign now */
#define BTCPC_SIGN_TIMEOUT      0x03  /* user did not confirm within timeout */
#define BTCPC_SIGN_NO_KEY       0x04  /* no posting key provisioned on this Flipper */
#define BTCPC_SIGN_WRONG_PURPOSE 0x05 /* purpose byte not recognised */
```

The phone must treat any nonzero status as a signing failure and surface the
appropriate error to the user. The `sig` field is meaningless when `status != 0x00`.

#### Phone-Side Implementation (TODO — not yet built)

When `btcpc-mobile-core` is extended to support delegated signing:

1. Build the SIGN_REQ payload with a monotonic `request_id`.
2. Wrap it in a `BtcpcFrame` with `msg_type = 0x13`.
3. Send over BLE to the Flipper's GATT characteristic (firmware defines this).
4. Await a SIGN_RESP frame with the matching `request_id`.
5. Verify the response signature against the Flipper's on-chain public key before
   using it — even over a trusted BLE channel.
6. If `status != 0x00`, surface the error to the user with a clear message.

A `request_id` timeout should be implemented on the phone (suggested: 30 seconds),
after which the pending request is cancelled and the user is notified.

#### Firmware-Side Implementation Notes (for Firmware Agent)

The firmware agent should implement on the Flipper side:
- Parse `BtcpcSignReq` from incoming `0x13` frames.
- Display a confirmation screen showing the `purpose` and a truncated hex of `digest`.
- On user confirmation: sign `digest` with the Flipper's stored ed25519 posting key.
- Return `BtcpcSignResp` with `request_id` mirrored and `status = 0x00` + `sig`.
- On user cancel / timeout / no key: return the appropriate status code.
- The Flipper's posting key must be provisioned separately (key injection flow TBD).

The Flipper's BLE GATT service characteristic for receive/transmit must be defined
in the firmware before the phone can discover it. Coordinate this with the firmware
agent before implementing the phone side of the BLE transport.

---

## Next Steps (Ordered)

1. **Capacitor plugin bridge** (not yet built): Kotlin plugin that calls UniFFI
   `BtcpcMobileCore` functions and exposes them to the Capacitor JS layer.
   This is where the biometric/PIN gate lives.

2. **cargo-ndk build script**: `scripts/build-jni.sh` to cross-compile for all
   three ABI targets and copy `.so` files to `jniLibs/`.

3. **Trust boundary decision**: Decide whether the signing flow uses locally
   bundled web assets or continues to load from `btcpc.net`. This decision blocks
   security review sign-off.

4. **Security review**: Required before any signing feature ships. The auditor
   must review the Capacitor plugin bridge, the biometric gate, and the trust
   boundary decision.

5. **Flipper signing delegation**: Coordinate with the firmware agent once the
   GATT characteristic is defined. Implement the phone-side `BtcpcSignReq` sender
   and `BtcpcSignResp` receiver in `ble.rs`.

6. **Physical device test**: The UniFFI path must be tested on a real Android device
   (not emulator only) before any of this is considered shipped.
