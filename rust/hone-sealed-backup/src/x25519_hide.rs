//! Ed25519 hide-key -> X25519 conversion (the standard birational map, RFC 7748 /
//! libsodium's `crypto_sign_ed25519_sk_to_curve25519` / `..._pk_to_curve25519`).
//!
//! HONE's `hide` role key is derived (via SLIP10-ed25519, same as owner/active/
//! posting) as a signing key everywhere else in the codebase — see
//! `hone-sdk::Wallet::hone_role_keypair("hide")`. Sealed backup is the first
//! consumer that needs it for ECDH rather than signing, so this module performs
//! the conversion HONE-side rather than changing what `hide` derives as.
//!
//! No new cryptography: this is the same clamped-SHA-512-scalar construction
//! `x25519-dalek` itself uses internally for `StaticSecret`, applied to an
//! Ed25519 seed instead of a random one, and the same Edwards->Montgomery
//! point map `curve25519-dalek` exposes as `EdwardsPoint::to_montgomery()`.

use anyhow::{anyhow, Result};
use curve25519_dalek::scalar::clamp_integer;
use ed25519_dalek::{SigningKey, VerifyingKey};
use sha2::{Digest, Sha512};
use x25519_dalek::{PublicKey as XPublicKey, StaticSecret as XStaticSecret};
use zeroize::Zeroize;

/// Convert an Ed25519 signing key to the X25519 static secret sharing the same
/// seed, per RFC 8032's key expansion (SHA-512 of the seed, clamp the first
/// half) — X25519 clamping is identical, so the clamped scalar is valid for
/// both. This is the standard "encrypt to a signing key" bridge; the resulting
/// scalar has no signing meaning and is only ever used for X25519 ECDH.
pub fn signing_key_to_x25519_static(signing_key: &SigningKey) -> XStaticSecret {
    let seed = signing_key.to_bytes(); // 32-byte Ed25519 seed
    let mut hash = Sha512::digest(seed);
    let mut scalar_bytes = [0u8; 32];
    scalar_bytes.copy_from_slice(&hash[..32]);
    hash.zeroize();
    let clamped = clamp_integer(scalar_bytes);
    scalar_bytes.zeroize();
    XStaticSecret::from(clamped)
}

/// Convert an Ed25519 verifying (public) key to its X25519 Montgomery-form
/// public key, via the birational Edwards<->Montgomery map.
pub fn verifying_key_to_x25519_public(verifying_key: &VerifyingKey) -> Result<XPublicKey> {
    let edwards = curve25519_dalek::edwards::CompressedEdwardsY(verifying_key.to_bytes())
        .decompress()
        .ok_or_else(|| anyhow!("invalid ed25519 public key: not a valid curve point"))?;
    Ok(XPublicKey::from(edwards.to_montgomery().to_bytes()))
}

/// Convert a hex-encoded Ed25519 public key (as stored in `WalletFile.hone_role_public_keys["hide"]`)
/// to its X25519 public key, for encrypting *to* a recipient wallet without needing their private key.
pub fn hide_pubkey_hex_to_x25519(hide_pubkey_hex: &str) -> Result<XPublicKey> {
    let bytes = hex::decode(hide_pubkey_hex.trim())?;
    let arr: [u8; 32] = bytes
        .try_into()
        .map_err(|_| anyhow!("hide public key must be exactly 32 bytes"))?;
    let verifying_key = VerifyingKey::from_bytes(&arr)?;
    verifying_key_to_x25519_public(&verifying_key)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ecdh_round_trips_through_the_ed25519_hide_key() {
        // Two independent "wallets"; alice encrypts to bob's hide public key,
        // bob opens it with his hide private key. Standard ECDH check.
        let alice_ephemeral = XStaticSecret::random_from_rng(rand::rngs::OsRng);

        let bob_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let bob_x25519_static = signing_key_to_x25519_static(&bob_signing);
        let bob_x25519_public = XPublicKey::from(&bob_x25519_static);

        // Also verify the pubkey-only derivation path (no private key needed) agrees.
        let bob_x25519_public_from_pub =
            verifying_key_to_x25519_public(&bob_signing.verifying_key()).unwrap();
        assert_eq!(bob_x25519_public.as_bytes(), bob_x25519_public_from_pub.as_bytes());

        let shared_alice = alice_ephemeral.diffie_hellman(&bob_x25519_public);
        let alice_ephemeral_public = XPublicKey::from(&alice_ephemeral);
        let shared_bob = bob_x25519_static.diffie_hellman(&alice_ephemeral_public);

        assert_eq!(shared_alice.as_bytes(), shared_bob.as_bytes());
    }

    #[test]
    fn hex_pubkey_conversion_matches_direct_conversion() {
        let signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let direct = verifying_key_to_x25519_public(&signing.verifying_key()).unwrap();
        let via_hex = hide_pubkey_hex_to_x25519(&hex::encode(signing.verifying_key().to_bytes())).unwrap();
        assert_eq!(direct.as_bytes(), via_hex.as_bytes());
    }
}
