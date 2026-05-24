# BTCPC Novel Proposals Log

This file is maintained by the Artistic Engineer Agent. Entries are appended chronologically.
Do not delete or overwrite existing entries. Mark stale entries as [STALE] in status.

Schema reference: novelproposals.db

---

# Proposal: Bitcoin Mesh Relay (BMR)

Date: 2026-05-22
Source: Synthesized from: github.com/eddieoz/btcmesh, github.com/BTCtoolshed/MeshtasticBitcoinCore_Bridge, github.com/permissionlesstech/bitchat, Blockstream Satellite docs, BIP157/BIP158, Locha Mesh, Reticulum/LXMF, nostr-tx-broadcast
Repo / URL: See individual sources cited in What I Found
Status: in-design
Tags: bitcoin, lora, meshtastic, offline, relay, spv, satellite, nostr, i2p, tor, mesh, transport, gateway, hardware-clients
Recommendation: F — Send to Design Agent

## One-Sentence Idea
A layered protocol that lets signed Bitcoin transactions reach the network and confirmed block headers return to a wallet using only LoRa mesh, Nostr, Blockstream Satellite, or Tor — no internet required at the originating device.

## What I Found

**TxTenna (Samourai + goTenna, 2018–2024)**
The original offline Bitcoin tx relay: signed transactions relayed over goTenna Mesh until a connected node forwarded to the network. Protocol: chunked hex over proprietary goTenna RF (900 MHz). Archived in April 2024 when Samourai was shut down on money laundering charges. The idea is open, the app is gone. goTenna hardware has been discontinued. This is a dead-end path for new builds.

**btcmesh (eddieoz, active 2024–2025)**
Python client + server over Meshtastic direct messages. Splits raw tx hex into ~200-char chunks, sends over LoRa, reassembles server-side, then pushes to Bitcoin RPC. 126 commits, 48 stars, 9 forks. Optionally routes the gateway through Tor for anonymity. No header sync. No SPV. Gateway-only, one-way outbound.

**MeshtasticBitcoinCore_Bridge (BTCtoolshed, active)**
Manual chunking with "+" / "==" delimiters over Meshtastic. Gateway machine runs Bitcoin Core. Also supports mempool fee queries back over the mesh ("-mempool-" command). 161 stars, 21 commits. Two-directional (query + send), but primitive and manual.

**Bitchat (Jack Dorsey / permissionlesstech, July 2025)**
Bluetooth mesh (not LoRa) chat app with Bitcoin tx relay as a secondary feature. Transactions travel device-to-device via BLE until a connected phone broadcasts to the network. X25519 + AES-GCM encryption. Range per hop: ~300m. No SPV. No header downlink. Significant because Dorsey's involvement will drive adoption but it is BLE-only — short range, phone-to-phone only. LoRa is not in scope.

**Locha Mesh (Bitcoin Venezuela, btcven, 2018–ongoing)**
Custom hardware (Turpial/Harpia) running a full mesh stack on license-free bands, with IPv6 support. More ambitious than Meshtastic — targets full block sync over mesh. 2024 reports show continued active development. Fragile: custom hardware in limited availability. The protocol concept is sound.

**Blockstream Satellite (Blockstream, active)**
Full block history + recent transactions broadcast from GEO satellites via DVB-S2, ~1.09 Mbps. Covers most of Earth. Hardware: small satellite dish + LNB + SDR or dedicated receiver. One-way downlink only (no uplink). Blocks received without any internet. This is the most mature and production-grade downlink solution. Critical insight: it works as the inbound half of the relay pair.

**BIP157 / BIP158 — Compact Block Filters (Neutrino)**
Modern SPV replacement. Full nodes generate Golomb-Rice Coded Set (GCS) filters (~10–20 KB/block) that let a light wallet check if any of its scripts appear in a block without downloading the whole block. Far better privacy than BIP37 bloom filters. Actively used in BDK, LDK, lnd/neutrino. Filters are the correct SPV primitive for the inbound data path.

**nostr-tx-broadcast (benthecarman) + bitcoin-nostr-relay (vnprc)**
Two separate Rust projects using Nostr as a censorship-resistant broadcast layer for Bitcoin transactions. Kind 28333 is the proposed ephemeral event kind for raw tx payloads. Both are active. This confirms Nostr as a viable alternative to direct Bitcoin P2P for broadcasting unconfirmed txs — and it works when a relay is reachable but the Bitcoin P2P network is firewalled or unreachable.

**Reticulum + LXMF (Markqvist, active)**
Transport-agnostic encrypted mesh stack. LoRa, packet radio, WiFi, serial — all unified. Message overhead: only 111 bytes (timestamped, signed, E2E encrypted, zero-conf routed). Not Bitcoin-specific but far more architecturally sound as a mesh transport than raw Meshtastic protocols. Could replace the ad-hoc chunking protocols in btcmesh/BTCtoolshed with a proper delivery layer.

**I2P / Tor as Bitcoin last-mile**
Bitcoin Core already supports running as a Tor hidden service and I2P destination. This means a gateway node on the BTCPC transport cascade can reach the Bitcoin P2P network anonymously, even if direct clearnet Bitcoin connections are blocked. BTCPC already has I2P and Tor in its transport cascade — this is a natural bridge.

## Why It Is New To Us
BTCPC has built a transport cascade (LoRa, Nostr, Matrix, I2P, Tor) for BTCPC chain entries. None of that cascade currently routes Bitcoin transactions or Bitcoin block data. Adding a Bitcoin relay gateway layer would make BTCPC nodes useful to Bitcoin users, not just BTCPC participants. No existing project covers the full stack (outbound tx over LoRa mesh + inbound header/filter sync from satellite + Nostr fallback + I2P/Tor anonymity at the gateway) in a single coherent design.

## Why It Could Help BTCPC
1. Demand creation: Any region with internet outage, censorship, or surveillance becomes a potential BTCPC node user — not because they want BTCPC, but because they need to send Bitcoin. This is a real, immediate use case with existing demand.
2. Network density: Every deployed BTCPC node that includes a BMR gateway becomes a community resource. Nodes become worth running even before BTCPC mining is mature.
3. Hardware leverage: Meshtastic LoRa hardware + a Raspberry Pi + a $30 satellite LNB is the full gateway stack. BTCPC already targets this hardware tier.
4. Transport reuse: The Nostr, I2P, and Tor delivery layers BTCPC has already built can carry Bitcoin tx payloads with near-zero additional work.
5. Credibility: Offering this makes BTCPC legible to Bitcoin-first builders and hardware communities who would otherwise ignore a new chain.

## How It Could Hurt BTCPC
1. Legal/regulatory: Relaying Bitcoin transactions is not inherently illegal, but operating infrastructure that routes transactions for others (especially through Tor) may attract regulatory scrutiny in some jurisdictions. Money transmission licensing is a risk if the gateway operator is seen as intermediating value.
2. Scope creep: This is Bitcoin infrastructure, not BTCPC chain work. If engineering time goes here before the BTCPC chain is stable, it is a distraction.
3. Dependency risk: Blockstream Satellite coverage has gaps. Meshtastic density is sparse in many regions. The relay works only where at least one gateway node exists with either internet or satellite downlink.
4. Responsibility ambiguity: If a user sends a Bitcoin transaction through a BTCPC-operated gateway and it gets stuck, lost, or double-spent, is BTCPC responsible? The protocol should make clear it is best-effort relay, not a custodial service.
5. Confusion with BTCPC chain: Some users may conflate "relaying Bitcoin txs" with BTCPC chain consensus. Documentation must be unambiguous.

## Where It Fits
hardware clients / gateway / P2P protocol / deployment

This is a gateway and relay protocol, not BTCPC core chain logic. It sits in the same layer as the transport cascade — it is one more delivery mode, but for a different chain.

## Build Path Without Coding
1. Research validation: Confirm btcmesh and BTCtoolshed bridge work end-to-end on current Meshtastic firmware. Document the actual packet loss rate and reassembly success rate.
2. Design sketch: Produce wire format specification for BMR packets (chunk header, sequence, session ID, checksum). Define the SPV header sync message format.
3. Prototype plan: BTCPC node running as Meshtastic gateway + Blockstream Satellite receiver as the downlink. Nostr relay as fallback broadcast path.
4. Required agents: Design Agent (wire format) → Embedded Firmware Agent (Meshtastic/ESP32 integration) → Deploy Agent (gateway deployment scripts) → Mobile Client Agent (Android wallet integration).
5. Definition of done: A signed Bitcoin transaction created on a device with no internet can reach the Bitcoin mempool via Meshtastic mesh → BTCPC gateway node → Nostr or Tor. Block headers can be received back via Blockstream Satellite or Nostr. SPV confirmation works without internet on the originating device.

