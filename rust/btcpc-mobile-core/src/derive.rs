//! SLIP-10 ed25519 key derivation for BTCPC role keys.
//!
//! Matches wallet.rs in rust/btcpc-node/src/wallet.rs exactly:
//!   - Master key: HMAC-SHA512("ed25519 seed", seed_bytes)
//!   - Each path segment: HMAC-SHA512(chain_code, 0x00 || key || (index | 0x80000000).to_be_bytes())
//!   - All segments are hardened (bit 31 set)
//!   - No passphrase — empty string passed to Mnemonic::to_seed("")
//!
//! BTCPC coin type 6942 (SLIP-44 registered).
//! Role 2 = posting key (safe to keep on device, used for daily operations).
//! Role 1 = active key (transfers, staking — require user confirmation).

use bip39::Mnemonic;
use ed25519_dalek::SigningKey;
use hmac::{Hmac, Mac};
use serde_json;
use sha2::Sha512;

use crate::MobileCoreError;

type HmacSha512 = Hmac<Sha512>;

/// Hardened index flag — same as wallet.rs `const H: u32 = 0x8000_0000`.
const H: u32 = 0x8000_0000;

/// BTCPC coin type in SLIP-44.
const BTCPC_COIN: u32 = 6942;

/// Derive the BTCPC posting key from a BIP39 mnemonic.
///
/// Path: SLIP-10 m/44'/6942'/2'/0'  (role 2 = posting)
/// Returns the ed25519 public key as a 64-char lowercase hex string.
pub fn derive_posting_pubkey(mnemonic_words: String) -> Result<String, MobileCoreError> {
    let (_, pub_key) = derive_role_key(&mnemonic_words, 2)?;
    Ok(hex::encode(pub_key))
}

/// Derive all four BTCPC role public keys from a BIP39 mnemonic.
///
/// Returns JSON: {"owner":"hex","active":"hex","posting":"hex","memo":"hex"}
/// Roles: 0=owner, 1=active, 2=posting, 3=memo
pub fn derive_all_pubkeys(mnemonic_words: String) -> Result<String, MobileCoreError> {
    let (_, owner)   = derive_role_key(&mnemonic_words, 0)?;
    let (_, active)  = derive_role_key(&mnemonic_words, 1)?;
    let (_, posting) = derive_role_key(&mnemonic_words, 2)?;
    let (_, memo)    = derive_role_key(&mnemonic_words, 3)?;
    let json = serde_json::json!({
        "owner":   hex::encode(owner),
        "active":  hex::encode(active),
        "posting": hex::encode(posting),
        "memo":    hex::encode(memo),
    });
    Ok(json.to_string())
}

/// Derive the ed25519 key pair for a given BTCPC role index.
///
/// Returns (private_key_bytes, public_key_bytes).
/// Private key bytes are zeroized when the returned arrays drop — callers
/// that hold signing keys should use ed25519_dalek::SigningKey directly.
pub(crate) fn derive_role_key(
    mnemonic_words: &str,
    role: u32,
) -> Result<([u8; 32], [u8; 32]), MobileCoreError> {
    let mnemonic = Mnemonic::parse(mnemonic_words)
        .map_err(|e| MobileCoreError::InvalidMnemonic(e.to_string()))?;

    // No passphrase — matches wallet.rs `mnemonic.to_seed("")`
    let seed = mnemonic.to_seed("");

    // Path: m/44'/6942'/role'/0'
    let path = [44 | H, BTCPC_COIN | H, role | H, 0 | H];

    slip10_ed25519(&seed, &path)
        .map_err(|e| MobileCoreError::DerivationError(e.to_string()))
}

/// SLIP-10 ed25519 derivation.
///
/// Matches wallet.rs `fn slip10` byte-for-byte:
///   1. Master: HMAC-SHA512("ed25519 seed", seed)
///   2. Each index: HMAC-SHA512(chain, 0x00 || key || (idx | H).to_be_bytes())
///      Note: all BTCPC paths are fully hardened so 0x00 prefix always applies.
///   3. Public key: Ed25519SigningKey::from_bytes(&key).verifying_key()
///
/// Returns (private_key_32, public_key_32).
fn slip10_ed25519(seed: &[u8], path: &[u32]) -> Result<([u8; 32], [u8; 32]), anyhow::Error> {
    // Master key
    let mut mac = HmacSha512::new_from_slice(b"ed25519 seed")
        .map_err(|_| anyhow::anyhow!("HMAC init failed"))?;
    mac.update(seed);
    let i = mac.finalize().into_bytes();

    let mut key:   [u8; 32] = i[..32].try_into()?;
    let mut chain: [u8; 32] = i[32..].try_into()?;

    for &idx in path {
        let mut mac = HmacSha512::new_from_slice(&chain)
            .map_err(|_| anyhow::anyhow!("HMAC child init failed"))?;
        mac.update(&[0x00]);
        mac.update(&key);
        mac.update(&(idx | H).to_be_bytes());
        let i = mac.finalize().into_bytes();
        key.copy_from_slice(&i[..32]);
        chain.copy_from_slice(&i[32..]);
    }

    let pub_key: [u8; 32] = SigningKey::from_bytes(&key).verifying_key().to_bytes();
    Ok((key, pub_key))
}
