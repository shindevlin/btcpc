# Design: Bitcoin Mesh Relay (BMR)

Status: in-design
Proposal: novelproposals.md — Bitcoin Mesh Relay (BMR), 2026-05-22
Author: Artistic Engineer Agent handoff to Design Agent
Date: 2026-05-24

---

## Problem Statement

A device with no internet connection — a phone, embedded node, or laptop in an offline, censored, or surveilled region — needs to submit a signed Bitcoin transaction to the Bitcoin network. It has no clearnet access. It may have LoRa radio range to a BTCPC gateway node. The gateway node may or may not have full internet access, but it has Tor and possibly a Nostr relay connection.

BTCPC already operates a gateway infrastructure: Nebra Pis running LoRa hardware, connected to the BTCPC chain via libp2p. This design describes how that same infrastructure can serve as a Bitcoin transaction relay — outbound for transactions, and eventually inbound for block headers and compact filters — without touching BTCPC chain state.

---

## Scope: What This Is and Is Not

This is a **gateway service layered on top of the BTCPC transport stack**. It is not a BTCPC chain feature. Bitcoin payloads arriving over any transport must never:

- Be applied to BTCPC chain state via `chain.apply_entry()`
- Be re-broadcast on the `btcpc/entries` gossipsub topic
- Satisfy the `peer_count` check for user-submitted entries

BMR is a separate module with a separate dispatch path, a separate handle type, and no interaction with BTCPC consensus. The zero-peers chain-integrity hardline is unaffected. A BTCPC node with zero libp2p peers must still reject BTCPC chain entries — that rule does not apply to Bitcoin relay and must not be conflated with it.

---

## Version Boundaries

**V1 — Minimum Viable Relay (build this first):**
- Inbound path: raw Bitcoin transaction (hex) arriving over LoRa via existing `lorawan.rs` Semtech framer, tagged with magic prefix `BTC1`
- Outbound path: POST to Blockstream Esplora `/tx` endpoint over Tor SOCKS proxy
- No block header downlink
- No compact filter service
- No Nostr fallback in v1

**V2 — Multi-path and Confirmation:**
- Outbound path 2: publish transaction as Nostr kind 28333 ephemeral event (Nostr-over-Tor)
- Outbound path 3: direct Bitcoin P2P over Tor SOCKS to an operator-configured remote Bitcoin node
- Inbound: block headers at 80 bytes/block (~4 MB/year) served back over LoRa downlink
- Inbound: BIP158 compact filters for recent N blocks only (pruned window, not full history)
- Inbound transport: Nostr relay subscription to a Bitcoin header-relay channel (nostr-tx-broadcast / bitcoin-nostr-relay)

**Out of scope permanently for BMR:**
- Bitcoin Core on gateway hardware (too heavy — see Hardware Fit)
- BLE relay (Bitchat covers this; do not duplicate)
- Satellite downlink hardware (Blockstream Satellite SDR requires separate hardware and budget — treat as optional operator upgrade, not a required component)

---

## 1. Existing Transport Modules: Reuse Analysis

The four active transport modules are `tor.rs`, `i2p.rs`, `nostr_transport.rs`, and `lorawan.rs`. Each has two layers: a **plumbing layer** (connection setup, framing, ACKs, session management) and a **dispatch layer** (what happens to received payloads).

**Plumbing layers: all reusable. Dispatch layers: none usable as-is.**

### `tor.rs`

Current function: registers BTCPC API and P2P ports as a v3 onion hidden service. Inbound only — other nodes connect to BTCPC's onion address.

BMR needs: **outbound** SOCKS5 proxy connection through the Tor daemon to reach a remote Bitcoin full node or Esplora API. This is a different primitive. `tor.rs` does not implement SOCKS5 proxying.

Reuse: the Tor daemon running on the host is the shared resource. BMR connects to it as a SOCKS5 proxy (default `127.0.0.1:9050`) using `reqwest`'s `proxy()` builder. No changes to `tor.rs` required. BMR reads the Tor availability from the same `BTCPC_TOR=true` env var to decide whether Tor is available for egress.