## Required Agent Handoff
Design Agent — needs to produce the formal wire format specification for BMR packets, the SPV header relay message format, and the multi-path gateway architecture diagram before any prototype begins.

## Scores
Novelty: 7
BTCPC Fit: 7
Buildability: 8
Hardware Leverage: 9
Software Leverage: 8
Verification Value: 5
Demand Creation: 9
Risk: 4
Agent Handoff Clarity: 9
Time Horizon: 3

## Final Recommendation
F — Send to Design Agent

## Orchestrator Summary
BTCPC's existing LoRa/Nostr/I2P/Tor transport cascade can be extended to relay signed Bitcoin transactions and receive Bitcoin block headers — no new hardware required, no consensus changes. A Bitcoin Mesh Relay (BMR) gateway on each BTCPC node would give the project immediate real-world utility: Bitcoin users in offline, censored, or surveilled regions could send transactions through BTCPC-operated LoRa mesh nodes. The outbound path (tx → Meshtastic → gateway → Tor/Nostr → Bitcoin P2P) is well-validated by btcmesh and btchat. The inbound path (block headers → Blockstream Satellite → gateway → Meshtastic → wallet) is already production-grade. BIP158 compact filters are the correct SPV primitive for confirming payments without a full node. The Design Agent should produce a formal wire format and multi-path architecture document as the first deliverable. This is not a BTCPC chain concern — it is a gateway service layered on top. Time horizon: near-term prototype.

---

# Proposal: Reticulum Transport Layer (RNS)

Date: 2026-05-24
Source: github.com/markqvist/Reticulum, github.com/BeechatNetworkSystemsLtd/Reticulum-rs, crates.io/crates/reticulum-rs, reticulum.network/manual/whatis.html
Repo / URL: https://github.com/markqvist/Reticulum
Status: new
Tags: reticulum, rns, transport, mesh, lora, packet-radio, encrypted, rust, p2p, censorship-resistant, low-bandwidth, gateway
Recommendation: H — Send to P2P Protocol Agent

## One-Sentence Idea
Reticulum is a transport-agnostic encrypted mesh stack with a published Rust crate that could give BTCPC nodes a unified, cryptographically authenticated gossip layer operating over LoRa, packet radio, serial, TCP, and I2P with a single wire format and 5 bps minimum bandwidth.

## What I Found

**Reticulum Network Stack (markqvist, active — v1.3.1 as of 2026)**
The canonical Python implementation. Transport-agnostic: runs over LoRa (via RNode hardware), packet radio TNCs, serial links, TCP, UDP, and I2P. Uses X25519 key exchange, Ed25519 signatures, AES-256-CBC, HMAC-SHA256. Link establishment costs only 297 bytes across 3 packets. Link keepalive costs 0.44 bits/second. Minimum viable medium: 5 bps half-duplex with 500-byte MTU. In active development. 4,000+ GitHub stars. Used in real mesh radio deployments. The Python reference implementation is production-grade.

**BeechatNetworkSystemsLtd/Reticulum-rs (MIT, active 2025–2026)**
A Rust implementation of the full Reticulum stack. Published as a crate on crates.io. Approximately 150 commits. Includes modules for cryptography, identity, destination, packet processing, buffer, transport, and interfaces. Designed explicitly for "embedded and constrained deployments." Maintained by Beechat Network Systems Ltd, a commercial entity building on Reticulum. This is the critical finding: a maintained Rust crate exists, which makes direct integration into BTCPC's Rust binary feasible without FFI or subprocess delegation.

**Erethon/reticulum-packet-rs**
A separate minimal Rust library for parsing Reticulum packets. 11 commits, 11 stars. Author describes it as "proof of concept" and "quick and dirty hack." Not production-grade. Useful only as a reference for wire format parsing. The BeechatNetworkSystemsLtd implementation is the serious Rust candidate.

**LXMF (Lightweight Extensible Messaging Format)**
The application layer built on top of Reticulum for store-and-forward messaging. Used by the Sideband and Nomad Network apps. Not required for BTCPC — BTCPC would use the raw Reticulum transport layer, not LXMF messaging.

**RNode Hardware**
Open-source LoRa transceiver specifically designed for Reticulum. Based on common LoRa modules. Buildable from off-the-shelf hardware or purchasable. BTCPC already targets Meshtastic LoRa hardware — RNode is a compatible alternative that gives native Reticulum framing.

## Why It Is New To Us
BTCPC's current LoRa transport (Semtech UDP / LoRaWAN) uses a proprietary chunking approach layered on top of LoRa. It is not encrypted at the transport layer and does not self-route. Reticulum would replace this with a properly designed encrypted transport that handles link establishment, authentication, and routing transparently across all physical media. The network already has multiple transports that BTCPC could consolidate: LoRa, serial, TCP, I2P — all unified under one Reticulum interface with no change to chain logic. The Rust crate is the enabling fact: no FFI, no subprocess, direct inclusion in the Rust binary.

## Why It Could Help BTCPC
1. Single encrypted transport API across all physical media: LoRa, serial, TCP/IP, I2P, and future interfaces all look identical to chain code above the Reticulum layer.
2. Transport-layer authentication included: X25519 + Ed25519 means BTCPC entries arrive with a cryptographic transport guarantee, separate from and complementary to the entry's own Ed25519 signature.
3. 5 bps minimum bandwidth means it works over the worst LoRa configurations, degraded serial links, and extremely congested bands.
4. Link establishment in 297 bytes: viable even on slow modes (WSPR-class links cannot carry it, but JS8Call and standard LoRa spreading factors can).
5. Existing Reticulum deployments provide immediate bootstrap peers for BTCPC nodes that participate in the RNS mesh.
6. The RNode hardware is an alternative to Meshtastic-dependent LoRa gateways — it runs raw LoRa with Reticulum framing, no Meshtastic firmware dependency.

## How It Could Hurt BTCPC
1. The Rust crate (BeechatNetworkSystemsLtd) is a commercial entity's implementation — not markqvist's reference. Feature parity with the Python original is unverified. If the Rust impl has gaps in the protocol, BTCPC nodes may fail to interoperate with the broader Reticulum network.
2. Reticulum's link-establishment protocol adds latency: 3 round-trips to establish a link before data flows. On a slow LoRa channel this could be 30+ seconds per new peer. Bad for fast epoch propagation (BTCPC's epoch is 30 seconds).
3. The hardline must be restated: Reticulum is a transport only. BTCPC must never form quorum or make consensus decisions based on Reticulum neighborhood topology. Reticulum peers are transport peers, not consensus peers.
4. The 500-byte MTU is a lower bound. BTCPC epoch seal entries may exceed this — fragmentation handling must be verified in the Rust crate.
5. Adding another transport library increases binary size and surface area. The BeechatNetworkSystemsLtd crate has not been security-audited for BTCPC use.

## Where It Fits
P2P protocol / gateway / hardware clients / transport

## Build Path Without Coding
1. Research validation: Pull the BeechatNetworkSystemsLtd crate, read its API surface against the Python reference implementation docs. Identify any missing primitives (link-layer framing, AnnouncePacket, ResourceAdvertisement).
2. Design sketch: Draft a `reticulum_transport.rs` interface trait that matches the existing BTCPC transport cascade pattern. Define what a "Reticulum peer" looks like to the BTCPC P2P layer.
3. Prototype plan: A BTCPC node with Reticulum transport enabled should be able to gossip a signed LedgerEntry to another BTCPC node over an RNode LoRa link, with no changes to chain.rs or clock.rs.
4. Required agents: P2P Protocol Agent (crate evaluation + interface design) → Security Auditor (crate audit) → Embedded Firmware Agent (RNode hardware integration).
5. Definition of done: Two BTCPC nodes exchange epoch seals over a Reticulum LoRa link with no internet. Neither node treats the Reticulum link as a consensus quorum source.

## Required Agent Handoff
P2P Protocol Agent — evaluate the BeechatNetworkSystemsLtd/Reticulum-rs crate for API completeness and protocol fidelity against the Python reference. Determine whether it can carry BTCPC gossipsub messages or requires a wrapper. Do not prototype until the crate gap analysis is complete.

## Scores
Novelty: 8
BTCPC Fit: 8
Buildability: 6
Hardware Leverage: 8
Software Leverage: 8
Verification Value: 5
Demand Creation: 5
Risk: 5
Agent Handoff Clarity: 8
Time Horizon: 4

## Final Recommendation
H — Send to P2P Protocol Agent

