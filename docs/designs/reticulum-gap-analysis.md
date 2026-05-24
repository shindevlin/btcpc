# Design: Reticulum-rs Integration Gap Analysis

Status: analysis-complete
Author: Shin Devlin <shindevlin@proton.me>
Date: 2026-05-24

---

## Subject

Evaluate `reticulum-rs` (BeechatNetworkSystemsLtd/Reticulum-rs, MIT) as a protocol layer
over LoRa and other physical media in the BTCPC transport cascade, replacing the current
ad-hoc chunking in `rust/btcpc-node/src/lorawan.rs`.

This document answers six specific questions, then issues a go/no-go recommendation.

---

## Sources Examined

- GitHub: `BeechatNetworkSystemsLtd/Reticulum-rs` (v0.1.0, 150 commits, 273 stars)
- `Cargo.toml`, `src/packet.rs`, `src/transport.rs`, `src/iface.rs`, `src/lib.rs`,
  `src/iface/hdlc.rs`, `src/destination.rs`, examples: `link_client.rs`
- `reticulum.network/manual/understanding.html` (canonical protocol specification)
- Open issue tracker (31 open issues, 26 open PRs as of 2026-05-24)
- Issue #89 "Merge with Reticulum community" (open, no comments)
- Issue #85 "Feature matrix" (references external community wiki, unresolved)

---

## Q1 — Wire Protocol Completeness

### What the spec requires

The Reticulum wire format is:

```
[HEADER 2 bytes] [ADDRESSES 16 or 32 bytes] [CONTEXT 1 byte] [DATA 0-465 bytes]
```

Maximum physical packet: 500 bytes (MTU floor for any compliant interface).
Maximum data field: 465 bytes.
Link establishment: 3 packets, 297 bytes total (X25519 keypair, ECDH, Ed25519 proof).
Announce packets: destination hash + public key + app data + nonce + Ed25519 signature.

### What the Rust crate implements

`packet.rs` defines a `Packet` struct with:
- `Header` — encodes `IfacFlag`, `HeaderType`, `PropagationType`, `DestinationType`, `PacketType`
  via bit-packing into a single byte, plus hop-count byte. Matches spec.
- `PacketType` — `{Data, Announce, LinkRequest, Proof}`. Correct.
- `PacketContext` — all 21 variants present, including the full Resource sub-protocol:
  `Resource`, `ResourceAdvertisement`, `ResourceRequest`, `ResourceHashUpdate`,
  `ResourceProof`, `ResourceInitiatorCancel`, `ResourceReceiverCancel`. This matches
  the Python reference implementation's packet context table.

`transport.rs` implements:
- Announce propagation with hop-count tracking and duplicate suppression (180-second cache).
- Outbound link creation (`Transport::link`) with `LinkRequest` packet.
- Inbound link handling (`handle_link_request_as_destination`) with `LinkProof` response.
- Path table, announce table, link table — all present.
- Keep-alive (0xFF byte) and link state machine (Pending → Active → Stale → Closed).
- Retransmit announces at 1-second intervals.

Cryptography: `x25519-dalek 2.0.1`, `ed25519-dalek 2.1.1`, `aes 0.8.4` + `cbc 0.1.2`,
`hkdf 0.12.4`, `hmac 0.12.1`, `sha2 0.10.8`. This matches the spec's X25519 ECDH,
Ed25519 signatures, AES-256-CBC + HMAC-SHA256, HKDF derivation.

### Gap

One internal constant: `PACKET_MDU = 2048`. This is a framing-buffer size, not a data
field limit — the crate processes packets up to 2 KB internally. Whether the serializer
enforces the 465-byte data field cap at the wire boundary was not confirmed from source
inspection alone. This is a minor uncertainty, not a show-stopper; the spec-defined
addresses and header are correctly structured.

**Assessment: Wire protocol is substantially implemented. The full packet type and context
enum is present and matches the Python reference spec. Critical uncertainty is byte-for-byte
wire compatibility, which cannot be confirmed without a live interop test against a Python
Reticulum node.**

