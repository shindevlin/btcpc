//! Bitcoin Mesh Relay (BMR) — gateway service for relaying raw Bitcoin transactions
//! received over LoRa (or any other inbound transport) to the Bitcoin network via Tor.
//!
//! This module is a **gateway service only**. It has zero interaction with BTCPC chain
//! state. Bitcoin payloads MUST NEVER call `chain.apply_entry()` and MUST NEVER be
//! re-broadcast on the `btcpc/entries` gossipsub topic. The zero-peers chain integrity
//! hardline is unaffected — it governs BTCPC entries only.
//!
//! # Design decisions (resolved from design doc open questions)
//!
//! 1. **reqwest socks feature**: was absent from Cargo.toml; added (`"socks"` feature).
//!    Required for `reqwest::Proxy::all("socks5h://...")` to compile and function.
//!
//! 2. **CRC algorithm**: CRC16-XMODEM (`crc::CRC_16_XMODEM`). 2-byte overhead keeps the
//!    chunk header at 16 bytes total. Covers the same corruption patterns as CRC32 for
//!    the short payloads LoRa carries; CRC32 would cost 2 extra bytes per chunk for no
//!    meaningful gain at this packet size.
//!
//! 3. **Session ID**: 8 random bytes (`rand::random::<[u8; 8]>()`), generated once per
//!    transaction by the originating device and held constant across all chunks for that
//!    tx. The gateway buffers reassembly state keyed on session_id. Sessions expire after
//!    60 seconds. No timestamp component — session IDs are ephemeral and not persisted,
//!    so log correlation happens via the truncated txid after reassembly, not the session.
//!
//! 4. **Env var naming**: all BMR vars prefixed `BTCPC_BMR_` to avoid collision with
//!    existing `BTCPC_*` vars in config.rs. The enable flag is `BTCPC_BMR=true`.
//!    Confirmed: config.rs has no Bitcoin or BMR vars.
//!
//! 5. **Bitcoin network independence**: BMR always forwards to the Bitcoin mainnet Esplora
//!    endpoint by default, regardless of BTCPC `chain_id`. A BTCPC testnet node can relay
//!    mainnet Bitcoin txs. Override with `BTCPC_BMR_ESPLORA_URL` if needed.
//!
//! 6. **LoRa frame size / chunk payload**: designed for SF7BW125 (242-byte max LoRa
//!    payload). `CHUNK_PAYLOAD_SIZE = 226` bytes (242 − 16 header bytes). Change
//!    `LORA_MAX_PAYLOAD` to 115 for SF9BW125 long-range configurations.
//!
//! # Wire format (v1, uplink chunks over LoRa)
//!
//! ```text
//! Magic:       BTC1        (4 bytes, literal ASCII) — not included in CRC
//! Session ID:  [8 bytes]   random, same across all chunks for this tx
//! Chunk index: [1 byte]    0-indexed
//! Total chunks:[1 byte]    total chunk count (max 255 → ~57 KB; sufficient for txs)
//! Payload:     [N bytes]   N = min(remaining_bytes, CHUNK_PAYLOAD_SIZE)
//! CRC16:       [2 bytes]   CRC16-XMODEM of (session_id || chunk_idx || total || payload)
//!
//! Total overhead: 16 bytes. Usable payload: 226 bytes at SF7BW125.
//! ```
//!
//! # Privacy
//!
//! - No LoRa node ID or gateway MAC is logged alongside a txid.
//! - Egress is Tor SOCKS5 only by default. Clearnet fallback requires explicit
//!   `BTCPC_BMR_CLEARNET=true` and logs a warning on every use.
//! - Log line on successful forward: `"BMR: forwarded tx <first 8 chars of txid>"`
//!
//! # Env vars
//!
//! ```text
//! BTCPC_BMR=true                — enable this module (off by default)
//! BTCPC_BMR_TOR_SOCKS           — Tor SOCKS5 address (default: 127.0.0.1:9050)
//! BTCPC_BMR_CLEARNET=true       — opt-in clearnet egress when Tor unavailable (default: false)
//! BTCPC_BMR_ESPLORA_URL         — override Esplora endpoint (default: https://blockstream.info/api/tx)
//! ```

use std::collections::HashMap;
use std::io::Cursor;
use std::time::{Duration, Instant};

use bitcoin::consensus::Decodable;
use crc::{Crc, CRC_16_XMODEM};
use tokio::sync::mpsc;
use tracing::{debug, info, warn};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Maximum LoRa payload at SF7BW125. Change to 115 for SF9BW125 long-range.
/// Kept as a named constant so LoRa configuration changes have a single edit point.
#[allow(dead_code)]
const LORA_MAX_PAYLOAD: usize = 242;