## Orchestrator Summary
A maintained Rust crate (BeechatNetworkSystemsLtd/Reticulum-rs, MIT) implements the Reticulum Network Stack — a transport-agnostic encrypted mesh protocol with X25519/Ed25519 crypto, 297-byte link establishment, and a 5 bps minimum viable bandwidth. This could replace BTCPC's current ad-hoc LoRa chunking with a proper encrypted transport that works identically over LoRa, packet radio, serial, TCP, and I2P. The key constraint: Reticulum is transport-only, never quorum-forming. The P2P Protocol Agent should evaluate the Rust crate for API completeness and protocol fidelity before any integration work begins. Risk: crate is commercial (Beechat), not markqvist's reference — gap analysis required.

---

# Proposal: HF Amateur Radio Data Modes (JS8Call / Winlink / WSPR)

Date: 2026-05-24
Source: github.com/js8call/js8call, js8call.com, winlink.org, law.cornell.edu/cfr/text/47/97.113, wsprnet.org, miscdotgeek.com/js8call-getting-started
Repo / URL: https://github.com/js8call/js8call
Status: new
Tags: hf-radio, js8call, winlink, wspr, bpsk31, shortwave, amateur-radio, sdr, censorship-resistant, transport, beacon, low-bandwidth, fcc-part97
Recommendation: C — Add to research backlog

## One-Sentence Idea
BTCPC epoch seal hashes (cleartext, non-commercial) could be relayed over HF shortwave using JS8Call's TCP API as a last-resort censorship-resistant transport with global skywave propagation, while WSPR serves only as a liveness beacon (50 bits per 2-minute slot — not a data channel).

## What I Found

**JS8Call (Jordan Sherer / KN4CRD, GPL-3.0, active — v3.0.1 released May 2026)**
A derivative of WSJT-X that adds a messaging and network protocol layer on top of FT8's weak-signal FSK modulation. Achieves keyboard-to-keyboard messaging over HF. Features: store-and-forward relay, multi-hop relay, APRS inbound relay. Effective throughput: approximately 25–40 characters per minute (roughly 3–5 bytes/second usable payload). Has a TCP API on port 2442, JSON-formatted commands — confirmed by third-party integrations (js8web, Python library, Node.js library). Third-party applications can inject messages and receive decoded messages via this API without touching the radio firmware. Hardware required: any HF transceiver with USB digital audio capability (Icom, Yaesu, Kenwood, Elecraft, Xiegu) + a computer running JS8Call. Alternatively: a software-defined radio (SDR) transmitter (HackRF, ADALM-PLUTO) if you have a license.

**Winlink (WW1MCA Network, active)**
Email relay over HF/VHF ham radio. Stores and forwards email messages via Pactor, Vara HF, or Ardop modems. Used extensively for emergency communications. Part 97 analysis: Winlink explicitly prohibits business/commercial communications. The key legal issue for BTCPC is not encryption — it is the commercial prohibition in FCC §97.113(a)(3): operators may not transmit "communications in which the station licensee or control operator has a pecuniary interest." Relaying blockchain transactions where parties gain financial benefit is legally grey to flat-out prohibited depending on interpretation. Winlink is not the right path for BTCPC.

**WSPR (Weak Signal Propagation Reporter)**
50 bits per 2-minute transmission window. Carries: callsign, Maidenhead grid locator, power level. That is the full payload. Zero room for hash data. Confirmed dead end as a data channel. However: WSPR's propagation map (WSPRnet) provides real-time ionospheric maps. A BTCPC node could passively receive WSPR beacons to assess HF propagation before attempting a JS8Call relay — this is a secondary research note, not a proposal.

**BPSK31 / Olivia over shortwave**
Fldigi is the canonical open-source software for encoding/decoding BPSK31, Olivia, RTTY, and related modes. Audio in/out via soundcard or SDR. Olivia 8/250 achieves approximately 40 bps usable throughput. BPSK31 is narrower (31 Hz bandwidth) and slower. Both are audible modem tones that can be relayed over AM/shortwave broadcast audio — the receive stack is RTL-SDR + Fldigi or OpenWebRX. Shortwave Radiogram (WRMI, Florida) already broadcasts digital text over shortwave using these modes every weekend. This proves the concept but does not provide a path for BTCPC to inject content: WRMI is a licensed commercial shortwave broadcaster; BTCPC cannot inject content without a contractual relationship with the station.

**FCC Part 97 Regulatory Analysis**
The core constraints for using amateur radio for BTCPC:
- §97.113(a)(3): No transmissions where the operator has a pecuniary interest. Relaying transactions where token rewards flow to the operator is prohibited.
- §97.113(a)(4): No music, obscene language, etc. (not relevant).
- Encryption: The FCC prohibition is on encoding "for the purpose of obscuring meaning." ED25519 signatures on LedgerEntries are public-key authentication, not encryption for obscuring content. If entries are transmitted in cleartext with an attached signature, this is likely permissible — but this is a legal question, not a technical one.
- Bottom line: BTCPC epoch seal hashes (cleartext, no personal gain to the relay operator, experimental/self-training purpose) are the most defensible payload. Signed LedgerEntries where the relaying node earns mining rewards are the most legally exposed payload. Any production use requires legal review in each operator's jurisdiction.

## Why It Is New To Us
BTCPC has no HF radio transport. The Tor/I2P/Nostr cascade handles internet-based censorship resistance. HF skywave propagation provides something none of those transports can: communication across thousands of kilometres with no infrastructure whatsoever — no internet, no satellite dish, no LoRa gateway within range. JS8Call's TCP API is the integration point: a BTCPC relay daemon could connect to a locally running JS8Call instance and inject epoch seal hashes when all other transports fail.

## Why It Could Help BTCPC
1. True last-resort transport: If Tor, I2P, Nostr, LoRa, and Matrix all fail, HF skywave still works. The network becomes more censorship-resistant.
2. Propagation reality: A JS8Call station in Ireland (where BTCPC genesis is set) can reliably reach North America, South America, and Africa on 40m/20m/17m bands at low power.
3. Epoch seal hash is the correct payload: 32 bytes (SHA256 hash) fits in one JS8Call message. The receiving node can use it to detect desync and trigger resync via other transports.
4. TCP API means no radio firmware hacking: BTCPC builds a small relay daemon that connects to JS8Call's port 2442 and injects/receives messages. The radio layer is entirely handled by existing open-source software.
5. Hardware is commodity: A $30 RTL-SDR can receive. A $100–$300 used HF transceiver can transmit. This is within the hardware budget of a BTCPC gateway operator.

## How It Could Hurt BTCPC
1. Legal risk is real and jurisdiction-specific: §97.113 analysis above. Relaying anything commercially-motivated over amateur radio requires careful scoping. BTCPC must position any HF relay as experimental/educational, never as commercial relay.
2. Amateur radio license required: Transmitting requires a valid amateur radio license (Technician class does not cover HF in the US; General or Extra required). Cannot be deployed by unlicensed operators.
3. Throughput is extremely low: 3–5 bytes/second on JS8Call. Epoch seals can be relayed; full LedgerEntry sets cannot. This is a beacon/alert channel, not a full gossip transport.
4. Reliability: HF propagation varies with solar cycle, time of day, and ionospheric conditions. This cannot be a primary transport — it is a last-resort fallback only.
5. WSPR confusion: The proposal is sometimes framed as "use WSPR for BTCPC." WSPR carries 50 bits total per 2-minute transmission. It cannot carry even a truncated hash. Clarify explicitly that WSPR is not a data transport for BTCPC.
6. Winlink is not viable for this use case due to the commercial prohibition — do not design for Winlink.

## Where It Fits
P2P protocol / gateway / future research only

## Build Path Without Coding
1. Research validation: Run a JS8Call instance locally, connect to TCP API on port 2442, confirm message inject and receive work as documented. Measure actual throughput of a 32-byte epoch seal hash transmission.
2. Legal review: Before any production deployment, send the payload design (cleartext epoch seal hash, amateur experimental purpose, no financial gain to operator) to the Bitcoin Compliance Agent for jurisdiction analysis.
3. Design sketch: Define a BTCPC HF relay message format: epoch number + seal hash (32 bytes) + node identity prefix (8 bytes) = 40 bytes total. This fits in one JS8Call store-and-forward message.
4. Prototype plan: A shell script that polls BTCPC node API for new epoch seals and injects them into JS8Call via port 2442. No Rust required for the prototype — validate the concept first.
5. Required agents: Bitcoin Compliance Agent (regulatory clearance) → Design Agent (wire format for HF relay messages) → P2P Protocol Agent (integration with transport cascade).
6. Definition of done: A BTCPC node with a connected HF transceiver running JS8Call can broadcast epoch seal hashes on amateur HF bands. A receiving node with an SDR can decode the hash and use it to verify sync status. No financial gain flows to the transmitting operator.

