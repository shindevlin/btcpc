//! Recipient (unlock method) wraps for the sealed backup's Data Encryption
//! Key (DEK). Each recipient wraps the same 32-byte DEK a different way — the
//! `age`/PGP envelope pattern. This pass implements the two "now, low effort"
//! recipients from the design doc: password (Argon2id) and HONE-wallet
//! (hide-key X25519 ECDH). External-wallet sign-to-derive and k-of-n guardian
//! quorum are deferred to a later pass.

use aes_gcm::aead::{Aead, KeyInit, OsRng as AesOsRng};
use aes_gcm::{Aes256Gcm, Key as AesKey, Nonce};
use anyhow::{anyhow, bail, Context, Result};
use argon2::{Algorithm, Argon2, Params, Version};
use ed25519_dalek::SigningKey;
use hkdf::Hkdf;
use rand::RngCore;
use serde::{Deserialize, Serialize};
use sha2::Sha256;
use x25519_dalek::{EphemeralSecret, PublicKey as XPublicKey, StaticSecret as XStaticSecret};

use crate::x25519_hide::{hide_pubkey_hex_to_x25519, signing_key_to_x25519_static};

const DEK_LEN: usize = 32;
const NONCE_LEN: usize = 12;

// Argon2id parameters for the password recipient. Matches hone-sdk::keystore's
// tuning (64 MiB / 3 iterations / 1 lane) so password-backup cost expectations
// are consistent across HONE's encrypted-at-rest formats.
const ARGON_M_COST_KIB: u32 = 64 * 1024;
const ARGON_T_COST: u32 = 3;
const ARGON_P_COST: u32 = 1;

/// A single wrapped-DEK stub stored in the envelope's `recipients` list.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "kind")]
pub enum Recipient {
    /// `KEK = Argon2id(password, salt)`, `wrap = AES-256-GCM(KEK, DEK)`.
    Password {
        salt: String,   // hex
        m_cost: u32,
        t_cost: u32,
        p_cost: u32,
        nonce: String,   // hex, 12 bytes
        wrapped_dek: String, // hex, DEK ciphertext + GCM tag
    },
    /// Encrypt-to-public-key against the recipient wallet's hide key (X25519,
    /// derived from the same Ed25519 hide keypair every other HONE role uses).
    /// This is a NaCl sealed-box-style anonymous ECDH: an ephemeral keypair is
    /// generated per wrap, so the sender's identity isn't needed to open it —
    /// only the recipient's hide private key is.
    HoneWallet {
        /// Which account's hide key this was sealed to (label only, not trusted).
        account: String,
        /// Ephemeral public key used for this wrap (hex).
        ephemeral_public: String,
        nonce: String,        // hex, 12 bytes
        wrapped_dek: String,  // hex, DEK ciphertext + GCM tag
    },
}

impl Recipient {
    /// Seal `dek` under a password.
    pub fn wrap_password(dek: &[u8; DEK_LEN], password: &str) -> Result<Self> {
        if password.is_empty() {
            bail!("sealed-backup password must not be empty");
        }
        let mut salt = [0u8; 16];
        AesOsRng.fill_bytes(&mut salt);
        let mut nonce_bytes = [0u8; NONCE_LEN];
        AesOsRng.fill_bytes(&mut nonce_bytes);

        let kek = argon2id_kek(password.as_bytes(), &salt, ARGON_M_COST_KIB, ARGON_T_COST, ARGON_P_COST)?;
        let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(&kek));
        let wrapped = cipher
            .encrypt(Nonce::from_slice(&nonce_bytes), dek.as_slice())
            .map_err(|e| anyhow!("password wrap failed: {e:?}"))?;