---

## Q2 — API Surface

The public API exposed by the crate:

```rust
// Instantiation
let transport = Transport::new(TransportConfig { name, identity, broadcast, retransmit, ... });

// Interface registration
transport.iface_manager().lock().await.spawn(SomeInterface::new(...));

// Destination creation and announce
let dest = transport.add_destination(identity, name).await;
transport.send_packet(dest.lock().await.announce(app_data).unwrap()).await;

// Link establishment
let link = transport.link(remote_announce).await;

// Receive announces (for discovering remote peers)
let announce_rx = transport.recv_announces().await;

// Receive data on a link
// (via LinkEvent / broadcast channel on Transport)
```

Interfaces provided out-of-the-box: `TcpClient`, `TcpServer`, `UdpInterface`.

The interface abstraction is a single-method trait:

```rust
pub trait Interface {
    fn mtu() -> usize;
}
```

The `InterfaceManager` wraps implementations, aggregates RX into a central channel, and
routes TX by interface ID. Adding a new interface requires implementing `Interface` + wiring
into `InterfaceManager::spawn`.

### What BTCPC would call

```rust
// Receive a payload from any Reticulum interface (LoRa, TCP, serial, ...)
let data_rx = transport.received_data_tx.subscribe();
while let Ok(received) = data_rx.recv().await {
    chain.apply_entry(&deserialize(received.data)).await;
}

// Send a payload over any available interface
transport.send_via_link(link, payload_bytes).await;
```

The pattern is sound for BTCPC's delivery-layer model.

### Gap

No LoRa interface exists in the crate. The only radio-adjacent interface is `kaonic.rs`,
which uses gRPC to communicate with Kaonic OFDM radio hardware at 869.535 MHz — a different
RF technology from LoRa's chirp spread spectrum. BTCPC would have to author a LoRa adapter
that bridges the existing Semtech UDP packet forwarder protocol (`lorawan.rs`) into
Reticulum's `Interface` trait. This is non-trivial: it requires translating Semtech's
push/pull model (gateway polls server) into Reticulum's TX/RX abstraction (stack pushes
frames to interface). The two models have opposite directionality by default.

**Assessment: API is usable for BTCPC's transport-only model. Sending and receiving payloads
through the stack is straightforward once a link is established. The LoRa adapter is the
primary integration cost and must be purpose-built.**

---

## Q3 — Fragmentation

### Spec position

The Reticulum Resource sub-protocol handles payloads that exceed the 465-byte data field.
A Resource is split into `ResourcePart` chunks, advertised via `ResourceAdvertisement`,
transferred with hash verification, and reassembled at the destination. The Resource layer
is transparent to the application — the caller sends a byte buffer, the stack fragments and
reassembles it.

### What the Rust crate implements

All seven Resource-related `PacketContext` variants are present:
`Resource`, `ResourceAdvertisement`, `ResourceRequest`, `ResourceHashUpdate`,
`ResourceProof`, `ResourceInitiatorCancel`, `ResourceReceiverCancel`.

However: the `destination.rs` source shows the `announce()` and `path_response()` methods
but does not expose a clear `send_resource(bytes: &[u8])` caller API. The link module
(`destination::link`) is declared but its send-side fragmentation implementation was not
visible in source inspection. The context variants being present is necessary but not
sufficient — the actual chunking and reassembly loop may be absent.

Issue #65 "Add support for requests" (January 2026, open) suggests higher-level request/
response over links is not yet complete, which typically depends on the Resource layer
working end-to-end.

### BTCPC payload sizes

A `LedgerEntry` serialized as JSON is typically 300–700 bytes depending on type. A sensor
registration entry with hardware attestation fields can exceed 1 KB. The 465-byte data
field is insufficient for most non-trivial entries. Fragmentation is not optional for BTCPC.

### Conclusion