## Required Agent Handoff
Bitcoin Compliance Agent — the legal question (can a licensed amateur operator relay BTCPC epoch seal hashes for experimental/educational purposes under FCC Part 97 without violating §97.113?) must be resolved before any implementation or deployment planning proceeds. The technical path is clear; the legal clearance is not.

## Scores
Novelty: 9
BTCPC Fit: 5
Buildability: 5
Hardware Leverage: 6
Software Leverage: 4
Verification Value: 6
Demand Creation: 3
Risk: 7
Agent Handoff Clarity: 8
Time Horizon: 7

## Final Recommendation
C — Add to research backlog

## Orchestrator Summary
JS8Call (GPL-3.0, v3.0.1 active, TCP API on port 2442) provides a clear integration point for BTCPC epoch seal hash relay over HF shortwave — a transport with global skywave propagation and zero infrastructure dependency. The payload is small enough: 40 bytes (epoch number + SHA256 hash + node prefix) fits in one JS8Call message. The TCP API means no radio firmware work; a daemon connects to port 2442. The blocker is legal: FCC §97.113 prohibits transmissions where the operator has a pecuniary interest, which may encompass BTCPC mining reward flow. Bitcoin Compliance Agent must rule on whether cleartext epoch hashes transmitted for experimental purposes are permissible before any deployment design begins. WSPR (50 bits/2 min) and Winlink (commercial prohibition) are confirmed non-starters for this use case. HF is a research backlog item until legal clearance is obtained.

---

# Proposal: AM/Shortwave Broadcast Block Header Downlink (Olivia/BPSK31 Modem Tones)

Date: 2026-05-24
Source: swradiogram.net, fldigi.sourceforge.io, rtl-sdr.com, wikipedia.org/wiki/Fldigi, sigidwiki.com/wiki/Olivia
Repo / URL: https://github.com/w1hkj/fldigi
Status: new
Tags: am-broadcast, shortwave, olivia, bpsk31, fldigi, rtl-sdr, one-way-downlink, block-header, censorship-resistant, sdr, receive-only
Recommendation: C — Add to research backlog

## One-Sentence Idea
Broadcast BTCPC epoch seal hashes as Olivia modem tones over AM or shortwave audio — a one-way, infrastructure-free downlink that any RTL-SDR can receive and Fldigi can decode, proved viable by the existing Shortwave Radiogram broadcasts on WRMI.

## What I Found

**Shortwave Radiogram (active, WRMI Florida, WINB Pennsylvania)**
A real, running, weekly shortwave broadcast that transmits digital text and images encoded as BPSK31/Olivia modem tones over licensed commercial shortwave stations. Produced by Dr. Kim Andrew Elliott. Received across multiple continents. Decoded by anyone with an SDR or shortwave receiver and Fldigi. This is not theoretical — this is a production system proving the concept.

**Fldigi (free software, LGPL-2.1, active development)**
Open-source software modem. Encodes and decodes BPSK31, Olivia, RTTY, MT63, Contestia, and many other digital modes via soundcard audio. Available on Linux, macOS, Windows. Source at github.com/w1hkj/fldigi. The receive stack is: RTL-SDR or shortwave receiver → audio output or virtual audio cable → Fldigi → decoded text. No hardware other than an RTL-SDR (~$25) required for receive.

**OpenWebRX (open source, active)**
Browser-based SDR receiver with built-in BPSK31 demodulator. Provides a web interface for decoding digital modes in-browser. Could serve as the BTCPC receive endpoint without requiring Fldigi installation.

**Olivia 8/250 mode specifics**
Olivia is an MFSK mode designed for extremely poor propagation conditions. Olivia 8/250 (8 tones, 250 Hz bandwidth): approximately 40 bps usable throughput, extremely low error rate under weak signal conditions. A 32-byte epoch seal hash transmits in roughly 6 seconds. An epoch number + hash + node ID (40 bytes) transmits in roughly 8 seconds. Fits comfortably within a 30-second BTCPC epoch window.

**The transmitter problem**
This is the gating constraint. NRSC-5 (HD Radio) is completely separate technology: OFDM, licensed broadcast stations, proprietary Xperi codec. Injecting content into an HD Radio station is not a viable path. For Olivia/BPSK31 modem tones over AM broadcast, the audio must be injected into a licensed broadcast transmitter. Three realistic paths:
a) Partner with an existing shortwave station (WRMI has accepted content from Shortwave Radiogram — a contractual relationship is possible but requires negotiation and ongoing fees).
b) Operate a licensed Part 15 AM transmitter (limited to 100 mW in the US, covering perhaps a neighbourhood — not useful for the global downlink use case).
c) Operate in a country with community radio licenses that permit data broadcasts (niche, jurisdiction-specific).

**NRSC-5 decode (receive-only, theori/nrsc5 project)**
theori/nrsc5 is an open-source RTL-SDR decoder for HD Radio (NRSC-5) digital audio and data subchannels. Allows decoding of HD Radio Program Service Data (station name, song title, album art). It is receive-only — there is no open transmitter. The relevant idea here is not transmitting NRSC-5 but whether existing HD Radio data subchannels could carry BTCPC data by agreement with a broadcaster. This is even more speculative than the Olivia path.

## Why It Is New To Us
BTCPC's current transport cascade is bidirectional and requires some form of networked channel (LoRa, Tor, Nostr, etc.). A one-way AM/SW broadcast downlink would be the first purely broadcast transport — no uplink required, no P2P, no internet at the receiver. The receiver just needs an SDR and software. This mirrors the Blockstream Satellite concept but over shortwave radio, with potentially better coverage in some regions (shortwave skywave propagation at night covers entire continents) and massively cheaper receive hardware ($25 RTL-SDR vs. satellite dish).

## Why It Could Help BTCPC
1. Zero-infrastructure receive: An RTL-SDR is cheaper than a LoRa module. If a BTCPC gateway broadcasts epoch seal hashes over shortwave, any node with an SDR can verify chain sync without internet.
2. Skywave coverage: At night, shortwave on 40m or 49m band covers thousands of kilometres from a single transmitter. One transmitter station in Ireland could cover Western Europe, North Africa, and the Eastern Seaboard of North America.
3. Fldigi is the receive stack: Open source, Linux-native, actively maintained, integrates via audio. BTCPC node could pipe Fldigi text output and extract seal hashes.
4. Proof of concept exists: Shortwave Radiogram demonstrates the exact technical stack BTCPC would use. The technology works.
5. Jamming-resistant: Unlike satellite (narrow beam, jammable at the uplink), shortwave broadcasts from multiple stations or frequencies are extremely hard to jam globally.

## How It Could Hurt BTCPC
1. Transmitter access is the hard problem: BTCPC cannot unilaterally broadcast on shortwave. A licensed station partner is required. This is a business/legal problem, not a technical one.
2. One-way only: The AM broadcast path is strictly receive-only for non-licensed operators. BTCPC nodes cannot use it as a bidirectional transport. It is a downlink only — useful for chain sync verification, not for gossip.
3. Time zones and propagation windows: Shortwave propagation depends on ionospheric conditions. A single frequency does not work 24/7 globally. Multiple frequencies needed for round-the-clock coverage.
4. The NRSC-5 path is blocked: Injecting into HD Radio subchannels requires broadcaster partnership and proprietary Xperi codec licensing. Not viable without a willing broadcaster partner.
5. Low urgency: BTCPC already has Blockstream Satellite (via BMR proposal) as a one-way downlink. AM/SW broadcast is an alternative to that — more accessible receive hardware, worse coverage reliability.

## Where It Fits
Gateway / P2P protocol / future research only

## Build Path Without Coding
1. Research validation: Set up an RTL-SDR, tune to WRMI on 9395 kHz or 15770 kHz during a Shortwave Radiogram broadcast, decode with Fldigi. Confirm the receive stack works end to end. Document actual decoded text quality.
2. Broadcaster outreach: Contact WRMI (Radio Miami International) to understand their content injection model — do they accept third-party data feeds? What is the cost? What format do they require?
3. Design sketch: Define a BTCPC shortwave broadcast message format: epoch number (4 bytes) + seal hash (32 bytes) + CRC (4 bytes) = 40 bytes. Encoded as Olivia 8/250, transmission duration ~8 seconds. Frequency and schedule published in BTCPC documentation.
4. Required agents: Design Agent (broadcast message format + frequency/schedule plan) → Bitcoin Compliance Agent (broadcast licensing analysis) → Deploy Agent (Fldigi integration script for receive-side nodes).
5. Definition of done: A BTCPC node with an RTL-SDR can receive epoch seal hashes from a shortwave broadcast and use them to verify chain sync. The transmit side is handled by a licensed broadcaster partner.

