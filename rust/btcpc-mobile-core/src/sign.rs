//! Ed25519 signing of canonical BTCPC entry messages.
//!
//! The signing message format MUST match canonical_signing_message() in
//! rust/btcpc-node/src/tx.rs exactly, or the node will reject submitted entries.
//!
//! All role indexes follow wallet.rs:
//!   0 = owner   (account rotation — do NOT keep on device long-term)
//!   1 = active  (transfers, staking — require biometric/PIN gate in UI)
//!   2 = posting (daily activity, inference bids — safe to keep on device)
//!   3 = memo    (encrypted messages)

use ed25519_dalek::{Signer, SigningKey};
use serde_json::json;
use std::collections::BTreeMap;

use crate::derive::derive_role_key;
use crate::MobileCoreError;

// ── Transfer ──────────────────────────────────────────────────────────────────

/// Sign a Transfer entry (posting key, role 2).
/// Canonical: {"chain_id":..,"type":"TRANSFER","from":..,"to":..,"amount":<u64>,"token":..,"nonce":<u64>}
pub fn sign_transfer(
    mnemonic_words: String,
    chain_id: String,
    from_account: String,
    to_account: String,
    amount: u64,
    token: String,
    nonce: u64,
) -> Result<String, MobileCoreError> {
    let msg = json!({
        "chain_id": chain_id,
        "type": "TRANSFER",
        "from": from_account,
        "to": to_account,
        "amount": amount,
        "token": token,
        "nonce": nonce,
    });
    sign_json(&mnemonic_words, 2, &msg)
}

// ── Stake / Unstake ───────────────────────────────────────────────────────────

/// Sign a Stake entry (active key, role 1).
/// Canonical: {"chain_id":..,"type":"STAKE","account":..,"amount":<u64>,"nonce":<u64>}
pub fn sign_stake(
    mnemonic_words: String,
    chain_id: String,
    account: String,
    amount: u64,
    nonce: u64,
) -> Result<String, MobileCoreError> {
    let msg = json!({"chain_id": chain_id, "type": "STAKE", "account": account, "amount": amount, "nonce": nonce});
    sign_json(&mnemonic_words, 1, &msg)
}

/// Sign an Unstake entry (active key, role 1).
pub fn sign_unstake(
    mnemonic_words: String,
    chain_id: String,
    account: String,
    amount: u64,
    nonce: u64,
) -> Result<String, MobileCoreError> {
    let msg = json!({"chain_id": chain_id, "type": "UNSTAKE", "account": account, "amount": amount, "nonce": nonce});
    sign_json(&mnemonic_words, 1, &msg)
}

// ── Account creation ──────────────────────────────────────────────────────────

/// Sign an AccountCreate entry (owner key, role 0).
/// `keys_json`: JSON object {"owner":"hex","active":"hex","posting":"hex","memo":"hex"}
/// `funded_by`: optional account that pays the 10 BTCPC name registration stake.
/// Canonical: {"chain_id":..,"type":"ACCOUNT_CREATE","account":..,"keys":{sorted},"chain_proofs":[],"funded_by":..}
pub fn sign_account_create(
    mnemonic_words: String,
    chain_id: String,
    account: String,
    keys_json: String,
    funded_by: Option<String>,
) -> Result<String, MobileCoreError> {
    let keys: serde_json::Value = serde_json::from_str(&keys_json)
        .map_err(|e| MobileCoreError::SigningError(e.to_string()))?;
    let sorted_keys: BTreeMap<String, serde_json::Value> = keys
        .as_object()
        .ok_or_else(|| MobileCoreError::SigningError("keys must be a JSON object".into()))?
        .iter()
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect();
    let msg = json!({
        "chain_id": chain_id,
        "type": "ACCOUNT_CREATE",
        "account": account,
        "keys": sorted_keys,
        "chain_proofs": [],
        "funded_by": funded_by,
    });
    sign_json(&mnemonic_words, 0, &msg)
}

// ── Node capability ───────────────────────────────────────────────────────────

/// Sign a node capability announcement (posting key, role 2).
/// The node verifies: check_sig_raw(account, "btcpc-capability:{account}:{epoch}")
pub fn sign_node_capability(
    mnemonic_words: String,
    account: String,
    epoch: u64,
) -> Result<String, MobileCoreError> {
    let (priv_bytes, _) = derive_role_key(&mnemonic_words, 2)?;
    let signing_key = SigningKey::from_bytes(&priv_bytes);
    let msg = format!("btcpc-capability:{}:{}", account, epoch);
    let sig = signing_key.sign(msg.as_bytes());
    Ok(hex::encode(sig.to_bytes()))
}