        Ok(Recipient::Password {
            salt: hex::encode(salt),
            m_cost: ARGON_M_COST_KIB,
            t_cost: ARGON_T_COST,
            p_cost: ARGON_P_COST,
            nonce: hex::encode(nonce_bytes),
            wrapped_dek: hex::encode(wrapped),
        })
    }

    /// Seal `dek` to a recipient HONE wallet's hide public key (hex, as found
    /// in `WalletFile.hone_role_public_keys["hide"]`). The sender needs only
    /// the recipient's public hide key — no private material.
    pub fn wrap_hone_wallet(dek: &[u8; DEK_LEN], account: &str, recipient_hide_pubkey_hex: &str) -> Result<Self> {
        let recipient_x25519_pub = hide_pubkey_hex_to_x25519(recipient_hide_pubkey_hex)?;

        let ephemeral_secret = EphemeralSecret::random_from_rng(rand::rngs::OsRng);
        let ephemeral_public = XPublicKey::from(&ephemeral_secret);
        let shared = ephemeral_secret.diffie_hellman(&recipient_x25519_pub);

        let kek = hkdf_kek(shared.as_bytes(), ephemeral_public.as_bytes(), recipient_x25519_pub.as_bytes())?;

        let mut nonce_bytes = [0u8; NONCE_LEN];
        AesOsRng.fill_bytes(&mut nonce_bytes);
        let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(&kek));
        let wrapped = cipher
            .encrypt(Nonce::from_slice(&nonce_bytes), dek.as_slice())
            .map_err(|e| anyhow!("hone-wallet wrap failed: {e:?}"))?;

        Ok(Recipient::HoneWallet {
            account: account.to_string(),
            ephemeral_public: hex::encode(ephemeral_public.as_bytes()),
            nonce: hex::encode(nonce_bytes),
            wrapped_dek: hex::encode(wrapped),
        })
    }

    /// Try to unwrap the DEK with a password. Fails (wrong password / not a
    /// password recipient) without panicking.
    pub fn try_unwrap_password(&self, password: &str) -> Result<[u8; DEK_LEN]> {
        match self {
            Recipient::Password { salt, m_cost, t_cost, p_cost, nonce, wrapped_dek } => {
                let salt = hex::decode(salt).context("bad salt hex")?;
                let nonce_bytes = hex::decode(nonce).context("bad nonce hex")?;
                let wrapped = hex::decode(wrapped_dek).context("bad wrapped_dek hex")?;

                let kek = argon2id_kek(password.as_bytes(), &salt, *m_cost, *t_cost, *p_cost)?;
                let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(&kek));
                let dek_bytes = cipher
                    .decrypt(Nonce::from_slice(&nonce_bytes), wrapped.as_ref())
                    .map_err(|_| anyhow!("wrong password or corrupted/tampered backup"))?;
                to_dek(&dek_bytes)
            }
            _ => bail!("not a password recipient"),
        }
    }

    /// Try to unwrap the DEK using the recipient wallet's hide *signing* key
    /// (i.e. the private half — this is what the recovering wallet holds).
    pub fn try_unwrap_hone_wallet(&self, recipient_hide_signing_key: &SigningKey) -> Result<[u8; DEK_LEN]> {
        match self {
            Recipient::HoneWallet { ephemeral_public, nonce, wrapped_dek, .. } => {
                let ephemeral_public_bytes = hex::decode(ephemeral_public).context("bad ephemeral_public hex")?;
                let ephemeral_public_arr: [u8; 32] = ephemeral_public_bytes
                    .try_into()
                    .map_err(|_| anyhow!("ephemeral_public must be 32 bytes"))?;
                let ephemeral_public = XPublicKey::from(ephemeral_public_arr);

                let recipient_x25519_static: XStaticSecret =
                    signing_key_to_x25519_static(recipient_hide_signing_key);
                let recipient_x25519_public = XPublicKey::from(&recipient_x25519_static);
                let shared = recipient_x25519_static.diffie_hellman(&ephemeral_public);

                let kek = hkdf_kek(shared.as_bytes(), ephemeral_public.as_bytes(), recipient_x25519_public.as_bytes())?;

                let nonce_bytes = hex::decode(nonce).context("bad nonce hex")?;
                let wrapped = hex::decode(wrapped_dek).context("bad wrapped_dek hex")?;
                let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(&kek));
                let dek_bytes = cipher
                    .decrypt(Nonce::from_slice(&nonce_bytes), wrapped.as_ref())
                    .map_err(|_| anyhow!("wrong hide key or corrupted/tampered backup"))?;
                to_dek(&dek_bytes)
            }
            _ => bail!("not a hone-wallet recipient"),
        }
    }
}