## Required Agent Handoff
No immediate handoff. This is a research backlog item. The technical stack is clear and proven (Fldigi + RTL-SDR + shortwave). The blocker is finding a broadcaster partner willing to carry BTCPC epoch data. Revisit when BTCPC has sufficient credibility to approach broadcasters. Design Agent should sketch the message format when bandwidth permits.

## Scores
Novelty: 8
BTCPC Fit: 4
Buildability: 3
Hardware Leverage: 7
Software Leverage: 5
Verification Value: 6
Demand Creation: 2
Risk: 5
Agent Handoff Clarity: 5
Time Horizon: 8

## Final Recommendation
C — Add to research backlog

## Orchestrator Summary
The technical stack for broadcasting BTCPC epoch seal hashes over shortwave AM radio is proven and open-source: Fldigi encodes Olivia modem tones, a licensed shortwave station carries the audio, RTL-SDR receivers decode it anywhere in the hemisphere for $25. Shortwave Radiogram on WRMI already does this with text content every weekend. A 40-byte BTCPC epoch payload (epoch number + seal hash + CRC) transmits in ~8 seconds in Olivia 8/250 mode. The blocker is not technical — it is access to a licensed transmitter. BTCPC cannot broadcast on shortwave without either (a) a licensed amateur radio station (see HF proposal for regulatory constraints) or (b) a commercial shortwave broadcaster partner. This is a research backlog item. No agent action needed until BTCPC has the credibility to negotiate a broadcaster partnership or until the HF amateur radio legal question is resolved.

---

# Proposal: Othernet / Dreamcatcher Satellite L-Band Downlink

Date: 2026-05-24
Source: othernet.is, wikipedia.org/wiki/Othernet, rtl-sdr.com/tag/othernet, rtl-sdr.com/tag/dreamcatcher, github.com/Othernet-Project/Dreamcatcher
Repo / URL: https://github.com/Othernet-Project/Dreamcatcher
Status: new
Tags: satellite, l-band, one-way-downlink, dreamcatcher, othernet, block-header, censorship-resistant, hardware
Recommendation: A — Ignore

## One-Sentence Idea
Othernet offered a $49–$70 L-band satellite receiver (Dreamcatcher) for one-way data broadcast downlinks, but Othernet Inc. went out of business in November 2025 — hardware may still be available secondhand but the satellite service is dead.

## What I Found

**Othernet Inc. (formerly Outernet) — Out of Business as of November 2025**
This is the definitive finding. Othernet Inc. is listed as out of business as of 1 November 2025. The company operated a one-way L-band satellite broadcast service that delivered weather data, news, and file content to low-cost receiver hardware (the Dreamcatcher board). The service has ended.

**Dreamcatcher Hardware History**
The hardware went through multiple generations. Early Dreamcatchers used an RTL-SDR to receive L-band (1.5 GHz). Later versions transitioned to a Ku-band LoRa stream. Dreamcatcher 3 used a built-in LoRa radio to receive their Ku-band satellite data stream — not a standard L-band LNB setup. The hardware is an Allwinner A20 (or similar) SoC with an integrated SDR/LoRa receiver, 512 MB RAM, SD card. Some Dreamcatcher boards remain available secondhand but without the Othernet satellite service, they are general-purpose embedded Linux boards with a LoRa radio — useful but not for satellite downlink.

**Comparison with Blockstream Satellite**
Blockstream Satellite is alive, actively maintained, covers most of the globe, and carries full Bitcoin blocks. It is the production alternative. The BMR proposal (already logged) addresses Blockstream Satellite integration. There is no comparable live satellite service for BTCPC to inject content into — Blockstream Satellite is a Bitcoin-specific service operated by Blockstream; it does not accept third-party data injection.

**The Content Injection Problem**
Even when Othernet was operational, content injection required a partnership with Othernet Inc. There was no open API for arbitrary third-party broadcast. BTCPC would have needed to negotiate a data feed relationship. With the company closed, this path is entirely blocked.

## Why It Is New To Us
It is not — this was on BTCPC's radar as a potential downlink option. The research confirms it is dead.

## Why It Could Help BTCPC
It cannot. Othernet is defunct. The hardware has no live satellite service. Secondhand Dreamcatcher boards are interesting as embedded Linux + LoRa hardware but the satellite downlink functionality is gone.

## How It Could Hurt BTCPC
Spending time designing for Othernet would be pure waste. The service is dead. Do not reference Othernet in any BTCPC architecture document as a live option.

## Where It Fits
Nowhere currently. Marked for monitoring only if a successor service emerges.

## Build Path Without Coding
None. Immediately ignore.

## Required Agent Handoff
No handoff. Close this item.

## Scores
Novelty: 1
BTCPC Fit: 1
Buildability: 1
Hardware Leverage: 2
Software Leverage: 1
Verification Value: 1
Demand Creation: 1
Risk: 2
Agent Handoff Clarity: 2
Time Horizon: 10

## Final Recommendation
A — Ignore

## Orchestrator Summary
Othernet Inc. went out of business November 2025. The Dreamcatcher satellite L-band downlink service is dead. Do not design for Othernet. If a satellite downlink remains desirable, the only live production-grade option is Blockstream Satellite (addressed in the BMR proposal). Close this item.

---

# Proposal: WiFi HaLow (802.11ah) as a BTCPC Gateway Transport

Date: 2026-05-24
Source: community.morsemicro.com, hackster.io/news/morse-micro-mm8108, morsemicro.com/evaluation-kits, store.rokland.com/products/alfa-ahpi7292s, cnx-software.com/2025/03/07/seeed-studio-wi-fi-halow, forum.openwrt.org, embeddedcomputing.com/morse-micro-mm8108
Repo / URL: https://github.com/MorseMicro/morse_driver
Status: new
Tags: wifi-halow, 802.11ah, sub-ghz, long-range, raspberry-pi, gateway, transport, low-power, iot, mesh, hardware
Recommendation: B — Watch

## One-Sentence Idea
WiFi HaLow (802.11ah) Raspberry Pi HATs are purchasable today at $30–$80, achieve 1–2 km range at up to 15 Mbps, and run standard Linux networking — making them a near-drop-in upgrade over LoRa for BTCPC gateway links where bandwidth and reliability matter more than absolute range.

## What I Found

**WiFi HaLow (IEEE 802.11ah) — Technology Overview**
Sub-1 GHz WiFi standard. Operates in license-exempt bands (900 MHz in the US, 863–868 MHz in Europe). Range: 1–2 km in open terrain (documented on multiple Pi HAT products). Throughput: up to 15 Mbps (ALFA AHPI7292S), up to 43 Mbps (Morse Micro MM8108). Power consumption substantially lower than standard 2.4 GHz WiFi. Designed for IoT applications with many devices and long range. Uses standard Linux 802.11 networking stack via a shim driver — appears to the OS as a standard WiFi interface.

**Available Raspberry Pi HAT Hardware (purchasable now)**
Multiple Pi HAT modules are available and shipping as of 2025–2026:
- ALFA Network AHPI7292S: Available from Rokland, RAKwireless, SparkFun, Getic. Up to 15 Mbps, Pi HAT form factor. Described as "world's first WiFi HaLow Pi HAT."
- AsiaRF MM610X-H06: Available on Amazon. Compatible with Pi 5, Pi 4B, Pi 3B. 1 km range documented.
- Heltec HT-HC01: 902–928 MHz, 1–2 km range.
- Seeed Studio Wio-WM6180: Mini-PCIe module for Pi SBCs, launched March 2025.

**Morse Micro MM8108 Developer Kit (mass production September 2025)**
The MM8108 is Morse Micro's second-generation HaLow SoC. Mass production confirmed September 2025. Raspberry Pi 4 + MM8108 evaluation kit (MM8108-EKH01-01) available via Mouser Electronics globally. Up to 43 Mbps. HaLowLink 2 evaluation platform (next-generation) available Q1 2026. This chip generation is also available in M.2 module form factor for router integration.

**Linux Driver Status — Critical Finding**
The HaLow driver is NOT in mainline Linux kernel. The driver ships as an out-of-tree module (dot11ah.ko) from MorseMicro's GitHub (github.com/MorseMicro/morse_driver). Kernel patches for S1G (Sub-1 GHz) features are required before building. Community reports: successful builds on Pi OS (Debian Bookworm) with kernel 6.6.51, and on Ubuntu 24.04 on Pi 4. Debian Trixie (which runs on BTCPC's Nebra Pi node at 192.168.68.75) ships kernel 6.x — driver should build on Trixie, but requires manual compilation and dkms setup. No mainline kernel integration confirmed as of May 2026. This is the key friction point.