/// BMR wire-format header overhead in bytes:
///   4 (magic) + 8 (session_id) + 1 (chunk_idx) + 1 (total_chunks) + 2 (CRC16) = 16
/// queue_payload receives the full frame including the BTC1 magic prefix.
const FRAME_HEADER_BYTES: usize = 4 + 8 + 1 + 1 + 2; // magic + sid + idx + total + crc

/// Usable payload bytes per chunk at SF7BW125 (242 − 16 = 226).
/// Kept as a named constant for firmware documentation and interop testing.
#[allow(dead_code)]
const CHUNK_PAYLOAD_SIZE: usize = LORA_MAX_PAYLOAD - FRAME_HEADER_BYTES; // 226

/// Maximum concurrent reassembly sessions held in memory.
const MAX_SESSIONS: usize = 64;

/// Time before an incomplete reassembly session is evicted.
const SESSION_TTL: Duration = Duration::from_secs(60);

/// Egress channel depth — generous given LoRa throughput.
const CHANNEL_DEPTH: usize = 64;

/// CRC16-XMODEM instance (stateless, zero cost to construct).
const CRC16: Crc<u16> = Crc::<u16>::new(&CRC_16_XMODEM);

/// Default Esplora mainnet endpoint.
const DEFAULT_ESPLORA_URL: &str = "https://blockstream.info/api/tx";

/// Default Tor SOCKS5 proxy address.
const DEFAULT_TOR_SOCKS: &str = "127.0.0.1:9050";

/// Retry delays for Esplora POST.
const RETRY_DELAYS_SECS: &[u64] = &[1, 3, 9];

// ---------------------------------------------------------------------------
// Public handle
// ---------------------------------------------------------------------------

/// Handle for the Bitcoin Mesh Relay. Cheap to clone.
/// Call `queue_payload` with a raw BMR wire-format chunk (including the `BTC1` magic).
/// When the module is disabled, the noop handle discards all payloads silently.
#[derive(Clone)]
pub struct BitcoinRelayHandle {
    tx: Option<mpsc::Sender<Vec<u8>>>,
}

impl BitcoinRelayHandle {
    /// Queue a raw BMR chunk (full wire frame, including `BTC1` magic prefix).
    /// Non-blocking — drops silently when the channel is full or module is disabled.
    pub fn queue_payload(&self, bytes: Vec<u8>) {
        if let Some(ref tx) = self.tx {
            let _ = tx.try_send(bytes);
        }
    }

    /// Returns a handle that discards all payloads. Used when `BTCPC_BMR` is not set.
    pub fn noop() -> Self {
        BitcoinRelayHandle { tx: None }
    }

    /// Create a handle connected to the given sender. For testing only.
    #[cfg(test)]
    pub fn from_sender(tx: mpsc::Sender<Vec<u8>>) -> Self {
        BitcoinRelayHandle { tx: Some(tx) }
    }
}

// ---------------------------------------------------------------------------
// Module entry point
// ---------------------------------------------------------------------------

/// Start the Bitcoin Mesh Relay if `BTCPC_BMR=true`.
/// Returns an active handle on success, a noop handle otherwise.
pub async fn start_bitcoin_relay() -> BitcoinRelayHandle {
    if std::env::var("BTCPC_BMR").as_deref() != Ok("true") {
        return BitcoinRelayHandle::noop();
    }

    let tor_socks = std::env::var("BTCPC_BMR_TOR_SOCKS")
        .unwrap_or_else(|_| DEFAULT_TOR_SOCKS.to_string());
    let allow_clearnet = std::env::var("BTCPC_BMR_CLEARNET").as_deref() == Ok("true");
    let esplora_url = std::env::var("BTCPC_BMR_ESPLORA_URL")
        .unwrap_or_else(|_| DEFAULT_ESPLORA_URL.to_string());

    // Build the Tor-proxied reqwest client. The socks5h:// scheme delegates hostname
    // resolution to Tor — the gateway's IP never touches DNS for the Esplora hostname.
    let client = match build_tor_client(&tor_socks) {
        Ok(c) => {
            info!("BMR: Tor egress via socks5h://{}", tor_socks);
            Some(c)
        }
        Err(e) => {
            warn!("BMR: failed to build Tor client ({}); clearnet_allowed={}", e, allow_clearnet);
            if allow_clearnet {
                match reqwest::Client::builder()
                    .timeout(Duration::from_secs(30))
                    .build()
                {
                    Ok(c) => {
                        warn!("BMR: using clearnet egress — privacy reduced");
                        Some(c)
                    }
                    Err(e2) => {
                        warn!("BMR: clearnet client build also failed ({}); relay disabled", e2);
                        None
                    }
                }
            } else {
                warn!("BMR: Tor unavailable and BTCPC_BMR_CLEARNET not set; relay disabled");
                None
            }
        }
    };

    let Some(client) = client else {
        return BitcoinRelayHandle::noop();
    };

    let (tx, rx) = mpsc::channel::<Vec<u8>>(CHANNEL_DEPTH);

    tokio::spawn(async move {
        run_relay(rx, client, esplora_url, allow_clearnet).await;
    });

    info!("BMR: Bitcoin Mesh Relay v1 active");
    BitcoinRelayHandle { tx: Some(tx) }
}

