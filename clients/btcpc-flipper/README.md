# BTCPC — Flipper Zero App

Chain identity node for the BTCPC network. The Flipper generates an ed25519
keypair on first run, signs all outgoing sensor data, and relays it to a paired
phone over BLE. The phone registers the Flipper's public key on-chain via a
`DeviceRegister` entry during the initial pairing flow.

## Prerequisites

Install ufbt (Flipper Zero build tool):

```sh
pip install ufbt
```

ufbt downloads the correct Flipper firmware SDK automatically on first build.

## Build

```sh
cd clients/btcpc-flipper
ufbt
```

The compiled `.fap` file lands in `.ufbt/build/btcpc.fap`.

## Install

With the Flipper connected over USB:

```sh
ufbt launch
```

This installs the `.fap` and launches the app immediately.

To install without launching:

```sh
ufbt install
```

## First run

1. The app checks `/ext/apps_data/btcpc/identity.key` on the microSD card.
2. If the file is missing, a new ed25519 keypair is generated from the
   Flipper's hardware TRNG (STM32WB55 RNG peripheral).
3. The secret key (64 bytes) is written to `identity.key`.
4. The public key (32 bytes) is written to `identity.pub`.
5. The **Identity / Key** screen displays the public key as a 64-character hex
   string. Note this down or photograph it — you need it for registration.

Key files on the Flipper microSD:

```
/ext/apps_data/btcpc/identity.key   — 64-byte ed25519 secret key (keep private)
/ext/apps_data/btcpc/identity.pub   — 32-byte ed25519 public key
```

## Pairing with the phone

1. Open the BTCPC mobile app on your phone.
2. Navigate to **Devices → Pair Flipper**.
3. The phone app scans for the Flipper's BLE advertisement (device name `BTCPC`).
4. On pairing, the phone submits a `DeviceRegister` chain entry containing the
   Flipper's public key (read from `identity.pub` or typed in manually).
5. Once the entry is sealed, the Flipper's signatures are accepted by the network.

Auto-reconnect: after the initial pairing, the Flipper stores the paired
device's BLE address in `/ext/apps_data/btcpc/paired.addr` and reconnects
automatically on subsequent app launches.

## Key registration (manual fallback)

If the mobile app is unavailable, copy the 64-character hex public key shown on
the Identity screen and submit a `DeviceRegister` entry via the HTTP API:

```sh
curl -X POST http://localhost:4242/api/entries \
  -H 'Content-Type: application/json' \
  -d '{
    "type": "DeviceRegister",
    "pubkey": "<64-hex-public-key>",
    "device_type": "flipper-zero"
  }'
```

## Data flow

```
Flipper sensors → sign with ed25519 sk → BLE frame → phone app → chain entry
                                         ↑
                    phone → clock sync / GPS / entry hashes → Flipper
```

### Messages Flipper sends (signed)

| Type              | Content                                    |
|-------------------|--------------------------------------------|
| `SUBGHZ_OBS`      | Frequency (Hz), RSSI (dBm), modulation     |
| `RFID_SCAN`       | Protocol, card ID bytes                    |
| `NFC_SCAN`        | Tech, UID, ATQA, SAK                       |
| `IBUTTON`         | 64-bit ROM code, family byte               |
| `IR_CAPTURE`      | Decoded protocol/address/command (or raw)  |
| `HEARTBEAT`       | Battery %, uptime (s), firmware version    |