**Comparison with LoRa**
LoRa at SF12/125kHz: ~250 bps, 5+ km range, license-free, ultra low power.
WiFi HaLow at MCS0: ~300 kbps, 1 km range, license-free, low power.
WiFi HaLow at MCS7: ~15 Mbps, 300 m range.
For BTCPC: HaLow wins on bandwidth (enough to carry full gossip traffic between gateway nodes), loses on raw range vs. LoRa. The key advantage over LoRa is that HaLow is standard WiFi — existing BTCPC TCP/libp2p stack works over HaLow without any protocol changes. No chunking, no LoRaWAN gateway, no Semtech UDP proxy.

**Nebra Pi Compatibility Note**
The Nebra Pi at 192.168.68.75 runs Debian Trixie (kernel 6.x). The ALFA AHPI7292S Pi HAT is compatible with Pi 4/5. If Morse Micro's out-of-tree driver builds cleanly on kernel 6.x (which community reports suggest it does on 6.6.51), the Nebra Pi could run HaLow as a secondary transport interface. No kernel upgrade required — driver compilation only.

## Why It Is New To Us
BTCPC's current LoRa transport is low-bandwidth and requires proprietary gateway hardware (Semtech UDP / ChirpStack). WiFi HaLow would give BTCPC a long-range transport that looks like standard WiFi to the node — libp2p/QUIC runs directly over it, no custom transport protocol needed. The 1 km range covers scenarios (farm, rural area, urban neighbourhood) where standard WiFi cannot reach and LoRa is overkill in its low-bandwidth constraint.

## Why It Could Help BTCPC
1. Drop-in for libp2p: HaLow presents as a standard network interface. The existing QUIC/TCP libp2p stack runs over it unchanged. No new transport code.
2. 1 km range with full bandwidth: Between two BTCPC gateway nodes (e.g., a Raspberry Pi cluster on a farm), HaLow provides 1 km links at megabit speeds. LoRa provides the same range at kilobit speeds.
3. Hardware is purchasable now: Multiple Pi HAT options available from Mouser, Rokland, Amazon. No waitlist, no custom order.
4. Nebra Pi is a direct test target: The existing Nebra Pi at 192.168.68.75 running Debian Trixie is a real test candidate for HaLow integration.
5. Mesh networking: HaLow supports 802.11s mesh networking. Multiple BTCPC nodes in a HaLow mesh would route traffic automatically without infrastructure.
6. IoT convergence: Verasens sensor nodes (ESP32, Arduino) with HaLow chips could connect directly to a BTCPC gateway at 1 km range — far beyond BLE and LoRa-class bandwidth for sensor data payloads.

## How It Could Hurt BTCPC
1. Out-of-tree driver: Manual kernel module compilation on every node. If a kernel update breaks the driver, the transport goes down. Not suitable for production without DKMS packaging and a CI test on each kernel update.
2. Not mainline: No distribution package exists. Every BTCPC node operator must manually build the driver. This raises the deployment barrier significantly.
3. Regulatory: HaLow uses the 900 MHz ISM band in the US, the 863–868 MHz SRD band in Europe. These are license-free but with power limits. Running at maximum power requires checking local regulations. Not a blocker, but an operator note.
4. Range vs. LoRa: At 1–2 km, HaLow is outranged by LoRa (5+ km) in typical deployments. For the "remote node" use case, LoRa wins on range. HaLow wins on bandwidth for medium-range gateway-to-gateway links.
5. New dependency: Adding a hardware-specific out-of-tree kernel module to BTCPC's deployment is a DevOps burden. The Deploy Agent would need to manage kernel module builds.

## Where It Fits
Hardware clients / gateway / P2P protocol / Verasens

## Build Path Without Coding
1. Research validation: Purchase one ALFA AHPI7292S Pi HAT (~$60 from Rokland), install on the Nebra Pi at 192.168.68.75, compile MorseMicro/morse_driver on Debian Trixie kernel 6.x, confirm interface comes up. Test link budget to a second Pi at 500m distance.
2. Design sketch: Define HaLow as a standard network interface transport in BTCPC's transport cascade — no new protocol, just an additional network interface that libp2p discovers. Document the DKMS packaging requirement.
3. Prototype plan: Two BTCPC nodes on the same HaLow network exchange epoch seals over libp2p QUIC. Measure latency and packet loss at 1 km.
4. Required agents: Deploy Agent (DKMS packaging for morse_driver, deployment script) → DevOps Agent (CI kernel module build test) → Embedded Firmware Agent (Verasens sensor nodes with HaLow chips).
5. Definition of done: The Nebra Pi BTCPC node (192.168.68.75) runs with a HaLow interface and forms a libp2p peer connection to a second BTCPC node at 500+ metre range. Driver builds cleanly via DKMS on Debian Trixie.

## Required Agent Handoff
Deploy Agent — the primary blocker is the out-of-tree driver packaging problem. The Deploy Agent should evaluate whether the MorseMicro morse_driver can be packaged as a DKMS module that survives kernel updates on Debian Trixie. If yes, HaLow becomes a viable transport to prototype. If not, revisit after mainline kernel inclusion.

## Scores
Novelty: 6
BTCPC Fit: 7
Buildability: 6
Hardware Leverage: 8
Software Leverage: 7
Verification Value: 4
Demand Creation: 5
Risk: 4
Agent Handoff Clarity: 8
Time Horizon: 4

## Final Recommendation
B — Watch (upgrade to D: Prototype Later after Deploy Agent evaluates DKMS packaging)

## Orchestrator Summary
WiFi HaLow (802.11ah) Pi HATs are purchasable now ($60 ALFA AHPI7292S, available from Rokland/SparkFun). They provide 1–2 km range at 15 Mbps — medium-range gateway links with full libp2p bandwidth, no custom transport code. The existing BTCPC Nebra Pi (192.168.68.75, Debian Trixie) is a direct test target. The blocker is the out-of-tree driver (MorseMicro/morse_driver on GitHub) — not in mainline Linux, requires manual DKMS packaging. The Deploy Agent should determine if DKMS packaging is feasible on Debian Trixie kernel 6.x. If yes, this becomes a near-term prototype: buy one HAT, compile the driver, and confirm a libp2p QUIC peer connection forms at 500m. HaLow does not replace LoRa (shorter range) but fills the gap between BLE (10m) and LoRa (5km) with full megabit bandwidth.

---

# Proposal: BTCPC Beacon — Belize AM/SW License Research

Date: 2026-05-24
Source: Belize PUC (puc.bz), Belize Broadcasting Authority (belizebroadcastingauthority.org), Belize National Assembly SI No. 76 of 2024, WIPO Lex (Broadcasting and Television Act Cap. 227), WRMI Radio Miami International (wrmi.net), NEXUS-International Broadcasting Association (nexus.org), Shortwave Radiogram (swradiogram.net), VOA Radiogram (voaradiogram.net), swcountry.be, FCC HF Broadcasting guidance, HFCC (hfcc.org)
Repo / URL: N/A — regulatory and infrastructure research
Status: assessed
Tags: beacon, shortwave, HF, AM, radio, broadcast, license, Belize, regulatory, SDR, MFSK, epoch, verification, infrastructure, offline
Recommendation: C — Add to research backlog (Belize license path); E — Prototype Now (WRMI airtime lease path)

## One-Sentence Idea
Broadcast sealed BTCPC epoch header hashes as a one-way radio beacon over shortwave or AM using an existing licensed transmitter, so anyone with an SDR and no internet can verify the chain tip.

## What I Found

### The Use Case Is Real and the Technology Is Proven

Nick Szabo and Elaine Ou demonstrated Bitcoin transaction relay over HF shortwave in 2017–2018. The Shortwave Radiogram project (successor to VOA Radiogram, produced by Dr. Kim Andrew Elliott) has broadcast digital text and images over shortwave since at least 2018, using MFSK32 and MFSK64 digital modes decodable with Fldigi on any computer connected to an SDR receiver. WRMI (Radio Miami International) carries Shortwave Radiogram on commercial transmitters. This proves the full stack from "digital data in" to "SDR + Fldigi decode out" is working in production today. A BTCPC epoch hash is 32 bytes (SHA-256) — trivially small for any digital HF mode. MFSK32 at ~42 characters/second could deliver a full epoch summary (hash + height + timestamp) in under two seconds of airtime.

### Regulatory Structure in Belize: Two Bodies, Not One

Broadcasting in Belize is split across two regulators, and any applicant needs to navigate both:

**1. Public Utilities Commission (PUC)** — governs spectrum/transmitter authorization under the Belize Telecommunications Act (Cap. 229). Issues Spectrum Authorizations. The SI No. 76 of 2024 (gazetted 11 May 2024, signed by PUC Chairman Dean Molina and Minister Michel Chebat) amends the fee schedule and explicitly lists broadcast categories in Schedule V including "Broadcast AM Radio Station District" and "Broadcast AM Radio Station National" as distinct Spectrum Authorization categories. The fee amounts in the schedule are unfortunately rendered as non-extractable images in the PDF — the row labels decoded cleanly but the numeric columns are blank in machine-readable output. The fee amounts remain unverified from primary source.