New work: a `TorEgress` wrapper in the new `bitcoin_relay.rs` module that constructs a `reqwest::Client` with `proxy(reqwest::Proxy::all("socks5h://127.0.0.1:9050"))`.

### `i2p.rs`

Current function: SAM bridge session, receives BTCPC ledger entries as JSON datagrams over UDP, dispatches through `chain.apply_entry()`, re-broadcasts on `btcpc/entries`.

Reuse: the SAM datagram session setup (HELLO, SESSION CREATE, UDP socket) is the plumbing. That machinery is correct and could be mirrored for a Bitcoin I2P session. However, I2P adds latency (several seconds for tunnel build) and complexity that is not justified in v1.

Decision: I2P is **deferred to v3**. The outbound path via Tor is sufficient for v1 and v2. I2P adds an alternative for jurisdictions where Tor is actively blocked; note it as a future transport option.

### `nostr_transport.rs`

Current function: publishes BTCPC ledger entries as kind 21337 Nostr events, subscribes to kind 21337 events from a relay pool, dispatches received events through `chain.apply_entry()`.

Reuse: the `nostr-sdk` client setup (keys, relay pool, `connect()`, `subscribe()`) is the plumbing. A Bitcoin relay Nostr path uses **kind 28333** (proposed ephemeral event kind for raw Bitcoin transactions, from the bitcoin-nostr-relay and nostr-tx-broadcast projects). This is a separate kind number, a separate filter, and a separate event format — the content is raw hex tx, not a serialized LedgerEntry.

A `BitcoinNostrHandle` in `bitcoin_relay.rs` would hold its own `nostr_sdk::Client` connected to a separate or shared relay pool. Publishing kind 28333 to Nostr relays that support ephemeral events is the v2 fallback when Tor+Esplora is unavailable.

Reuse: the `Keys::generate()` + persist-to-RocksDB pattern from `load_or_generate_keys()` can be copied exactly for the Bitcoin relay Nostr identity. The relay pool setup pattern is identical.

Decision: **v2 feature**. Wire format design must account for it now but do not implement in v1.

### `lorawan.rs`

Current function: Semtech UDP Packet Forwarder framing. Receives `PUSH_DATA` from the Nebra/sx1302 gateway, decodes base64 LoRa payloads, dispatches based on payload shape:
- 32-byte binary: BTCPC entry hash lookup
- `{"entry":` prefix: inline LedgerEntry JSON

The Semtech framing, ACK/PULL_RESP machinery, gateway MAC tracking, and downlink queue are all directly reusable. BMR introduces a third payload shape.

**The key change: add a third payload case to `handle_lora_payload()`.**

```
else if payload.starts_with(b"BTC1") {
    // Route to bitcoin_relay module — do not touch chain
    bitcoin_relay_handle.relay_tx(&payload[4..]).await;
}
```

This is a one-line branch addition to `lorawan.rs` and a `BitcoinRelayHandle` added to the LoRaWAN run context. The Semtech framing is not changed at all.

Decision: `lorawan.rs` Semtech framer is **directly reusable**. The dispatch function needs one additional branch. The rest is unchanged.

---

## 2. New Module Design: `bitcoin_relay.rs`

### Module Location

`rust/btcpc-node/src/bitcoin_relay.rs`

This is a feature-gated module, enabled by `BTCPC_BITCOIN_RELAY=true` env var. If the env var is not set, the module returns a no-op handle and no goroutines are spawned.

### Public API

```rust
pub struct BitcoinRelayHandle {
    tx: mpsc::Sender<Vec<u8>>,  // raw Bitcoin tx bytes (not hex; decoded at ingest)
}

impl BitcoinRelayHandle {
    /// Queue a raw Bitcoin transaction for relay. Non-blocking; drops if full.
    pub fn relay_tx(&self, raw_tx: Vec<u8>) {
        let _ = self.tx.try_send(raw_tx);
    }
    
    /// Returns a no-op handle when the module is disabled.
    pub fn noop() -> Self { ... }
}

pub async fn start_bitcoin_relay() -> BitcoinRelayHandle { ... }
```

The handle is `Clone`. The channel depth is 64 transactions — generous given LoRa's throughput constraints (a full transaction at SF7BW125 takes multiple frames).

### Ingest: Transaction Validation

