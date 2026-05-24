//! Ed25519 signing of canonical BTCPC entry messages.
//!
//! The signing message format MUST match canonical_signing_message() in
//! rust/btcpc-node/src/tx.rs exactly, or the node will reject submitted entries.
//!
//! Transfer canonical message (from tx.rs lines 2251–2260):
//!   {"chain_id":<id>,"type":"TRANSFER","from":<from>,"to":<to>,
//!    "amount":<amount>,"token":<token>,"nonce":<nonce>}
//!
//! serde_json::json! preserves insertion order (indexmap feature), so field
//! order in the json! macro matches the signing message the node expects.
//!
//! The signature is ed25519 over the raw UTF-8 bytes of that JSON string,
//! as verified by: verifying_key.verify_strict(message.as_bytes(), &sig)

use ed25519_dalek::{Signer, SigningKey};
use serde_json::json;

use crate::derive::derive_role_key;
use crate::MobileCoreError;

/// Sign a Transfer entry using the posting key (role 2) derived from the mnemonic.
///
/// NOTE: The node's validate_and_apply checks Transfer against the ACTIVE key
/// (role 1). If the account only has an active key registered and no posting
/// key, use sign_with_role(mnemonic, chain_id, 1, canonical_json) instead.
///
/// The canonical signing message for Transfer is:
///   {"chain_id":<id>,"type":"TRANSFER","from":<from>,"to":<to>,
///    "amount":<u64>,"token":<token>,"nonce":<u64>}
///
/// Returns: 128-char lowercase hex ed25519 signature.
pub fn sign_transfer(
    mnemonic_words: String,
    chain_id: String,
    from_account: String,
    to_account: String,
    amount: u64,
    token: String,
    nonce: u64,
) -> Result<String, MobileCoreError> {
    // Role 2 = posting key (safe to keep on device)
    let (priv_bytes, _) = derive_role_key(&mnemonic_words, 2)?;

    let signing_key = SigningKey::from_bytes(&priv_bytes);

    // Canonical message matches tx.rs `canonical_signing_message` Transfer arm.
    // Field order must be preserved — serde_json preserves json!{} insertion order.
    let msg = json!({
        "chain_id": chain_id,
        "type": "TRANSFER",
        "from": from_account,
        "to": to_account,
        "amount": amount,
        "token": token,
        "nonce": nonce,
    });
    let msg_str = serde_json::to_string(&msg)
        .map_err(|e| MobileCoreError::SigningError(e.to_string()))?;

    let sig = signing_key.sign(msg_str.as_bytes());
    Ok(hex::encode(sig.to_bytes()))
}

/// Sign an arbitrary canonical JSON string using the key for a given role index.
///
/// Role indexes (matching wallet.rs):
///   0 = owner   (key rotation, governance — should NOT be on device)
///   1 = active  (transfers, staking — require biometric/PIN confirmation)
///   2 = posting (daily activity — safe to keep on device)
///   3 = memo    (encrypted messages)
///
/// `canonical_json` must be the exact bytes the node will verify against —
/// callers are responsible for constructing it correctly per tx.rs.
///
/// Returns: 128-char lowercase hex ed25519 signature.
pub fn sign_with_role(
    mnemonic_words: String,
    chain_id: String,
    role_index: u32,
    canonical_json: String,
) -> Result<String, MobileCoreError> {
    // chain_id is passed for documentation; callers must embed it in canonical_json.
    let _ = chain_id;

    let (priv_bytes, _) = derive_role_key(&mnemonic_words, role_index)?;
    let signing_key = SigningKey::from_bytes(&priv_bytes);
    let sig = signing_key.sign(canonical_json.as_bytes());
    Ok(hex::encode(sig.to_bytes()))
}