**2. Belize Broadcasting Authority (BBA)** — governs broadcast content licensing under the Broadcasting and Television Act (Cap. 227). The Act states "no person shall establish or operate any radio or television station except under and in accordance with a licence issued by the Minister." Fees are not codified — they are "such fee as the Minister prescribes." This is a discretionary framework, not a fee schedule. The Minister can price, reprice, or revoke at will.

The Act contains no foreign ownership restrictions or citizenship requirements on its face. However, the absence of a codified restriction in the statute does not mean the Minister lacks discretion to deny. The licensing process requires a written application to the BBA Board Chairman; the Board has four weeks to recommend; the Minister decides. This is a political and relationship-dependent process with no hard timelines and no codified appeal rights documented in the public-facing text.

### Belize Shortwave History: Dead End

Belize had one historical shortwave broadcaster: Radio Belize (call sign ZIK2), operating from the 1950s at up to 5 kW via a Collins transmitter. The dagger symbol on every entry in swcountry.be's database confirms ZIK2 is discontinued. There is no current shortwave broadcast infrastructure in Belize. Building HF in Belize means starting from a bare field: antenna farm, 5–50 kW transmitter, ITU/HFCC frequency coordination, mains power feed, and a physical site — all in a country with no existing shortwave tradition. This is a multi-year, multi-hundred-thousand-dollar greenfield infrastructure project for the SW path.

### Domestic AM: Limited Geographic Reach

AM ground-wave covers roughly 100–500 km by day, extending via sky-wave at night. A domestic AM station in Belize City reaches Central America and parts of the Gulf at best. It does not reach Europe, Asia, or most of South America. For a global chain-tip beacon, domestic AM is insufficient unless paired with internet relay. Nighttime sky-wave propagation is highly variable and not reliable for a timestamped verification service.

### The Belize IBC Company Path

Belize offshore IBCs can be formed in 24–48 hours for roughly $450–$1,500 USD (agent fees vary), with annual renewals of $500–$800 USD. Directors and shareholders need not reside in Belize. This is genuinely simple for the corporate formation step. However, the IBC formation does not solve the substantive blockers: you still need a transmitter site in Belize, a PUC Spectrum Authorization, and a BBA broadcast license with ministerial approval. The company formation is the easy part; the hard parts remain hard.

### Foreign Precedent: None Found

No evidence was found of any foreign entity obtaining a Belize broadcast license for a non-traditional purpose (data beacon, blockchain, or technical service) in the last decade. The Belize radio station ecosystem is small — a handful of domestic AM/FM stations serving the local population. Absence of precedent is informative: this path has never been walked, which means BTCPC would be navigating uncharted regulatory territory with a regulator who has no frame of reference for what a cryptographic beacon is.

### The Real Path: Airtime Lease on Existing Licensed SW Transmitters

Two commercial entities solve every Belize-license problem by simply not requiring a Belize license:

**WRMI (Radio Miami International, Okeechobee FL, USA)**
The largest privately-owned shortwave station in the Western Hemisphere. 14 transmitters (most 100 kW), 23 directional antennas covering 11 worldwide beam directions. Published rate: $1.00 per minute for block airtime purchases. No content review for non-hate-speech content. Fully licensed. Already carries Shortwave Radiogram — the exact digital-text-over-shortwave use case. A BTCPC beacon could be as short as 1 minute per epoch (30-second epoch = broadcast the hash in the subsequent minute). At $1/min, 48 broadcasts per day = $48/day = ~$1,440/month for continuous hourly coverage. More realistically: 4 broadcasts per day at strategic propagation windows = $4/day = $120/month.

**NEXUS-IBA (Milan, Italy)**
UN-recognized NGO operating shortwave transmitters from 10 to 300 kW covering Europe, Africa, Asia/Pacific, Middle East, Americas. No membership required. Pricing is quote-based ("just 1/6 of the lowest 1-minute rate of a US radio or TV spot"). Does not explicitly support digital-data-only transmissions — their IPAR service is described as audio/voice/music. A data beacon would require explicit negotiation with NEXUS about whether digital mode audio (MFSK tones played as an audio file) is acceptable content. NEXUS is the option if WRMI coverage footprint does not reach the target region.

**The Technical Stack Is Already Specified**

Transmit side: Generate epoch hash + height + timestamp, encode as MFSK32 audio file using Fldigi or a pure Rust MFSK encoder, deliver audio file to WRMI for scheduled playback at the agreed minute slot. No exotic hardware needed. The transmitter operator just plays an audio file.

Receive side: RTL-SDR dongle ($25), antenna (wire or whip), PC or Raspberry Pi running Gqrx or SDR# + Fldigi. Standard ham radio decoding software. No internet required. Any operator worldwide with this kit can receive and verify the chain tip beacon. This is the same receive stack already used by thousands of Shortwave Radiogram listeners.

### ITU and HFCC Coordination