Incoming raw bytes from LoRa (after stripping the `BTC1` prefix) are:
1. Hex-decoded if needed (originating devices may send hex over the air)
2. Parsed with the `rust-bitcoin` crate: `bitcoin::Transaction::consensus_decode()`
3. If parse fails: silently drop, log warning with byte length
4. If parse succeeds: queue for egress

Validation is **structural only** — the gateway does not have UTXO state, so it cannot validate that inputs exist or are unspent. The Bitcoin network handles full validation. The gateway's job is to reject obviously malformed payloads (random noise, truncated frames) before spending a Tor circuit on them.

This is the correct level of validation. Do not attempt fee estimation, script validation, or mempool checks at the gateway.

### Chunking

A Bitcoin transaction can be 250 bytes (simple P2PKH) to several hundred kilobytes (complex scripts, CoinJoin). LoRa at SF7BW125 has a maximum payload of 242 bytes. A standard transaction must be chunked.

BMR chunk wire format (all bytes, transmitted over LoRa as the base64-encoded LoRa payload):

```
Magic:      BTC1        (4 bytes, literal ASCII)
Session:    [8 bytes]   random session ID, same across all chunks for this tx
ChunkIdx:   [1 byte]    0-indexed chunk number
TotalChunks:[1 byte]    total number of chunks (max 255 = ~60 KB; sufficient)
Payload:    [N bytes]   chunk data, N = min(raw_tx_remainder, 228)
Checksum:   [2 bytes]   CRC16 of (session + chunk_idx + total + payload)
```

Total overhead per chunk: 16 bytes. Usable payload per chunk: 226 bytes. A 250-byte transaction requires 2 chunks. A 10 KB CoinJoin requires 45 chunks.

Chunk reassembly happens at the gateway in `bitcoin_relay.rs`. The gateway holds an in-memory map of `session_id → (total_chunks, received_chunks[])`. On receipt of the final chunk, if all chunks are present and CRC validates, the full transaction is reconstructed and enqueued for egress. Session state is held in memory only, not in RocksDB — incomplete sessions are dropped after 60 seconds.

### Egress: V1 Path — Blockstream Esplora over Tor

Blockstream Esplora API endpoint: `https://blockstream.info/api/tx` (mainnet) or `https://blockstream.info/testnet/api/tx` (testnet).

POST method: `Content-Type: text/plain`, body: lowercase hex-encoded transaction.

The `reqwest::Client` is built once at module startup with a Tor SOCKS5 proxy configured. If `BTCPC_TOR=true`, the proxy is `socks5h://127.0.0.1:9050` (hostname resolution via Tor). If Tor is not available, the egress loop logs a warning and drops the transaction — it does not fall back to clearnet. Clearnet fallback would defeat the privacy model.

Retry policy: 3 attempts with exponential backoff (1s, 3s, 9s). On persistent failure: log the txid (sha256d of raw bytes), drop.

Env vars introduced:
```
BTCPC_BITCOIN_RELAY=true           — enable the module
BTCPC_BITCOIN_RELAY_ESPLORA_URL    — override Esplora endpoint (optional)
BTCPC_BITCOIN_RELAY_NETWORK        — "mainnet" or "testnet" (default: mainnet)
```

### Egress: V2 Path — Nostr Kind 28333

When Esplora POST fails after retries, v2 falls back to publishing the raw transaction as a Nostr kind 28333 ephemeral event to relays that support it.

Event format per bitcoin-nostr-relay convention:
```
kind: 28333
content: <lowercase hex-encoded raw tx>
tags: [["network", "mainnet"]]
```

The Nostr client for BMR uses its own keypair (not the BTCPC chain Nostr identity). Generated once, stored in RocksDB as `bitcoin_relay:nostr_key`.

Published to relays that are known to accept kind 28333 — separate from BTCPC's relay list. A default list of known Bitcoin-tx-relay-friendly relays will be maintained in the env var `BTCPC_BITCOIN_RELAY_NOSTR_RELAYS`.

### Header Downlink: V2

Block headers arrive at the gateway via a Nostr subscription to a Bitcoin header relay channel (to be specified) or from the operator's own connection to a Bitcoin full node. At 80 bytes/header and one block every ~10 minutes, the sustained data rate is negligible.

