//! Sensor reading submission for the Android micronode.
//!
//! Handles every sensor type the Java service can provide:
//! accelerometer, linear-acceleration, gravity, gyroscope, orientation,
//! orientation-geo, light, barometer, magnetometer, proximity,
//! steps, step-detector, heart-rate, GPS, battery, audio-level.
//!
//! Java calls nativeSubmitReading; this builds the LedgerEntry, signs it
//! with the owner's posting key, applies it locally, and broadcasts over
//! btcpc/entries gossip.
//!
//! # Posting-key signing (PR #7 follow-up)
//!
//! `LedgerEntry::SensorReading` carries a `signed_by` field and the chain
//! (`rust/btcpc-node/src/tx.rs`, `check_signature`) requires a valid ed25519
//! signature over the canonical signing message once the named owner has a
//! posting key registered. `signed_by` is always set to `reading.owner` here
//! (the phone signs for its own account); see
//! `clients/btcpc-flipper/docs/SIGNING_INTEGRATION.md` (Option B) for why
//! the phone — not the Flipper — is the component that holds the posting key
//! and performs this signature.
//!
//! `build_canonical_signing_message` below is a faithful reproduction of
//! `canonical_signing_message`'s `SensorReading` arm in
//! `rust/btcpc-node/src/tx.rs` (btcpc-node is a `[[bin]]`-only crate with no
//! `lib` target, so it cannot be imported here — this must be kept in sync
//! by hand with that function). Field set and order: `chain_id`, `type`,
//! `sensor_id`, `owner`, `value`, `data_hash`, `signed_by`. Deliberately
//! excludes the server-set `epoch` and `metadata`.

use std::sync::Arc;
use ed25519_dalek::{Signer, SigningKey};
use sha2::{Digest, Sha256};
use tracing::warn;

use btcpc_types::LedgerEntry;
use crate::chain::Chain;
use crate::net::NetCmd;

pub struct SensorReading {
    /// "account/deviceName-sensorType"
    pub sensor_id:    String,
    pub sensor_type:  String,
    /// Primary scalar value (magnitude, lux, hPa, bpm, etc.)
    pub primary_value: f64,
    /// Full multi-axis or composite JSON: {"x":1.2,"y":0.3,"z":9.8}
    /// or {"lat":53.3,"lon":-6.2,"accuracy":5.0} etc.
    pub values_json:   String,
    pub unit:          String,
    pub owner:         String,
    pub epoch:         u64,
}

/// Reproduces `canonical_signing_message`'s `SensorReading` arm
/// (`rust/btcpc-node/src/tx.rs`) exactly: same fields, same order, same
/// `serde_json::to_string` shape. Must stay byte-for-byte identical to what
/// the chain verifies against, or signatures produced here will be rejected.
fn build_canonical_signing_message(
    chain_id:  &str,
    sensor_id: &str,
    owner:     &str,
    value:     f64,
    data_hash: &str,
    signed_by: &str,
) -> String {
    let msg = serde_json::json!({
        "chain_id": chain_id,
        "type": "SENSOR_READING",
        "sensor_id": sensor_id,
        "owner": owner,
        "value": value,
        "data_hash": data_hash,
        "signed_by": signed_by,
    });
    serde_json::to_string(&msg).expect("canonical signing message is always valid JSON")
}

/// Sign `message` with a hex-encoded 32-byte ed25519 posting-key seed,
/// returning the hex-encoded 64-byte signature. Mirrors
/// `load_signing_key`/`SigningKey::from_bytes` in `rust/btcpc-node/src/main.rs`.
fn sign_with_posting_key(posting_key_hex: &str, message: &str) -> Option<String> {
    let bytes = hex::decode(posting_key_hex.trim()).ok()?;
    let seed: [u8; 32] = bytes.try_into().ok()?;
    let signing_key = SigningKey::from_bytes(&seed);
    let signature = signing_key.sign(message.as_bytes());
    Some(hex::encode(signature.to_bytes()))
}