Resource context variants exist in the packet layer. Whether end-to-end Resource
fragmentation and reassembly is fully implemented in the link layer could not be confirmed
from source inspection. Issue #65 being open is a yellow flag — if the Resource layer were
fully working, request/response would likely follow quickly.

**This is the single most important unknown. It must be verified with a live test before
any integration work begins.**

---

## Q4 — Hardline Check (Transport-Only, No Consensus)

The crate contains:
- `Transport`, `TransportConfig`, `Link`, `Destination`
- Path routing table, announce table, link state machine
- Keep-alive and retransmit logic

The crate contains no:
- Quorum primitives
- Epoch or block concepts
- Voting or leader election
- Clock synchronization
- Peer scoring for consensus weight

The design is a pure delivery stack. Received data arrives on a broadcast channel
(`received_data_tx`). The caller decides what to do with it. Funneling into
`chain.apply_entry()` is a one-liner.

The existing `lorawan.rs` already enforces the hardline: LoRaWAN peers never count toward
`peer_count`, entries go through `apply_entry()`, and re-broadcast goes back to libp2p.
Reticulum would sit at the same layer — it delivers bytes, BTCPC decides meaning.

**Assessment: The crate is structurally safe for BTCPC's hardline. No consensus logic
exists or could accidentally be invoked. The hardline is enforced at the BTCPC layer, not
the transport layer, which is correct.**

---

## Q5 — Interoperability with Python Reticulum Nodes

### The fork question

Issue #89 "Merge with Reticulum community" was opened April 17, 2026. The issue body
requests that BeechatNetworkSystemsLtd fork to the Reticulum-CE community org to avoid
"fragmented development." The issue is open with no comments, no assignee, no label, and
no linked PR. This means:

1. The Beechat implementation is currently developed independently of the Python reference.
2. There is no known coordination with markqvist (the Python author) or the community org.
3. Wire-format compatibility has not been documented, tested, or committed to publicly.

### What can be inferred

The cryptographic primitives match the spec (X25519, Ed25519, AES-256-CBC, HKDF, HMAC-SHA256).
The packet type and context enums match the spec. These are necessary conditions for interop.
They are not sufficient — byte ordering, field encoding, serialization format (the crate uses
MessagePack via `rmp` for some purposes alongside raw binary), and announce signature
construction must match the Python reference exactly.

Issue #92 "Lack of dynamic MTU support results in tcp_client: couldn't decode packet errors"
(April 2026, open) is a concrete interop symptom: the crate cannot currently decode packets
from a peer with a different MTU. In a mixed Rust/Python deployment, MTU negotiation
differences would produce exactly this failure mode.

Issue #82 "Transport node stops forwarding announces" (March 2026, open) means a Rust node
acting as a transport (relay) node would silently stop relaying, breaking any mesh that
depends on it.

### Conclusion