Inbound headers are queued for LoRa downlink exactly as BTCPC entry hashes are today: the `LoraWanHandle::queue_hash()` pattern is mirrored by a `queue_header()` call that sends the 80-byte header block as a LoRa downlink in PULL_RESP frames.

Header payload format over LoRa:
```
Magic:      BTCH        (4 bytes, "Bitcoin Header")
Height:     [4 bytes]   uint32 LE block height
Header:     [80 bytes]  raw Bitcoin block header
Total:      88 bytes — fits in a single SF7BW125 frame (max 242 bytes)
```

No chunking required for headers. Each header is one LoRa frame.

### BIP158 Compact Filters: V2

Compact block filters are ~10–20 KB per block and ~700 MB–1 GB per year for the full chain. This does not fit on a Pi 3 with 906 MB RAM serving filters continuously.

Design constraint: the gateway serves filters only for the **most recent N blocks** (configurable, default N=1000 = ~1 week). Filters are requested by the originating device with a specific filter query; the gateway fetches from a remote Esplora or `bitcoind` with `-blockfilterindex` enabled and serves the result.

Filter fetch is lazy (on-demand per device query, not prefetched). No local filter index is maintained on the gateway.

---

## 3. Hardware Fit: Nebra Pi (Pi 3, 906 MB RAM, sx1302_hal)

### What the Nebra Pi can run alongside `btcpc-node`

The Nebra Pi currently runs:
- `btcpc-node` binary (Rust, single binary, Axum + libp2p + RocksDB)
- `sx1302_hal` LoRa packet forwarder (C binary, low footprint)
- Tor daemon (if enabled)

Measured working set of `btcpc-node` at rest on a Pi 3: approximately 50–80 MB resident. Tor daemon: ~20 MB. sx1302_hal: ~5 MB. Total: ~105–125 MB active. Available headroom: ~780 MB.

**Bitcoin Core: ruled out.** Bitcoin Core at full sync requires 8–16 GB of state and 1–2 GB RAM working set. Even with pruning, the RAM requirement (~512 MB for a minimal node) is impractical alongside `btcpc-node` on a 906 MB device. Do not design for Bitcoin Core on the Nebra.

**BMR module footprint:** The `bitcoin_relay.rs` module is a few async tasks within the same `btcpc-node` process — no additional binary, no additional RocksDB instance. Memory overhead: in-flight transaction reassembly state (capped at 64 sessions × 60 KB = ~3.8 MB maximum), reqwest client with connection pool (~5 MB). Total: negligible.

**Tor SOCKS proxying:** Already running if `BTCPC_TOR=true`. No additional daemon required.

**sx1302_hal compatibility:** The Semtech UDP Packet Forwarder protocol used by `lorawan.rs` is the standard output format of `sx1302_hal`. No firmware changes required. The gateway hardware sees no difference — it is forwarding LoRa frames to UDP:1700 as it does today.

**Conclusion:** BMR v1 runs entirely within the existing `btcpc-node` process on the Nebra Pi. No additional hardware, no additional binaries, no additional services. The only new resource is the Tor SOCKS connection for egress.

---

## 4. Privacy Model

Three concrete privacy requirements and how each is met.

### P1: LoRa source identity must not be linked to Bitcoin txid

**Threat:** The gateway receives a Bitcoin tx from LoRa device with Meshtastic node ID `!abc12345`. If the gateway logs the mapping `{node_id: "!abc12345", txid: "aabbccdd..."}`, a gateway operator can link a physical device to a Bitcoin transaction.

**Mitigation:** `bitcoin_relay.rs` must not log node IDs, MAC addresses, or Meshtastic identifiers alongside txids. Log only: `bitcoin_relay: accepted tx, size=N bytes, session=<random session ID>`. The session ID is random per transaction and not persisted to RocksDB.