// ---------------------------------------------------------------------------
// Build a reqwest::Client with a Tor SOCKS5h proxy
// ---------------------------------------------------------------------------

fn build_tor_client(tor_socks: &str) -> Result<reqwest::Client, reqwest::Error> {
    let proxy_url = format!("socks5h://{}", tor_socks);
    reqwest::Client::builder()
        .proxy(reqwest::Proxy::all(&proxy_url)?)
        .timeout(Duration::from_secs(30))
        .build()
}

// ---------------------------------------------------------------------------
// Reassembly state
// ---------------------------------------------------------------------------

struct Session {
    total_chunks: u8,
    chunks: Vec<Option<Vec<u8>>>, // indexed by chunk_idx
    created: Instant,
}

impl Session {
    fn new(total_chunks: u8) -> Self {
        Session {
            total_chunks,
            chunks: vec![None; total_chunks as usize],
            created: Instant::now(),
        }
    }

    fn is_expired(&self) -> bool {
        self.created.elapsed() > SESSION_TTL
    }

    /// Store a chunk. Returns the reassembled transaction bytes if all chunks arrived.
    fn insert_chunk(&mut self, idx: u8, payload: Vec<u8>) -> Option<Vec<u8>> {
        let i = idx as usize;
        if i >= self.chunks.len() {
            return None;
        }
        self.chunks[i] = Some(payload);

        let all_present = self.chunks.iter().all(|c| c.is_some());
        if all_present {
            let assembled: Vec<u8> = self.chunks
                .iter()
                .flat_map(|c| c.as_ref().unwrap().iter().copied())
                .collect();
            Some(assembled)
        } else {
            None
        }
    }
}

// ---------------------------------------------------------------------------
// Main relay run loop
// ---------------------------------------------------------------------------

async fn run_relay(
    mut rx: mpsc::Receiver<Vec<u8>>,
    client: reqwest::Client,
    esplora_url: String,
    allow_clearnet: bool,
) {
    let mut sessions: HashMap<[u8; 8], Session> = HashMap::new();

    while let Some(frame) = rx.recv().await {
        // Evict expired sessions before processing each frame.
        evict_expired(&mut sessions);

        match parse_chunk(&frame) {
            Err(e) => {
                debug!("BMR: chunk parse error: {}", e);
            }
            Ok((session_id, chunk_idx, total_chunks, payload)) => {
                // Enforce session cap — evict oldest if full.
                if sessions.len() >= MAX_SESSIONS && !sessions.contains_key(&session_id) {
                    evict_oldest(&mut sessions);
                }

                let session = sessions
                    .entry(session_id)
                    .or_insert_with(|| Session::new(total_chunks));

                // Sanity: total_chunks must be consistent across chunks of same session.
                if session.total_chunks != total_chunks {
                    debug!("BMR: session {:?} total_chunks mismatch — dropping chunk", session_id);
                    continue;
                }

                if let Some(raw_tx) = session.insert_chunk(chunk_idx, payload) {
                    sessions.remove(&session_id);
                    // Validate and relay the fully reassembled transaction.
                    relay_tx(raw_tx, &client, &esplora_url, allow_clearnet).await;
                }
            }
        }
    }
}

/// Evict all sessions that have exceeded SESSION_TTL.
fn evict_expired(sessions: &mut HashMap<[u8; 8], Session>) {
    sessions.retain(|_, s| !s.is_expired());
}