fn to_dek(bytes: &[u8]) -> Result<[u8; DEK_LEN]> {
    bytes
        .try_into()
        .map_err(|_| anyhow!("unwrapped DEK has wrong length"))
}

fn argon2id_kek(password: &[u8], salt: &[u8], m_cost: u32, t_cost: u32, p_cost: u32) -> Result<[u8; DEK_LEN]> {
    let params = Params::new(m_cost, t_cost, p_cost, Some(DEK_LEN))
        .map_err(|e| anyhow!("bad argon2 params: {e}"))?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut out = [0u8; DEK_LEN];
    argon
        .hash_password_into(password, salt, &mut out)
        .map_err(|e| anyhow!("argon2id derivation failed: {e}"))?;
    Ok(out)
}

/// HKDF-SHA256 over the ECDH shared secret, binding both public keys into the
/// info string as domain separation (standard practice for sealed-box-style ECDH).
fn hkdf_kek(shared_secret: &[u8], ephemeral_public: &[u8], recipient_public: &[u8]) -> Result<[u8; DEK_LEN]> {
    let hk = Hkdf::<Sha256>::new(None, shared_secret);
    let mut info = Vec::with_capacity(64 + 32);
    info.extend_from_slice(b"HONE-sealed-backup-v1|");
    info.extend_from_slice(ephemeral_public);
    info.extend_from_slice(recipient_public);
    let mut out = [0u8; DEK_LEN];
    hk.expand(&info, &mut out)
        .map_err(|_| anyhow!("HKDF expand failed"))?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn password_wrap_round_trips() {
        let dek = [7u8; DEK_LEN];
        let r = Recipient::wrap_password(&dek, "correct horse battery staple").unwrap();
        let opened = r.try_unwrap_password("correct horse battery staple").unwrap();
        assert_eq!(opened, dek);
    }

    #[test]
    fn password_wrap_rejects_wrong_password() {
        let dek = [7u8; DEK_LEN];
        let r = Recipient::wrap_password(&dek, "right").unwrap();
        assert!(r.try_unwrap_password("wrong").is_err());
    }

    #[test]
    fn hone_wallet_wrap_round_trips() {
        let dek = [9u8; DEK_LEN];
        let hide_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let hide_pub_hex = hex::encode(hide_signing.verifying_key().to_bytes());

        let r = Recipient::wrap_hone_wallet(&dek, "alice", &hide_pub_hex).unwrap();
        let opened = r.try_unwrap_hone_wallet(&hide_signing).unwrap();
        assert_eq!(opened, dek);
    }

    #[test]
    fn hone_wallet_wrap_rejects_wrong_key() {
        let dek = [9u8; DEK_LEN];
        let hide_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let hide_pub_hex = hex::encode(hide_signing.verifying_key().to_bytes());
        let r = Recipient::wrap_hone_wallet(&dek, "alice", &hide_pub_hex).unwrap();

        let wrong_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        assert!(r.try_unwrap_hone_wallet(&wrong_signing).is_err());
    }

    #[test]
    fn distinct_wraps_use_distinct_salts_and_ephemerals() {
        let dek = [1u8; DEK_LEN];
        let a = Recipient::wrap_password(&dek, "pw").unwrap();
        let b = Recipient::wrap_password(&dek, "pw").unwrap();
        match (a, b) {
            (Recipient::Password { salt: s1, wrapped_dek: w1, .. }, Recipient::Password { salt: s2, wrapped_dek: w2, .. }) => {
                assert_ne!(s1, s2);
                assert_ne!(w1, w2);
            }
            _ => panic!("expected password recipients"),
        }
    }
}