Every one of the above is captured, packed into its wire struct, signed with
the device key, and sent to the phone by a dedicated capture scene
(`scenes/btcpc_scene_subghz.c` / `_nfc.c` / `_rfid.c` / `_ibutton.c` / `_ir.c`)
or via the **Auto Rotate** scene, which cycles through all of them adaptively
(`btcpc_scheduler.c`). `data_hash` for the resulting `SensorReading` /
`SensorDataCommit` chain entry is `SHA-256(msg_type_byte || raw payload
bytes)` — computed independently on the Flipper (`btcpc_data_hash.c`, for
on-device display/logging) and on the phone (`payload_data_hash()` in
`android/rust/btcpc-miner/src/flipper_rx.rs`) from the same signed bytes, so
the hash is reproducible end-to-end. See `docs/SIGNING_INTEGRATION.md` for
the full two-hop trust model (device key signs the BLE frame; phone re-signs
the chain entry with the owner's posting key).

### Messages phone sends (no signature required)

| Type          | Content                                  |
|---------------|------------------------------------------|
| `ENTRY_HASH`  | 32-byte SHA-256 to rebroadcast via Sub-GHz |
| `CLOCK_SYNC`  | Unix timestamp in milliseconds           |
| `GPS`         | Lat/lon/alt/accuracy from phone GPS      |

## Crypto

Ed25519 via TweetNaCl (public domain, Daniel J. Bernstein et al.).
The `randombytes()` function is backed by `furi_hal_random_get()` which reads
the STM32WB55 hardware TRNG.

Stack usage: ed25519 sign requires approximately 1.5–2 KB of stack for the
SHA-512 computation and field arithmetic. The app is configured with an 8 KB
stack to provide headroom.

## Project layout

```
clients/btcpc-flipper/
  application.fam           — ufbt app manifest
  btcpc.c                   — app entry point, identity management
  btcpc.h                   — types, constants, shared state
  btcpc_ble.c/.h            — Serial BLE profile transport
  btcpc_data_hash.c/.h      — data_hash = SHA-256(msg_type || payload) helper
  btcpc_scheduler.c/.h      — adaptive sensor rotation (Auto Rotate scene)
  crypto/
    ed25519.c/.h             — TweetNaCl wrapper + randombytes() glue
    tweetnacl.c/.h           — TweetNaCl (public domain, ed25519 subset)
    sha256.c/.h              — standalone SHA-256 for data_hash
  protocol/
    btcpc_protocol.h         — BLE wire format definitions
    btcpc_protocol.c         — serialise / sign / parse messages
  scenes/
    btcpc_scene_main.c       — main menu
    btcpc_scene_identity.c   — public key display
    btcpc_scene_ble.c        — BLE pairing status
    btcpc_scene_subghz.c     — Sub-GHz RSSI capture
    btcpc_scene_nfc.c        — NFC (ISO14443) presence-scan capture
    btcpc_scene_rfid.c       — 125kHz RFID presence-scan capture
    btcpc_scene_ibutton.c    — iButton (1-Wire) presence-read capture
    btcpc_scene_ir.c         — infrared signal capture
    btcpc_scene_rotate.c     — adaptive auto-rotation across all sensors
  legacy/
    btcpc_wallet.c           — archived hardware wallet prototype (v0.2)
  test/
    test_scheduler.c         — host-buildable scheduler unit tests
    test_sha256.c             — host-buildable SHA-256 vector tests
    test_ed25519.c            — host-buildable ed25519 correctness tests
    test_sensor_payloads.c    — host-buildable capture->sign->hash tests
    test_host_ed25519.c       — host build of btcpc_ed25519_* (no furi_hal)
    test_host_crypto_shim.c   — host randombytes() for TweetNaCl in tests
```

## On-device hardware verification (open item)

The capture functions for NFC (`btcpc_nfc_poll_once`), 125kHz RFID
(`btcpc_rfid_read_once`), iButton (`btcpc_ibutton_read_once`), and IR
(`btcpc_ir_capture_once`) are written against the documented shape of the
Flipper SDK's `nfc`/`lfrfid`/`ibutton`/`infrared` worker APIs, but have not
been compiled against a real Flipper firmware SDK (no `ufbt`/ARM toolchain
was available in the environment that wrote them). Each is isolated to a
single function per sensor class specifically so that reconciling against
the real SDK on real hardware — build errors, exact worker call sequences,
API drift across firmware versions — only touches that one function; the
payload struct layout, signing, and `data_hash` computation around it are
hardware-independent and already verified by the host test suite in `test/`.
Sub-GHz (`btcpc_scene_subghz.c`) is the one capture path that reuses only
low-level, stable `furi_hal_subghz_*` calls already present before this
change and needs no further reconciliation.