// ── Inference marketplace ─────────────────────────────────────────────────────

/// Sign an InferenceJobBid (posting key, role 2).
/// Canonical: {"chain_id":..,"type":"INFERENCE_JOB_BID","bidder":..,"job_id":..,"fee":<u64>,"role":..,"nonce":<u64>}
pub fn sign_inference_bid(
    mnemonic_words: String,
    chain_id: String,
    bidder: String,
    job_id: String,
    fee: u64,
    role: String,
    nonce: u64,
) -> Result<String, MobileCoreError> {
    let msg = json!({
        "chain_id": chain_id,
        "type": "INFERENCE_JOB_BID",
        "bidder": bidder,
        "job_id": job_id,
        "fee": fee,
        "role": role,
        "nonce": nonce,
    });
    sign_json(&mnemonic_words, 2, &msg)
}

/// Sign an InferenceJobComplete (posting key, role 2).
/// Canonical: {"chain_id":..,"type":"INFERENCE_JOB_COMPLETE","worker":..,"job_id":..,"result_hash":..}
pub fn sign_inference_complete(
    mnemonic_words: String,
    chain_id: String,
    worker: String,
    job_id: String,
    result_hash: String,
) -> Result<String, MobileCoreError> {
    let msg = json!({
        "chain_id": chain_id,
        "type": "INFERENCE_JOB_COMPLETE",
        "worker": worker,
        "job_id": job_id,
        "result_hash": result_hash,
    });
    sign_json(&mnemonic_words, 2, &msg)
}

// ── Coverage report ───────────────────────────────────────────────────────────

/// Sign a CoverageReport entry (posting key, role 2).
/// Canonical: {"chain_id":..,"type":"COVERAGE_REPORT","reporter":..,"lat":..,"lon":..,"carrier_mcc_mnc":..,"data_hash":..}
/// `data_hash`: SHA-256 hex of the raw cell/WiFi scan JSON batch.
pub fn sign_coverage_report(
    mnemonic_words: String,
    chain_id: String,
    reporter: String,
    lat: f64,
    lon: f64,
    carrier_mcc_mnc: String,
    data_hash: String,
) -> Result<String, MobileCoreError> {
    let msg = json!({
        "chain_id": chain_id,
        "type": "COVERAGE_REPORT",
        "reporter": reporter,
        "lat": lat,
        "lon": lon,
        "carrier_mcc_mnc": carrier_mcc_mnc,
        "data_hash": data_hash,
    });
    sign_json(&mnemonic_words, 2, &msg)
}

/// Sign a SensorDataCommit (posting key, role 2).
/// Canonical: {"chain_id":..,"type":"SENSOR_DATA_COMMIT","sensor_id":..,"owner":..,"batch_hash":..,"reading_count":<u64>}
pub fn sign_sensor_commit(
    mnemonic_words: String,
    chain_id: String,
    sensor_id: String,
    owner: String,
    batch_hash: String,
    reading_count: u64,
) -> Result<String, MobileCoreError> {
    let msg = json!({
        "chain_id": chain_id,
        "type": "SENSOR_DATA_COMMIT",
        "sensor_id": sensor_id,
        "owner": owner,
        "batch_hash": batch_hash,
        "reading_count": reading_count,
    });
    sign_json(&mnemonic_words, 2, &msg)
}

// ── Generic role-based signing ────────────────────────────────────────────────

/// Sign arbitrary canonical JSON with a specific role key.
/// Role: 0=owner, 1=active, 2=posting, 3=memo.
pub fn sign_with_role(
    mnemonic_words: String,
    chain_id: String,
    role_index: u32,
    canonical_json: String,
) -> Result<String, MobileCoreError> {
    let _ = chain_id;
    let (priv_bytes, _) = derive_role_key(&mnemonic_words, role_index)?;
    let signing_key = SigningKey::from_bytes(&priv_bytes);
    let sig = signing_key.sign(canonical_json.as_bytes());
    Ok(hex::encode(sig.to_bytes()))
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn sign_json(
    mnemonic_words: &str,
    role: u32,
    msg: &serde_json::Value,
) -> Result<String, MobileCoreError> {
    let (priv_bytes, _) = derive_role_key(mnemonic_words, role)?;
    let signing_key = SigningKey::from_bytes(&priv_bytes);
    let msg_str = serde_json::to_string(msg)
        .map_err(|e| MobileCoreError::SigningError(e.to_string()))?;
    let sig = signing_key.sign(msg_str.as_bytes());
    Ok(hex::encode(sig.to_bytes()))
}