/// Evict the oldest session by creation time when the cap is reached.
fn evict_oldest(sessions: &mut HashMap<[u8; 8], Session>) {
    if let Some(oldest_key) = sessions
        .iter()
        .min_by_key(|(_, s)| s.created)
        .map(|(k, _)| *k)
    {
        sessions.remove(&oldest_key);
    }
}

// ---------------------------------------------------------------------------
// Chunk parsing
// ---------------------------------------------------------------------------

/// Parse a raw BMR wire frame (including the `BTC1` magic prefix).
///
/// Wire layout:
///   [0..4]   magic "BTC1"
///   [4..12]  session_id (8 bytes)
///   [12]     chunk_idx
///   [13]     total_chunks
///   [14..N-2] payload
///   [N-2..N] CRC16-XMODEM of (session_id || chunk_idx || total_chunks || payload)
///
/// Returns `(session_id, chunk_idx, total_chunks, payload_bytes)` on success.
fn parse_chunk(frame: &[u8]) -> Result<([u8; 8], u8, u8, Vec<u8>), &'static str> {
    // Minimum: magic(4) + sid(8) + idx(1) + total(1) + crc(2) = 16 bytes.
    if frame.len() < FRAME_HEADER_BYTES {
        return Err("frame too short");
    }
    if &frame[..4] != b"BTC1" {
        return Err("missing BTC1 magic");
    }

    let mut session_id = [0u8; 8];
    session_id.copy_from_slice(&frame[4..12]);
    let chunk_idx   = frame[12];
    let total_chunks = frame[13];

    if total_chunks == 0 {
        return Err("total_chunks is zero");
    }
    if chunk_idx >= total_chunks {
        return Err("chunk_idx >= total_chunks");
    }

    // Payload sits between the fixed header and the 2-byte CRC at the tail.
    let payload_end = frame.len() - 2;
    let payload = frame[14..payload_end].to_vec();

    // Verify CRC16-XMODEM over: session_id || chunk_idx || total_chunks || payload
    let received_crc = u16::from_le_bytes([frame[payload_end], frame[payload_end + 1]]);
    let mut crc_input = Vec::with_capacity(8 + 1 + 1 + payload.len());
    crc_input.extend_from_slice(&session_id);
    crc_input.push(chunk_idx);
    crc_input.push(total_chunks);
    crc_input.extend_from_slice(&payload);
    let computed_crc = CRC16.checksum(&crc_input);

    if computed_crc != received_crc {
        return Err("CRC16 mismatch");
    }

    Ok((session_id, chunk_idx, total_chunks, payload))
}

// ---------------------------------------------------------------------------
// Transaction validation and egress
// ---------------------------------------------------------------------------

