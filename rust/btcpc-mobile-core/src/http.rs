//! HTTP client for BTCPC node API endpoints.
//!
//! Uses reqwest with rustls-tls (pure Rust TLS, no OpenSSL C dependency).
//! Required for Android cross-compilation.
//!
//! HARDLINE: the node rejects all entries when peer_count == 0.
//! Surface "not connected to network" errors to the user — do NOT retry silently.

use crate::MobileCoreError;

// ── Node info ─────────────────────────────────────────────────────────────────

pub fn fetch_node_info(base_url: String) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/node/info", trim(base_url));
    run_async(async move { get_text(url).await })
}

// ── Account & balance ─────────────────────────────────────────────────────────

pub fn fetch_balance(base_url: String, account: String) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/balance/{}", trim(base_url), account);
    run_async(async move { get_text(url).await })
}

pub fn fetch_account(base_url: String, account: String) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/account/{}", trim(base_url), account);
    run_async(async move {
        let client = build_client()?;
        let resp = client
            .get(&url)
            .send()
            .await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        if resp.status() == reqwest::StatusCode::NOT_FOUND {
            return Err(MobileCoreError::ParseError("account not found".into()));
        }
        if !resp.status().is_success() {
            return Err(MobileCoreError::NetworkError(format!("HTTP {}", resp.status())));
        }
        resp.text().await.map_err(|e| MobileCoreError::NetworkError(e.to_string()))
    })
}

// ── Transfer ──────────────────────────────────────────────────────────────────

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
    let url = format!("{}/api/transfer", trim(base_url));
    let mut body = serde_json::json!({
        "from": from, "to": to, "amount": amount, "token": token,
        "signed_by": signed_by, "nonce": nonce, "signature": sig_hex,
    });
    if let Some(m) = memo {
        body["memo"] = serde_json::Value::String(m);
    }
    run_async(async move { submit_entry(url, body).await })
}

// ── Stake / Unstake ───────────────────────────────────────────────────────────

pub fn post_stake(
    base_url: String,
    account: String,
    amount: u64,
    nonce: u64,
    signed_by: String,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/stake", trim(base_url));
    let body = serde_json::json!({
        "account": account, "amount": amount, "nonce": nonce,
        "signed_by": signed_by, "signature": sig_hex,
    });
    run_async(async move { submit_entry(url, body).await })
}

pub fn post_unstake(
    base_url: String,
    account: String,
    amount: u64,
    nonce: u64,
    signed_by: String,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/unstake", trim(base_url));
    let body = serde_json::json!({
        "account": account, "amount": amount, "nonce": nonce,
        "signed_by": signed_by, "signature": sig_hex,
    });
    run_async(async move { submit_entry(url, body).await })
}

// ── Account creation ──────────────────────────────────────────────────────────

pub fn post_account_create(
    base_url: String,
    account: String,
    keys_json: String,
    funded_by: Option<String>,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/account/create", trim(base_url));
    let keys: serde_json::Value = serde_json::from_str(&keys_json)
        .map_err(|e| MobileCoreError::ParseError(e.to_string()))?;
    let mut body = serde_json::json!({
        "account": account, "keys": keys, "signature": sig_hex,
    });
    if let Some(fb) = funded_by {
        body["funded_by"] = serde_json::Value::String(fb);
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
            return Err(MobileCoreError::NetworkError(format!("HTTP {}: {}", status, text)));
        }
        Ok(text)
    })
}

// ── Node capability ───────────────────────────────────────────────────────────

pub fn post_node_capability(
    base_url: String,
    account: String,
    sig_hex: String,
    epoch: u64,
    vision: bool,
    audio: bool,
    code_exec: bool,
    tier: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/node/capability", trim(base_url));
    let body = serde_json::json!({
        "account": account, "signature": sig_hex, "epoch": epoch,
        "vision": vision, "audio": audio, "code_exec": code_exec, "tier": tier,
    });
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
            return Err(MobileCoreError::NetworkError(format!("HTTP {}: {}", status, text)));
        }
        Ok(text)
    })
}

// ── Inference marketplace ─────────────────────────────────────────────────────

pub fn fetch_inference_jobs(
    base_url: String,
    status_filter: Option<String>,
) -> Result<String, MobileCoreError> {
    let mut url = format!("{}/api/task/jobs", trim(base_url));
    if let Some(s) = status_filter {
        url = format!("{}?status={}", url, s);
    }
    run_async(async move { get_text(url).await })
}

pub fn post_inference_bid(
    base_url: String,
    job_id: String,
    bidder: String,
    fee: u64,
    role: String,
    nonce: u64,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/task/bid", trim(base_url));
    let body = serde_json::json!({
        "job_id": job_id, "bidder": bidder, "fee": fee,
        "role": role, "nonce": nonce, "signature": sig_hex,
    });
    run_async(async move { submit_entry(url, body).await })
}