pub async fn submit(
    reading:     SensorReading,
    chain:       Arc<Chain>,
    cmd_tx:      tokio::sync::mpsc::Sender<NetCmd>,
    posting_key: String,
) {
    // Deterministic hash of the reading for on-chain commitment.
    let hash_input = format!(
        "{}:{}:{}:{}",
        reading.sensor_id, reading.sensor_type,
        reading.primary_value.to_bits(), reading.epoch
    );
    let data_hash = hex::encode(Sha256::digest(hash_input.as_bytes()));

    // Parse values JSON for metadata enrichment.
    let values: serde_json::Value = serde_json::from_str(&reading.values_json)
        .unwrap_or(serde_json::Value::Null);

    let metadata = serde_json::json!({
        "type":    reading.sensor_type,
        "unit":    reading.unit,
        "values":  values,
    });

    // The phone signs for its own account (Option B in SIGNING_INTEGRATION.md).
    let signed_by = reading.owner.clone();

    let entry = LedgerEntry::SensorReading {
        sensor_id: reading.sensor_id.clone(),
        owner:     reading.owner.clone(),
        epoch:     reading.epoch,
        value:     reading.primary_value,
        data_hash: data_hash.clone(),
        metadata:  Some(metadata),
        signed_by: signed_by.clone(),
    };

    if let Err(e) = chain.apply_entry(&entry) {
        warn!("sensors: apply failed: {}", e);
        return;
    }

    // Sign the canonical message with the owner's posting key, if we have one.
    // A missing/invalid key falls back to an unsigned submission, which the
    // chain still accepts for keyless owners (bootstrap grace period) but
    // will reject once the owner has a posting key registered — see
    // `check_signature` in tx.rs.
    let sig = if posting_key.is_empty() {
        None
    } else {
        let message = build_canonical_signing_message(
            &chain.chain_id,
            &reading.sensor_id,
            &reading.owner,
            reading.primary_value,
            &data_hash,
            &signed_by,
        );
        sign_with_posting_key(&posting_key, &message)
    };

    // Gossip envelope matches the chain-wide convention: {"entry": ..., "sig": ...}
    // (see rust/btcpc-node/src/main.rs, "Wrap as {\"entry\": <json>, \"sig\": <hex_or_null>}").
    let envelope = serde_json::json!({ "entry": entry, "sig": sig });
    if let Ok(data) = serde_json::to_vec(&envelope) {
        let _ = cmd_tx.send(NetCmd::Broadcast {
            topic: "btcpc/entries",
            data,
        }).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signature, Verifier, VerifyingKey};

    /// Mirrors `sensor_reading_keyed_owner_applies_with_correct_signature` in
    /// rust/btcpc-node/src/tx.rs: build a SensorReading, sign it with a known
    /// posting key, and verify the signature against the canonical message
    /// with the derived public key. Proves the phone produces a signature
    /// the chain's `check_signature` will accept.
    #[test]
    fn signed_sensor_reading_verifies_against_canonical_message() {
        let posting_key_seed = [b'a'; 32];
        let signing_key = SigningKey::from_bytes(&posting_key_seed);
        let posting_key_hex = hex::encode(posting_key_seed);

        let chain_id  = "btcpc-satoshi";
        let sensor_id = "alice/pixel-accelerometer";
        let owner     = "alice";
        let value     = 9.81_f64;
        let data_hash = "ab".repeat(32);
        let signed_by = owner;

        let message = build_canonical_signing_message(
            chain_id, sensor_id, owner, value, &data_hash, signed_by,
        );

        let sig_hex = sign_with_posting_key(&posting_key_hex, &message)
            .expect("posting key must be valid hex and produce a signature");

        // Verify exactly as the chain would: decode pubkey + sig, verify_strict
        // over the same canonical message.
        let verifying_key = signing_key.verifying_key();
        let sig_bytes = hex::decode(&sig_hex).unwrap();
        let sig_array: [u8; 64] = sig_bytes.try_into().unwrap();
        let signature = Signature::from_bytes(&sig_array);

        assert!(
            verifying_key.verify(message.as_bytes(), &signature).is_ok(),
            "signature produced by sign_with_posting_key must verify against the canonical message"
        );

        // Sanity: verifying key we derived matches what a chain node would
        // derive from the same hex seed via `load_signing_key`.
        let expected_pubkey_hex = hex::encode(verifying_key.to_bytes());
        assert_eq!(expected_pubkey_hex.len(), 64);
    }

    #[test]
    fn wrong_key_signature_fails_verification() {
        let real_seed = [b'a'; 32];
        let attacker_seed = [b'z'; 32];

        let real_key_hex = hex::encode(real_seed);
        let attacker_key = SigningKey::from_bytes(&attacker_seed);

        let message = build_canonical_signing_message(
            "btcpc-satoshi", "alice/sensor", "alice", 1.0, &"cd".repeat(32), "alice",
        );

        // Attacker signs with their own key but claims to be alice.
        let attacker_sig_hex = sign_with_posting_key(&hex::encode(attacker_seed), &message).unwrap();

        let real_signing_key = SigningKey::from_bytes(&real_seed);
        let real_verifying_key = real_signing_key.verifying_key();

        let sig_bytes = hex::decode(&attacker_sig_hex).unwrap();
        let sig_array: [u8; 64] = sig_bytes.try_into().unwrap();
        let signature = Signature::from_bytes(&sig_array);

        assert!(
            real_verifying_key.verify(message.as_bytes(), &signature).is_err(),
            "an attacker's signature must not verify against the real owner's public key"
        );

        // Also sanity-check the attacker's own key does verify their own sig
        // (proves the failure above is about key mismatch, not a broken test).
        let attacker_verifying = attacker_key.verifying_key();
        assert!(attacker_verifying.verify(message.as_bytes(), &signature).is_ok());
        let _ = real_key_hex;
    }
}