/// Structurally validate and relay one fully-reassembled raw Bitcoin transaction.
async fn relay_tx(
    raw: Vec<u8>,
    client: &reqwest::Client,
    esplora_url: &str,
    allow_clearnet: bool,
) {
    // Structural decode — rejects noise, truncated frames, and malformed scripts.
    // Does NOT check UTXOs, fee rate, or mempool policy (gateway has no UTXO state).
    let tx = match bitcoin::Transaction::consensus_decode(&mut Cursor::new(&raw)) {
        Ok(tx) => tx,
        Err(e) => {
            warn!("BMR: structural decode failed ({}) — {} bytes dropped", e, raw.len());
            return;
        }
    };

    let txid = tx.compute_txid();
    // Privacy: log only the first 8 chars of the txid — enough for correlation in
    // operator logs, not enough to link to a specific UTXO set or wallet.
    let txid_prefix = &txid.to_string()[..8];

    let hex_tx = hex::encode(&raw);

    for (attempt, &delay_secs) in RETRY_DELAYS_SECS.iter().enumerate() {
        if attempt > 0 {
            tokio::time::sleep(Duration::from_secs(delay_secs)).await;
        }

        match client
            .post(esplora_url)
            .header("Content-Type", "text/plain")
            .body(hex_tx.clone())
            .send()
            .await
        {
            Ok(resp) if resp.status().is_success() => {
                info!("BMR: forwarded tx {}", txid_prefix);
                return;
            }
            Ok(resp) => {
                let status = resp.status();
                // 400 from Esplora means the tx was already broadcast or is invalid
                // on the Bitcoin network. Either way, no point retrying.
                if status.as_u16() == 400 {
                    debug!("BMR: Esplora 400 for tx {} — already broadcast or invalid", txid_prefix);
                    return;
                }
                warn!(
                    "BMR: Esplora attempt {}/{} returned {} for tx {}",
                    attempt + 1, RETRY_DELAYS_SECS.len() + 1, status, txid_prefix
                );
            }
            Err(e) => {
                // If Tor is not running, the connection will be refused here.
                // Do not fall back to clearnet unless explicitly configured.
                if e.is_connect() && !allow_clearnet {
                    warn!(
                        "BMR: Tor egress connection failed for tx {} — dropping (clearnet not allowed)",
                        txid_prefix
                    );
                    return;
                }
                warn!(
                    "BMR: egress attempt {}/{} failed for tx {}: {}",
                    attempt + 1, RETRY_DELAYS_SECS.len() + 1, txid_prefix, e
                );
            }
        }
    }

    warn!("BMR: all egress attempts exhausted for tx {} — dropped", txid_prefix);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn make_chunk(
        session_id: [u8; 8],
        chunk_idx: u8,
        total_chunks: u8,
        payload: &[u8],
    ) -> Vec<u8> {
        let mut crc_input = Vec::new();
        crc_input.extend_from_slice(&session_id);
        crc_input.push(chunk_idx);
        crc_input.push(total_chunks);
        crc_input.extend_from_slice(payload);
        let crc = CRC16.checksum(&crc_input);

        let mut frame = Vec::new();
        frame.extend_from_slice(b"BTC1");
        frame.extend_from_slice(&session_id);
        frame.push(chunk_idx);
        frame.push(total_chunks);
        frame.extend_from_slice(payload);
        frame.extend_from_slice(&crc.to_le_bytes());
        frame
    }

    #[test]
    fn parse_chunk_round_trip() {
        let sid = [1u8; 8];
        let payload = b"hello bitcoin";
        let frame = make_chunk(sid, 0, 1, payload);
        let (parsed_sid, idx, total, parsed_payload) = parse_chunk(&frame).unwrap();
        assert_eq!(parsed_sid, sid);
        assert_eq!(idx, 0);
        assert_eq!(total, 1);
        assert_eq!(parsed_payload, payload);
    }

    #[test]
    fn parse_chunk_rejects_missing_magic() {
        let mut frame = make_chunk([0u8; 8], 0, 1, b"data");
        frame[0] = b'X'; // corrupt magic
        assert!(parse_chunk(&frame).is_err());
    }

    #[test]
    fn parse_chunk_rejects_crc_mismatch() {
        let mut frame = make_chunk([0u8; 8], 0, 1, b"data");
        let last = frame.len() - 1;
        frame[last] ^= 0xFF; // flip CRC bytes
        assert!(parse_chunk(&frame).is_err());
    }

    #[test]
    fn parse_chunk_rejects_too_short() {
        let frame = b"BTC1".to_vec(); // only 4 bytes
        assert!(parse_chunk(&frame).is_err());
    }

    #[test]
    fn parse_chunk_rejects_idx_out_of_range() {
        let frame = make_chunk([0u8; 8], 5, 3, b"data"); // idx >= total
        assert!(parse_chunk(&frame).is_err());
    }

    #[test]
    fn session_single_chunk_reassembles() {
        let payload = b"singlechunk".to_vec();
        let mut session = Session::new(1);
        let result = session.insert_chunk(0, payload.clone());
        assert_eq!(result, Some(payload));
    }

    #[test]
    fn session_two_chunks_reassemble_in_order() {
        let mut session = Session::new(2);
        assert!(session.insert_chunk(0, b"hello".to_vec()).is_none());
        let result = session.insert_chunk(1, b"world".to_vec());
        assert_eq!(result, Some(b"helloworld".to_vec()));
    }

    #[test]
    fn session_two_chunks_reassemble_out_of_order() {
        let mut session = Session::new(2);
        assert!(session.insert_chunk(1, b"world".to_vec()).is_none());
        let result = session.insert_chunk(0, b"hello".to_vec());
        assert_eq!(result, Some(b"helloworld".to_vec()));
    }

    #[test]
    fn noop_handle_does_not_panic() {
        let handle = BitcoinRelayHandle::noop();
        // Should discard silently — no panic.
        handle.queue_payload(b"BTC1anything".to_vec());
    }

    #[test]
    fn chunk_payload_size_matches_spec() {
        // Verify the constant arithmetic is correct.
        assert_eq!(CHUNK_PAYLOAD_SIZE, 226, "CHUNK_PAYLOAD_SIZE must be 226 at SF7BW125");
        assert_eq!(
            LORA_MAX_PAYLOAD - FRAME_HEADER_BYTES,
            226,
            "242 - 16 = 226"
        );
    }
}