pub fn post_inference_complete(
    base_url: String,
    job_id: String,
    worker: String,
    result_hash: String,
    latency_ms: u64,
    output_text: String,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/task/complete", trim(base_url));
    let body = serde_json::json!({
        "job_id": job_id, "worker": worker, "result_hash": result_hash,
        "latency_ms": latency_ms, "output_text": output_text, "signature": sig_hex,
    });
    run_async(async move { submit_entry(url, body).await })
}

// ── Coverage & sensor ─────────────────────────────────────────────────────────

pub fn post_coverage_report(
    base_url: String,
    reporter: String,
    lat: f64,
    lon: f64,
    signal_dbm: Option<i32>,
    carrier_mcc_mnc: String,
    technology: String,
    data_hash: String,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/coverage/report", trim(base_url));
    let mut body = serde_json::json!({
        "reporter": reporter,
        "lat": lat,
        "lon": lon,
        "carrier_mcc_mnc": carrier_mcc_mnc,
        "technology": technology,
        "data_hash": data_hash,
        "signature": sig_hex,
    });
    if let Some(dbm) = signal_dbm {
        body["signal_dbm"] = serde_json::Value::Number(dbm.into());
    }
    run_async(async move {
        let client = build_client()?;
        let resp = client.post(&url).json(&body).send().await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        let status = resp.status();
        let text = resp.text().await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        if !status.is_success() {
            return Err(MobileCoreError::NetworkError(format!("HTTP {}: {}", status, text)));
        }
        Ok(text)
    })
}

pub fn post_sensor_register(
    base_url: String,
    sensor_id: String,
    owner: String,
    sensor_type: String,
    location: Option<String>,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/sensor/register", trim(base_url));
    let mut body = serde_json::json!({
        "sensor_id": sensor_id,
        "owner": owner,
        "sensor_type": sensor_type,
        "signature": sig_hex,
    });
    if let Some(loc) = location {
        body["location"] = serde_json::Value::String(loc);
    }
    run_async(async move { submit_entry(url, body).await })
}

pub fn post_sensor_commit(
    base_url: String,
    sensor_id: String,
    owner: String,
    batch_hash: String,
    reading_count: u64,
    sensor_type: String,
    value: Option<f64>,
    sig_hex: String,
) -> Result<String, MobileCoreError> {
    let url = format!("{}/api/sensor/commit", trim(base_url));
    let mut body = serde_json::json!({
        "sensor_id": sensor_id,
        "owner": owner,
        "batch_hash": batch_hash,
        "reading_count": reading_count,
        "sensor_type": sensor_type,
        "signature": sig_hex,
    });
    if let Some(v) = value {
        body["value"] = serde_json::Value::Number(
            serde_json::Number::from_f64(v)
                .unwrap_or(serde_json::Number::from(0))
        );
    }
    run_async(async move {
        let client = build_client()?;
        let resp = client.post(&url).json(&body).send().await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        let status = resp.status();
        let text = resp.text().await
            .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
        if !status.is_success() {
            return Err(MobileCoreError::NetworkError(format!("HTTP {}: {}", status, text)));
        }
        Ok(text)
    })
}

// ── Internals ─────────────────────────────────────────────────────────────────

fn trim(url: String) -> String {
    url.trim_end_matches('/').to_owned()
}

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

async fn get_text(url: String) -> Result<String, MobileCoreError> {
    let client = build_client()?;
    let resp = client
        .get(&url)
        .send()
        .await
        .map_err(|e| MobileCoreError::NetworkError(e.to_string()))?;
    if !resp.status().is_success() {
        return Err(MobileCoreError::NetworkError(format!("HTTP {}", resp.status())));
    }
    resp.text()
        .await
        .map_err(|e| MobileCoreError::NetworkError(e.to_string()))
}

async fn submit_entry(
    url: String,
    body: serde_json::Value,
) -> Result<String, MobileCoreError> {
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

    let parsed: serde_json::Value = serde_json::from_str(&text)
        .map_err(|e| MobileCoreError::ParseError(e.to_string()))?;

    let accepted = parsed
        .get("accepted")
        .and_then(|v| v.as_bool())
        .unwrap_or(false);

    if !accepted {
        let reason = parsed
            .get("error")
            .and_then(|v| v.as_str())
            .unwrap_or("unknown error")
            .to_owned();
        return Err(MobileCoreError::ParseError(format!(
            "not accepted: {}",
            reason
        )));
    }

    parsed
        .get("hash")
        .and_then(|v| v.as_str())
        .map(str::to_owned)
        .ok_or_else(|| MobileCoreError::ParseError("no hash in response".into()))
}