**Implementation note:** The `lorawan.rs` `handle_frame()` function currently logs the gateway MAC in `track_gateway()`. This is the LoRaWAN gateway MAC (the Nebra's own hardware), not the originating LoRa device's ID. This is acceptable — the Nebra knows its own MAC already. The originating device identity from Meshtastic headers, if present in the LoRa payload application layer, must be stripped by the originating device before transmission or stripped on ingest in `bitcoin_relay.rs`.

### P2: Egress must not link the gateway's IP to the transaction

**Threat:** If the gateway POSTs to Esplora over clearnet, the source IP of the POST is the Nebra's IP address. This links a physical hardware installation to a specific Bitcoin transaction at a specific time.

**Mitigation:** Egress is through Tor SOCKS5 only, with `socks5h://` (hostname resolution via Tor, not locally). The Esplora server and any Tor exit nodes see only a Tor exit IP, not the Nebra's IP. This is structurally identical to how Bitcoin Core's `-onion=` flag works.

**Hard rule:** If Tor is not available (`BTCPC_TOR` not set or Tor daemon unreachable), the BMR egress loop drops the transaction with a log warning. It does not fall back to clearnet. Clearnet fallback is opt-in only, via an explicit `BTCPC_BITCOIN_RELAY_ALLOW_CLEARNET=true` env var that must default to `false`.

### P3: In-flight payload is opaque to mesh intermediaries (V2)

**Threat:** If a LoRa mesh hop between the originating device and the BTCPC gateway is operated by a third party, that hop can read the raw transaction hex and extract destination addresses.

**Mitigation (v2):** The originating device encrypts the BMR payload to the gateway's published pubkey before chunking. The gateway holds the private key in RocksDB (`bitcoin_relay:encrypt_key`). Mesh hops see encrypted ciphertext only. Encryption scheme: X25519 key exchange + ChaCha20-Poly1305 AEAD (same as what Bitchat uses for BLE, well-understood in the Bitcoin community).

**V1 note:** V1 does not include payload encryption. The originating device and the BTCPC gateway are assumed to be in direct LoRa range (single hop) in v1. Multi-hop encryption is a v2 concern.

---

## 5. Minimum Viable Design: V1 Build Specification

Build only these components in v1. Everything else is future work.

### Components

1. **`bitcoin_relay.rs`** — new module in `rust/btcpc-node/src/`
   - `start_bitcoin_relay()` — returns `BitcoinRelayHandle` or noop handle
   - `BitcoinRelayHandle::relay_tx(raw_bytes: &[u8])` — queues assembled tx
   - Chunk reassembly state machine (in-memory, 60s TTL per session)
   - Egress loop: structural parse with `rust-bitcoin::Transaction::consensus_decode()`, then POST to Esplora via Tor SOCKS reqwest client

2. **One-line addition to `lorawan.rs`** — in `handle_lora_payload()`, add:
   ```rust
   } else if payload.starts_with(b"BTC1") {
       bitcoin_relay_handle.relay_tx(&payload[4..]).await;
   }
   ```
   The `BitcoinRelayHandle` is passed into the `run()` function as a new parameter.

3. **`main.rs` wiring** — call `start_bitcoin_relay()` before constructing `AppState`, pass handle into LoRaWAN setup.

4. **Wire format** — BMR chunk format as specified in Section 2.

### What Is Not Built in V1

- Nostr kind 28333 broadcast
- Block header downlink
- BIP158 compact filter serving
- Payload encryption
- I2P egress
- Any Blockstream Satellite hardware integration
- Any mobile client changes

### Crates Required

These are new dependencies — the Design Agent should specify exact versions when the Cargo.toml PR is drafted:

| Crate | Purpose |
|---|---|
| `bitcoin` (rust-bitcoin) | `Transaction::consensus_decode()` for structural validation |
| `crc16` or `crc` | CRC16 checksum for chunk integrity |

`reqwest` (with `socks` feature) and `tokio` are already in Cargo.toml.

Note: `reqwest` with SOCKS5 proxy support requires the `socks` feature flag: `reqwest = { features = ["socks", ...] }`. Check the current Cargo.toml — this feature may not yet be enabled.

### Definition of Done for V1

- A signed Bitcoin transaction created on a device with no internet, sent as BMR-chunked LoRa frames to a Nebra BTCPC gateway, successfully arrives in the Bitcoin mempool.
- Verified via Esplora txid lookup after relay.
- No BTCPC chain state is modified by the relay operation.
- Gateway logs show no node ID or clearnet IP linkage.
- The Nebra Pi does not show memory or CPU regression under `btcpc-node` when the BMR module is active.

### Smoke Test

On a single machine (no real LoRa hardware required for initial test):

1. Send a raw Bitcoin testnet transaction as UDP to the Semtech listener on port 1700, framed as a PUSH_DATA with a `BTC1`-prefixed payload
2. Observe `bitcoin_relay: accepted tx` log
3. Observe Tor SOCKS POST attempt in logs (even if Esplora rejects it on testnet)
4. Confirm no entry appears in `chain.apply_entry()` logs

---

## 6. Open Questions for the Design Agent

1. **reqwest `socks` feature:** Confirm whether the current `reqwest = { version = "0.12", ... }` in Cargo.toml includes the `socks` feature. If not, add it. This gates the entire v1 egress path.

2. **Chunk CRC:** CRC16/CCITT is standard and fits in 2 bytes. Is 16-bit sufficient or should this be CRC32 (4 bytes, stronger, still fits in the 242-byte LoRa frame with 6 fewer payload bytes per chunk)?

3. **Session ID generation:** 8 random bytes from `rand::thread_rng()`. Should this include a timestamp component for easier log correlation?

4. **Env var naming:** The proposed names follow the `BTCPC_*` prefix convention. Confirm no collision with existing env vars in `config.rs`.

5. **Testnet vs mainnet routing:** Should the BMR module respect the node's `chain_id` (btcpc-satoshi = testnet, btcpc-1 = mainnet) to auto-select the Esplora endpoint, or should the Bitcoin network be independently configurable? Recommended: independent configuration. BTCPC testnet nodes should be able to relay mainnet Bitcoin txs.

6. **Inbound LoRa frame size constraints:** The Nebra sx1302 at SF7BW125 allows 242 bytes payload. At SF9BW125 (longer range): 115 bytes payload. The chunk format must be re-evaluated for long-range configurations. Should chunk size be negotiated or fixed at the conservative 115-byte limit?

---

## 7. Agent Handoff Sequence

1. **Design Agent** (this document): produce wire format finalization, confirm crate list, resolve open questions 1–6 above. Output: finalized BMR wire format spec as an addendum to this document.

2. **Embedded Firmware Agent**: design the originating-device side. What does an ESP32 or Meshtastic node send? How does it chunk and tag the BMR payload? How does it receive and parse block header downlinks?

3. **Deploy Agent**: add BMR env vars to the Nebra Pi deployment scripts. Add Tor daemon installation and configuration to the gateway setup guide. Update `btcpc-nebra` service configuration.

4. **Bitcoin Compliance Agent**: review before any public deployment. Flag: Tor-mediated Bitcoin tx relay may trigger money transmission analysis in some jurisdictions. The module is best-effort relay, not custody — but the agent should confirm this framing is legally defensible in target jurisdictions.

5. **Mobile Client Agent (v2)**: design the Android wallet integration that allows a phone to submit a Bitcoin transaction via BTCPC BMR from within the BTCPC Android client.

---

## 8. Reference Projects

| Project | What to steal | What to ignore |
|---|---|---|
| btcmesh (eddieoz) | Python chunking logic for understanding packet loss patterns | Python runtime, Meshtastic DM approach (use Semtech UDP instead) |
| MeshtasticBitcoinCore_Bridge | Two-directional query/response pattern | Bitcoin Core dependency, manual delimiter scheme |
| bitcoin-nostr-relay (vnprc) | Kind 28333 event format, relay selection logic | Full relay server (we only need the client side) |
| nostr-tx-broadcast (benthecarman) | Kind 28333 publish pattern | (nothing to ignore; directly applicable) |
| Bitchat (permissionlesstech) | X25519+ChaCha20 payload encryption scheme (for v2) | BLE transport, phone-to-phone topology |
| rust-bitcoin | `Transaction::consensus_decode()` structural parse | Full Bitcoin node functionality |

---

*This document is a Design Agent handoff from the Artistic Engineer Agent. It is not a code specification and must not be committed as production code. The Design Agent should produce a finalized wire format addendum and a revised open-questions resolution before any implementation begins.*
