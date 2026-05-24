//! HTTP client for BTCPC node API endpoints.
//!
//! Uses reqwest with rustls-tls (pure Rust TLS, no OpenSSL C dependency).
//! This is required for Android cross-compilation — OpenSSL vendored builds
//! are fragile with cargo-ndk across multiple ABI targets.
//!
//! Node API endpoints (Axum, port 4242) — verified against api.rs:
//!
//!   GET  /api/node/info     — node status JSON
//!   POST /api/transfer      — submit a signed Transfer entry (flat body)
//!   POST /api/stake         — submit a signed Stake entry (flat body)
//!   POST /api/unstake       — submit a signed Unstake entry (flat body)
//!
//! There is NO generic /api/entries endpoint. Each entry type has its own route.
//! The body shape is flat (fields directly on the JSON object, not wrapped).
//!
//! Response shape for all submission endpoints (from apply_and_broadcast):
//!   {"hash": "<64-char hex>" | null, "accepted": true | false, "error": "<str>" | null}
//!
//! HARDLINE: the node rejects all entries when peer_count == 0 to prevent
//! local forks. The mobile client MUST surface this error to the user and
//! must NOT retry locally or show the entry as confirmed.

use crate::MobileCoreError;

/// Fetch /api/node/info from a BTCPC node.
/// `base_url`: e.g. "http://192.168.1.10:4242" or "https://node.btcpc.net"
/// Returns the raw JSON string. Caller parses it.
pub fn fetch_node_info(base_url: String) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/node/info", base_url.trim_end_matches('/'));
    run_async(async move {
        let client = build_client()?;
        let resp = client
            .get(&url)
            .send()
            .await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        if !resp.status().is_success() {
            return Err(MobileCoreError::NetworkError(format!(
                "node returned HTTP {}",
                resp.status()
            )));
        }
        resp.text()
            .await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))
    })
}

/// POST a Transfer entry to POST /api/transfer.
///
/// Body shape (verified against rust/btcpc-node/src/api.rs TransferBody):
///   { "from", "to", "amount", "token", "memo", "signed_by", "nonce", "signature" }
///
/// `amount`: in dreams (u64). 1 BTCPC = 100,000,000 dreams.
/// `token`: "dreams" for native token.
/// `signed_by`: the account name of the signer (usually same as `from`).
/// `sig_hex`: 128-char hex ed25519 signature over the canonical signing message.
///
/// Returns the entry hash (64-char hex) on success.
///
/// Error cases:
/// - NetworkError: connection failed or non-2xx HTTP
/// - ParseError: response JSON malformed
/// - ParseError("not accepted: <reason>"): node rejected the entry, reason in message.
///   Check for "not connected to network" — user must wait for peers before retrying.
pub fn post_transfer(
    base_url: String,
    from: String,
    to: String,
    amount: u64,
    token: String,
    memo: Option<String>,
    signed_by: String,
    nonce: u64,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/transfer", base_url.trim_end_matches('/'));

    let mut body = serde_json::json!({
        "from":      from,
        "to":        to,
        "amount":    amount,
        "token":     token,
        "signed_by": signed_by,
        "nonce":     nonce,
        "signature": sig_hex,
    });
    if let Some(m) = memo {
        body["memo"] = serde_json::Value::String(m);
    }

    run_async(async move {
        let client = build_client()?;
        let resp = client
            .post(&url)
            .json(&body)
            .send()
            .await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;

        let status = resp.status();
        let text = resp
            .text()
            .await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;

        if !status.is_success() {
            return Err(MobileCoreError::NetworkError(format!(
                "node returned HTTP {}: {}",
                status, text
            )));
        }

        // Response shape: {"hash": "<hex>" | null, "accepted": bool, "error": "<str>" | null}
        let parsed: serde_json::Value = serde_json::from_str(&text)
            .map_err(|e| MobileCoreError::ParseError(e.to_string()))?;

        let accepted = parsed.get("accepted").and_then(|v| v.as_bool()).unwrap_or(false);
        if !accepted {
            let reason = parsed
                .get("error")
                .and_then(|v| v.as_str())
                .unwrap_or("unknown error")
                .to_owned();
            return Err(MobileCoreError::ParseError(format!("not accepted: {}", reason)));
        }

        parsed
            .get("hash")
            .and_then(|v| v.as_str())
            .map(str::to_owned)
            .ok_or_else(|| MobileCoreError::ParseError("no hash in response".into()))
    })
}

// ── Internals ─────────────────────────────────────────────────────────────────

fn build_client() -> Result<reqwest::Client, MobileCoreError> {
    reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(15))
        .build()
        .map_err(|e| MobileCoreError::NetworkError(e.to_string()))
}

fn run_async<F, T>(fut: F) -> T
where
    F: std::future::Future<Output = T>,
{
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("tokio runtime")
        .block_on(fut)
}