**Interoperability with Python Reticulum nodes is unconfirmed and structurally uncertain.**
The crate's wire format has not been validated against the Python reference. The community
convergence issue (#89) is open and unresolved. Issue #92 demonstrates an existing
decoding failure in a Rust-to-Rust scenario with MTU differences — a Python node would
amplify this problem. BTCPC cannot assume interop without a live test against a Python
Reticulum node.

---

## Q6 — Link Establishment Latency on LoRa

### The numbers

Reticulum link establishment requires 3 packets (Link Request, Link Proof, ACK).
At SF7BW125 (the default in `lorawan.rs`):
- Time-on-air for a 297-byte packet ≈ 200–260 ms.
- 3 RTTs at 250 ms each = 750 ms worst case.
- Add processing time: ~800–900 ms total before data flows.

The BTCPC epoch is 30 seconds. A node has 29+ seconds after link establishment to submit
a sensor hash or entry. 800 ms is not a blocker.

However, Reticulum links have a keep-alive mechanism and a stale timeout of 10 seconds
followed by a 5-second close window. On LoRa, a keep-alive exchange (2 packets) at SF7
costs ~500 ms of airtime. On a LoRa channel with multiple nodes, duty cycle limits
(1% in EU 869 MHz band) further constrain how many keep-alives can coexist. An always-on
Reticulum link over LoRa is not feasible for more than a handful of nodes simultaneously.

**Assessment: Initial link establishment latency (~800 ms) is acceptable for a 30-second
epoch. Persistent link keep-alives are incompatible with duty-cycle-constrained LoRa at
any meaningful node density. BTCPC's LoRa transport model should use ephemeral links or
a store-and-forward model, not persistent links.**

---

## Dependency Audit

The crate pulls in `tonic 0.13.0` and `prost 0.13.5` (gRPC code generation) solely for
the Kaonic radio interface. These are heavy build dependencies for a use case BTCPC does
not need (Kaonic is an OFDM radio product, not a LoRa gateway). Building `reticulum-rs`
into `btcpc-node` adds ~12 gRPC-related crates to `Cargo.lock` for zero runtime benefit.

The crate version is 0.1.0 with no semver stability commitment.

---

## Open Issue Summary (Risk-Relevant)

| Issue | Title | Impact on BTCPC |
|-------|-------|-----------------|
| #92 | Dynamic MTU support missing — decode errors | High — LoRa and TCP have different MTUs |
| #82 | Transport stops forwarding announces | High — breaks mesh relay |
| #95 | TCPClient teardown breaks TCPServer | Medium — affects mixed-interface deployments |
| #90 | Missing transport_identity in interface config | Medium — configuration completeness |
| #89 | Fork vs. community alignment | High — interop with Python nodes unconfirmed |
| #65 | Requests not yet supported | High — Resource fragmentation completeness uncertain |
| #87 | Direct SPI/SX1262 support not implemented | High — no native LoRa interface |

Seven risk-relevant open issues, four rated High. The crate is active but pre-stability.

---

## Reticulum-rs vs. Current lorawan.rs

| Capability | Current lorawan.rs | With reticulum-rs |
|---|---|---|
| Physical LoRa framing | Semtech UDP forwarder | Same (unchanged) |
| Addressing | None (MAC-based) | 128-bit SHA-256 truncated hashes |
| Link-layer encryption | None | X25519 ECDH + AES-256-CBC |
| Fragmentation | None (entries must fit one LoRa frame) | Resource layer (if complete) |
| Routing across media | None | Multi-hop path table |
| Keep-alives | None | 15-second window (duty-cycle risk) |
| Python Reticulum interop | N/A | Unconfirmed |
| LoRa interface in crate | N/A | Not implemented, must be built |
| Build weight added | None | ~12 extra crates (tonic, prost, ...) |
| Maturity | Stable, tested | v0.1.0, 7 high-risk open issues |

---

## Go / No-Go Recommendation

**NO-GO for integration into the current transport cascade.**

The recommendation is to defer Reticulum-rs integration until the following conditions
are met. This is a phased defer, not a permanent rejection.

### Blocking conditions (all must be resolved before integration)

**B1 — LoRa interface absent.**
The crate has no LoRa adapter. BTCPC would need to author a bridge between `lorawan.rs`'s
Semtech push/pull model and Reticulum's `Interface` trait. This is non-trivial. The bridge
must correctly handle duty-cycle constraints, Semtech downlink timing, and the inverted
request model. This work has no upstream crate to build on — it would be BTCPC-specific
and require testing against physical hardware (the Nebra Pi SX1302).

**B2 — Fragmentation completeness unconfirmed.**
Whether the Resource layer in the Rust crate does end-to-end fragmentation and reassembly
is unknown. Most BTCPC `LedgerEntry` types exceed the 465-byte Reticulum data field. If
Resource is incomplete, the entire value proposition of Reticulum over the current `lorawan.rs`
(which at least explicitly handles the 32-byte hash / inline-entry split) collapses.
A live test must confirm: send a 700-byte payload through a Rust-side Reticulum link,
receive it intact on the other end.

**B3 — Python interop unconfirmed.**
The value of a standardized mesh stack is interoperability with non-BTCPC nodes. Issue #89
(community convergence) is open and unresponded to. Issue #92 (MTU decode failures)
demonstrates active wire-format problems between Rust nodes. A live test against the Python
reference node must confirm packet exchange before claiming the interop benefit.

**B4 — Issue #92 (MTU) and #82 (announce forwarding) must be resolved.**
These are not edge cases — they are exactly the failure modes that appear on a heterogeneous
LoRa mesh with mixed SF settings (different MTUs) and sparse connectivity (announce
forwarding breaks). A transport with known announce-forwarding failures cannot be used as
a reliable propagation path for BTCPC entries.

### Non-blocking observations (informational)

- Latency (800 ms link establishment) is acceptable for a 30-second epoch. Not a blocker.
- The hardline (transport-only, no consensus) is structurally safe with this crate.
- Duty-cycle constraints mean persistent Reticulum links over LoRa are impractical at
  node densities above ~3. A store-and-forward model is required regardless of transport.
- The tonic/prost dependency should be feature-flagged or removed before integration.

### Recommended revisit path

1. Monitor issue #89 for upstream alignment decision. If the Beechat fork commits to
   community wire-format compatibility, interop becomes testable.
2. Watch for issue #92 (MTU) and #82 (announce forwarding) to close. Both are recent
   (2026-04, 2026-03) — the crate is active.
3. When #92 and #82 are closed: run a Python Reticulum node and a Rust Reticulum node
   on loopback TCP; exchange a 700-byte payload; confirm receipt and decoding. If that
   passes, B3 and B2 are partially resolved.
4. If the above tests pass: design the Semtech-UDP-to-Reticulum-Interface bridge as a
   standalone out-of-tree crate (`btcpc-lora-bridge`). Do not wire it into AppState until
   the bridge has its own integration test.
5. Keep current `lorawan.rs` in place. It is stable, tested, and correctly enforces
   the hardline. Reticulum would be an additive layer, not a replacement, at the point
   of integration.

### Current state of LoRa transport

`lorawan.rs` already does what matters:
- 32-byte hash downlink for hash-only propagation (fits one LoRa frame).
- Inline JSON entry for small entries with explicit size awareness.
- `apply_entry()` → libp2p re-broadcast on receive.
- Peer count never incremented (hardline enforced).

What it does not do:
- Encryption at the link layer (entries are signed by their submitter, but the LoRa hop
  is in plaintext).
- Multi-hop routing across multiple physical media.
- Addressing beyond gateway MAC.

These are real gaps. Reticulum would address all three. But adding an unproven stack with
known announce-forwarding bugs and unconfirmed Python interop to a production transport
cascade is worse than the current gaps — it introduces opaque failure modes on hardware
that is slow to debug.

---

## P2P Agent View

The instinct to standardize on Reticulum is correct. A shared encrypted mesh protocol
with sub-5-bps viability and multi-media routing is exactly what the LoRa transport phase
needs long-term. The BeechatNetworkSystemsLtd crate shows genuine implementation depth —
the packet type and context enums match the spec, the cryptographic primitives are right,
and the transport architecture is recognizable.

The problem is timing and completeness. Seven high-risk open issues, a v0.1.0 with no
stability commitment, no LoRa interface, fragmentation completeness uncertain, Python
interop unconfirmed, and tonic/prost dragged in for a radio product BTCPC doesn't use.
None of these are fatal to the crate's future. All of them are fatal to shipping it into
the BTCPC transport cascade in the next phase.

The current `lorawan.rs` is correct and sufficient for Phase 8. It should stay. Reticulum
integration should be tracked as a Phase 8b item, contingent on the four blocking conditions
above resolving in the upstream crate. No BTCPC code should be written against this API
until B1-B4 are cleared.