For any entity operating its own shortwave transmitter, ITU coordination goes through the national administration (in Belize's case, the PUC), which then submits to the ITU Radiocommunication Bureau. The HFCC (High Frequency Coordination Conference) manages the seasonal frequency database for commercial shortwave broadcasters and holds coordination conferences twice yearly (A and B schedules). HF spectrum is extremely congested globally. A new broadcaster needs to find uncoordinated frequencies — a process that takes months and requires technical expertise. This is the correct technical path for a licensed transmitter, but it is a six-to-twelve month process and is irrelevant if using WRMI or NEXUS (they handle their own ITU coordination).

### Alternative Jurisdictions

No Caribbean or Central American neighbor offers a materially simpler path for owning a transmitter than Belize. Guatemala (SIT), Costa Rica (SUTEL), and Honduras (CONATEL) all have comparable regulatory frameworks requiring local physical presence, ministerial or agency approval, and ITU coordination for HF. None of them have existing shortwave infrastructure for sale or lease. The airtime-lease model (WRMI, NEXUS) is jurisdiction-agnostic and strictly superior for BTCPC's purposes.

## Why It Is New To Us

BTCPC has explored LoRa, Nostr, Matrix, Tor, I2P, and Blockstream Satellite as alternative transport layers. A one-way shortwave broadcast beacon has not been considered. The specific combination of: (1) WRMI $1/min commercial airtime, (2) MFSK32 digital audio encoding, (3) RTL-SDR receive-side, and (4) epoch hash as payload is novel to BTCPC and requires no chain modifications, no new protocol, and no regulatory licensing by BTCPC itself.

## Why It Could Help BTCPC

**Trust without connectivity.** An SDR operator in a country with no internet — or in a country where BTCPC's internet presence is censored — can tune to a known shortwave frequency, decode the current epoch hash, and verify it against their local chain state. This is censorship resistance at the physical layer.

**Credibility signal.** "BTCPC broadcasts its chain tip on shortwave" is a powerful public statement about the seriousness of the project. It is provably harder to fake or censor than any internet-based verification mechanism. It invites the existing SDR hobbyist and ham radio community into BTCPC's orbit — a technically sophisticated audience.

**No chain modification required.** The beacon consumes already-available data (sealed epoch hashes, which are public on the chain). The broadcast is a read-only side-channel. It cannot fork the chain, cannot introduce entries, and cannot weaken the zero-peer submission hardline.

**Hardware leverage.** Any RTL-SDR dongle ($25) plus a wire antenna becomes a BTCPC chain verification node. This extends the "useful old hardware" principle to radio hardware that tens of thousands of people already own.

**Proof of time.** Shortwave broadcasts are inherently timestamped by their transmission time. Radio operators who log reception (a common ham radio practice) create an independent, decentralized, paper-and-radio record of what the BTCPC chain tip was at a given time. This is a novel form of distributed timestamping.

## How It Could Hurt BTCPC

**Continuity dependency.** If BTCPC commits to a scheduled shortwave beacon and then misses broadcasts (due to transmitter scheduling conflicts, fee delinquency, or WRMI operational issues), it creates a public failure event. The beacon must be treated as a reliable service, not a demo.

**Content policy friction.** WRMI and NEXUS have content review rights. A cryptographic data beacon is unusual content. WRMI's acceptance is not guaranteed — it needs to be negotiated explicitly. NEXUS's IPAR program is described as audio-first; a digital mode feed may be out of scope.

**Reception is not universal.** HF propagation is atmospheric and variable. Coverage depends on frequency selection, time of day, solar conditions, and receiver location. A beacon that is often undecodable is worse than no beacon — it creates false impressions of unreliability.

**Not a verification mechanism, only a hint.** The beacon broadcasts the hash but cannot prove the chain is valid by itself. A receiver must already have context — prior chain state, the genesis hash, understanding of BTCPC's epoch structure — to interpret the beacon. New users cannot bootstrap from the beacon alone.

**Belize license path is a dead end for the near term.** Pursuing a Belize transmitter license would cost a minimum of six to twelve months, $50,000–$200,000+ in infrastructure, and an opaque ministerial approval process with no guarantee of success. This path is not worth pursuing given that WRMI airtime achieves the same outcome for $120/month.

## Where It Fits

BTCPC core (epoch verification, chain tip publication), hardware clients (SDR receive), P2P protocol (alternative transport/verification layer), future research (owned transmitter long-term)

## Build Path Without Coding

1. **Research validation**: Contact WRMI directly (info@wrmi.net) to confirm (a) they accept digital mode audio content (MFSK tones), (b) confirm $1/min block rate, (c) confirm scheduling granularity (can we buy a repeating 1-minute slot per day or per hour).
2. **Design sketch**: Define the beacon payload format — what goes in each broadcast. Candidate: epoch height (uint64), sealed epoch hash (32 bytes hex), timestamp (unix ms), chain ID. Total: ~120 characters. Fits in under 3 seconds of MFSK32. Include a preamble tone for SDR auto-detection.
3. **Prototype plan**: Encode a sample payload as MFSK32 audio using Fldigi or a Rust MFSK crate. Test decoding on an RTL-SDR in the same room. Then test via online SDR receivers (websdr.org) after a first WRMI test broadcast. Log successful decodes.
4. **Required agents**: Design Agent (beacon payload format, preamble design), P2P Protocol Agent (how beacon integrates with node chain-tip verification logic), Embedded Firmware Agent (RTL-SDR receive code for Pi nodes — passive beacon listener).
5. **Definition of done**: A sealed epoch hash from the live BTCPC chain, broadcast once via WRMI, decoded by at least two SDR receivers in different locations, hash verified against the on-chain state. Cost under $10 for the test.

## Required Agent Handoff

**Design Agent** should receive this first. The beacon payload format, preamble design, and decode protocol need to be specified before anything else. The Design Agent should produce: (1) a byte-level beacon frame spec, (2) the broadcast schedule recommendation (frequency, time windows, repeat cadence), (3) a contact template for WRMI content negotiation.

## Scores

Novelty: 9
BTCPC Fit: 8
Buildability: 7
Hardware Leverage: 8
Software Leverage: 6
Verification Value: 9
Demand Creation: 6
Risk: 3
Agent Handoff Clarity: 8
Time Horizon: 2

## Final Recommendation

E — Prototype Now (WRMI airtime lease path only)
C — Add to research backlog (Belize license path — revisit only if BTCPC ever needs a dedicated transmitter at scale)

The Belize license path is not worth pursuing now. It is a six-to-twelve month regulatory slog with uncertain outcome, significant infrastructure cost, and no advantage over leasing airtime from an already-licensed broadcaster. The airtime lease path — specifically WRMI at $1/min — can be tested for under $10, requires no new code (MFSK32 audio encoding is off-the-shelf), and achieves exactly the stated use case: anyone with an RTL-SDR dongle and no internet can verify the BTCPC chain tip. The Belize research is complete and the answer is: do not pursue.

## Orchestrator Summary

BTCPC can broadcast sealed epoch hashes as a one-way shortwave beacon using WRMI (Radio Miami International) at $1/minute — no Belize license needed. The technical stack is proven: MFSK32 digital mode (used by Shortwave Radiogram in production), RTL-SDR receive, Fldigi decode. A 32-byte SHA-256 epoch hash plus height and timestamp fits in under 3 seconds of airtime. Test cost is under $10. The Belize broadcast license path was fully assessed and rejected: two-regulator system (PUC spectrum + BBA content), ministerial discretion on fees with no codified amounts, no existing shortwave infrastructure in Belize, no foreign-entity precedent, and a minimum six-to-twelve month process before a signal could go out. The action is: contact WRMI to confirm digital mode audio is acceptable content, specify the beacon payload format (Design Agent), and schedule a one-time test broadcast. This is a censorship-resistance and credibility feature that requires no chain modifications and cannot weaken any chain invariant.

---

# Proposal: BTCPC Beacon — Broadcaster Contact Sheet (Phase 2)

Date: 2026-05-24
Source: Primary-source web research: wrmi.net, swradiogram.net, wbcq.com, nexus.org, wwcr.com, en.wikipedia.org
Repo / URL: docs/designs/shortwave-broadcaster-contacts.md
Status: actionable — contact sheet complete, ready for outreach
Tags: shortwave, beacon, wrmi, wbcq, nexus, kim-elliott, mfsk32, olivia, sdr, epoch-hash, broadcaster, contact
Recommendation: O — Send to Orchestrator immediately

## One-Sentence Idea
Broadcaster contacts, verified rates, and content policy notes for leasing shortwave airtime to transmit BTCPC sealed epoch hashes as MFSK digital audio — the full contact sheet is at docs/designs/shortwave-broadcaster-contacts.md.

## Phase 2 Findings (superseding prior Belize research)

**WRMI (primary target) — confirmed actionable:**
- Contact: Jeff White, General Manager
- Email: info@wrmi.net / Phone: +1-305-559-9764
- Rate: $1.00/min published rate card (verified 2026-05-24), blocks of 15/30/60 min
- 4 slots/day = $4/day = ~$120/month for Americas coverage
- Digital-mode audio acceptance: not explicitly confirmed — must ask in first email
- Address: 10400 NW 240th Street, Okeechobee, Florida 34972, USA

**Kim Andrew Elliott (Shortwave Radiogram) — parallel contact:**
- Email: radiogram@verizon.net
- Status: Active through at least program 424 (November 2025), confirmed running
- Ask about: sponsored BTCPC data segment within an existing Shortwave Radiogram broadcast
- Why: cheaper than raw WRMI airtime; existing technically-sophisticated SDR audience; Kim Elliott has done digital-mode experiments before

**WBCQ (backup) — confirmed actionable:**
- Contact: Allan Weiner, Owner/GM
- Email: wbcq@wbcq.com / Phone: 1-207-889-0039 / Twitter: @AllanWBCQ
- Rate: ~$50/hour (~$0.83/min) as of February 2026; promotional $25/hour on 6160 kHz available
- Content culture: highly permissive; unconventional content accepted
- Up to 500 kW; 7.490 / 9.330 / 5.130 / 6.160 MHz

**NEXUS-IBA (Europe/Africa coverage) — quote-based:**
- Email: info@nexus.org / USA toll-free: 888-612-0039
- Rates: quote-based; no published per-minute rate
- IPAR: non-religious, non-commercial, org turnover under $50k USD — digital-mode audio acceptance must be confirmed explicitly before booking
- Consultation: free 15-minute call available before committing

**Ruled out:**
- WWCR (Nashville): exclusively religious/talk, no path for data beacon
- TWR Bonaire: shortwave ended 1993, now medium-wave religious only
- Caribbean Beacon (Anguilla): religious-only, no third-party booking
- Radio Verdad (Guatemala): 700W, religious, not viable for coverage

## Required Agent Handoff
Orchestrator — the contact sheet is complete. The next action is human outreach: email Jeff White at info@wrmi.net and Kim Elliott at radiogram@verizon.net. No agent can send these emails; the Orchestrator should surface this to the human operator for action. The Design Agent should produce the MFSK32 beacon payload spec in parallel so the audio file is ready when WRMI confirms.

## Scores
Novelty: 5
BTCPC Fit: 9
Buildability: 9
Hardware Leverage: 8
Software Leverage: 5
Verification Value: 9
Demand Creation: 5
Risk: 2
Agent Handoff Clarity: 9
Time Horizon: 1

## Final Recommendation
O — Send to Orchestrator immediately

## Orchestrator Summary
Contact sheet complete. Two emails needed now: (1) Jeff White at info@wrmi.net — confirm digital-mode audio is acceptable content, request rate/scheduling confirmation for 1-minute daily slots at $1/min, (2) Kim Elliott at radiogram@verizon.net — ask about a sponsored BTCPC data segment within Shortwave Radiogram. WBCQ (wbcq@wbcq.com, Allan Weiner) is the backup at ~$0.83/min. NEXUS-IBA (info@nexus.org) covers Europe/Africa if needed. Full contact details, rates, content policy notes, and the recommended email script are at docs/designs/shortwave-broadcaster-contacts.md. A single 1-minute test broadcast on WRMI costs $1.00. The Design Agent should produce the MFSK32 beacon payload frame spec so the audio file is ready when WRMI confirms acceptance.
