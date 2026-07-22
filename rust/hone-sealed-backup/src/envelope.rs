//! The sealed backup envelope: one DEK encrypts the payload once; the DEK is
//! wrapped per recipient; a policy says how many wraps must open for recovery
//! to succeed. See `docs/design/hone-sealed-backup.md` for the full design.

use aes_gcm::aead::{Aead, KeyInit, OsRng};
use aes_gcm::{Aes256Gcm, Key as AesKey, Nonce};
use anyhow::{anyhow, bail, Result};
use ed25519_dalek::SigningKey;
use rand::RngCore;
use serde::{Deserialize, Serialize};

use crate::recipient::Recipient;
use crate::wallet_material::{PublicIndex, WalletMaterial};

pub const FORMAT_MAGIC: &str = "HONE-SEALED-BACKUP";
pub const FORMAT_VERSION: u8 = 1;
const DEK_LEN: usize = 32;
const NONCE_LEN: usize = 12;

/// How many recipients must successfully unwrap the DEK before the payload
/// decrypts. `KOfN` is defined for forward-compatibility with the design
/// doc's guardian-quorum tier but is not constructible or openable yet —
/// Shamir-splitting the DEK is deferred to a later pass.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Policy {
    /// OR — any single recipient's unwrap succeeds.
    Any,
    /// AND — every recipient in the list must unwrap.
    All,
}

/// The `.hone-backup` file contents.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SealedBackup {
    pub magic: String,
    pub version: u8,
    /// Symmetric cipher used for the payload — always "aes-256-gcm".
    pub cipher: String,
    pub nonce: String,      // hex, 12 bytes
    pub payload_ct: String, // hex, AES-256-GCM(DEK, wallet_material) + tag
    pub recipients: Vec<Recipient>,
    pub policy: Policy,
    /// Public address index — outside the encryption, readable without unlocking.
    pub public_index: PublicIndex,
}

impl SealedBackup {
    /// Seal `material` behind one or more recipients under `policy`. A fresh
    /// DEK is generated here and handed to `build_recipients` so every
    /// recipient wraps the *same* DEK before the payload is encrypted once.
    ///
    /// `policy: All` requires every recipient to independently unwrap at open
    /// time — this crate enforces that at open, not by weakening any
    /// individual wrap (each recipient's wrap is exactly as strong alone as
    /// under `Any`).
    pub fn seal(
        material: &WalletMaterial,
        build_recipients: impl FnOnce(&[u8; DEK_LEN]) -> Result<Vec<Recipient>>,
        policy: Policy,
    ) -> Result<Self> {
        let mut dek = [0u8; DEK_LEN];
        OsRng.fill_bytes(&mut dek);
        let recipients = build_recipients(&dek)?;
        if recipients.is_empty() {
            bail!("a sealed backup needs at least one recipient");
        }
        let result = seal_with_dek(material, &dek, recipients, policy);
        dek.iter_mut().for_each(|b| *b = 0); // best-effort clear of the local DEK copy
        result
    }

    /// Convenience: seal with a single password recipient, policy `Any`.
    pub fn seal_with_password(material: &WalletMaterial, password: &str) -> Result<Self> {
        Self::seal(material, |dek| Ok(vec![Recipient::wrap_password(dek, password)?]), Policy::Any)
    }

    /// Attempt to open with a password. Returns the recovered wallet material
    /// on success. Errors (wrong password, no password recipient, corrupted
    /// file) do not leak which recipient or byte failed beyond "won't open."
    pub fn open_with_password(&self, password: &str) -> Result<WalletMaterial> {
        self.verify_format()?;
        let password_recipients: Vec<&Recipient> = self
            .recipients
            .iter()
            .filter(|r| matches!(r, Recipient::Password { .. }))
            .collect();
        if password_recipients.is_empty() {
            bail!("this backup has no password recipient");
        }

        match self.policy {
            Policy::Any => {
                let dek = password_recipients
                    .iter()
                    .find_map(|r| r.try_unwrap_password(password).ok())
                    .ok_or_else(|| anyhow!("wrong password or corrupted backup"))?;
                self.decrypt_payload(&dek)
            }
            Policy::All => {
                // Policy::All with only a password unwrap available can't be
                // satisfied here — every recipient must independently unwrap.
                if self.recipients.len() > 1 {
                    bail!("this backup requires all recipients (policy: all); password alone is not enough");
                }
                let dek = password_recipients[0].try_unwrap_password(password)?;
                self.decrypt_payload(&dek)
            }
        }
    }

    /// Attempt to open using the recipient wallet's hide signing key.
    pub fn open_with_hone_wallet(&self, hide_signing_key: &SigningKey) -> Result<WalletMaterial> {
        self.verify_format()?;
        let wallet_recipients: Vec<&Recipient> = self
            .recipients
            .iter()
            .filter(|r| matches!(r, Recipient::HoneWallet { .. }))
            .collect();
        if wallet_recipients.is_empty() {
            bail!("this backup has no HONE-wallet recipient");
        }

        match self.policy {
            Policy::Any => {
                let dek = wallet_recipients
                    .iter()
                    .find_map(|r| r.try_unwrap_hone_wallet(hide_signing_key).ok())
                    .ok_or_else(|| anyhow!("wrong wallet or corrupted backup"))?;
                self.decrypt_payload(&dek)
            }
            Policy::All => {
                if self.recipients.len() > 1 {
                    bail!("this backup requires all recipients (policy: all); this wallet alone is not enough");
                }
                let dek = wallet_recipients[0].try_unwrap_hone_wallet(hide_signing_key)?;
                self.decrypt_payload(&dek)
            }
        }
    }

    fn verify_format(&self) -> Result<()> {
        if self.magic != FORMAT_MAGIC {
            bail!("not a HONE sealed backup file (bad magic)");
        }
        if self.version != FORMAT_VERSION {
            bail!("unsupported sealed-backup version {} (expected {})", self.version, FORMAT_VERSION);
        }
        if self.cipher != "aes-256-gcm" {
            bail!("unsupported cipher '{}'", self.cipher);
        }
        Ok(())
    }

    fn decrypt_payload(&self, dek: &[u8; DEK_LEN]) -> Result<WalletMaterial> {
        let nonce_bytes = hex::decode(&self.nonce)?;
        let ct = hex::decode(&self.payload_ct)?;
        let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(dek));
        let plaintext = cipher
            .decrypt(Nonce::from_slice(&nonce_bytes), ct.as_ref())
            .map_err(|_| anyhow!("payload decrypt failed: corrupted or tampered backup"))?;
        WalletMaterial::from_payload_bytes(&plaintext)
    }

    pub fn to_json(&self) -> Result<String> {
        Ok(serde_json::to_string_pretty(self)?)
    }

    pub fn from_json(s: &str) -> Result<Self> {
        Ok(serde_json::from_str(s)?)
    }
}

/// Seal with an explicit, caller-supplied DEK (used internally so multiple
/// recipients can wrap the *same* DEK before the payload is encrypted once).
fn seal_with_dek(material: &WalletMaterial, dek: &[u8; DEK_LEN], recipients: Vec<Recipient>, policy: Policy) -> Result<SealedBackup> {
    let payload = material.to_payload_bytes()?;
    let mut nonce_bytes = [0u8; NONCE_LEN];
    OsRng.fill_bytes(&mut nonce_bytes);
    let cipher = Aes256Gcm::new(AesKey::<Aes256Gcm>::from_slice(dek));
    let payload_ct = cipher
        .encrypt(Nonce::from_slice(&nonce_bytes), payload.as_slice())
        .map_err(|e| anyhow!("payload encryption failed: {e:?}"))?;

    Ok(SealedBackup {
        magic: FORMAT_MAGIC.to_string(),
        version: FORMAT_VERSION,
        cipher: "aes-256-gcm".to_string(),
        nonce: hex::encode(nonce_bytes),
        payload_ct: hex::encode(payload_ct),
        recipients,
        policy,
        public_index: material.public_index(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::x25519_hide::hide_pubkey_hex_to_x25519;

    const TEST_MNEMONIC: &str =
        "legal winner thank year wave sausage worth useful legal winner thank yellow";

    #[test]
    fn password_seal_and_open_round_trips() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let backup = SealedBackup::seal_with_password(&material, "hunter2-but-better").unwrap();

        let recovered = backup.open_with_password("hunter2-but-better").unwrap();
        assert_eq!(recovered.mnemonic, material.mnemonic);
        assert_eq!(recovered.account, "alice");
        assert_eq!(recovered.hone_role_private_keys, material.hone_role_private_keys);
    }

    #[test]
    fn wrong_password_does_not_open() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let backup = SealedBackup::seal_with_password(&material, "right").unwrap();
        assert!(backup.open_with_password("wrong").is_err());
    }

    #[test]
    fn hone_wallet_seal_and_open_round_trips() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let recipient_hide_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let recipient_hide_pub_hex = hex::encode(recipient_hide_signing.verifying_key().to_bytes());

        let backup = SealedBackup::seal(
            &material,
            |dek| Ok(vec![Recipient::wrap_hone_wallet(dek, "alice", &recipient_hide_pub_hex)?]),
            Policy::Any,
        )
        .unwrap();

        let recovered = backup.open_with_hone_wallet(&recipient_hide_signing).unwrap();
        assert_eq!(recovered.mnemonic, material.mnemonic);
    }

    #[test]
    fn any_policy_opens_with_either_recipient() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let recipient_hide_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let recipient_hide_pub_hex = hex::encode(recipient_hide_signing.verifying_key().to_bytes());

        let backup = SealedBackup::seal(
            &material,
            |dek| {
                Ok(vec![
                    Recipient::wrap_password(dek, "pw")?,
                    Recipient::wrap_hone_wallet(dek, "alice", &recipient_hide_pub_hex)?,
                ])
            },
            Policy::Any,
        )
        .unwrap();

        assert_eq!(backup.open_with_password("pw").unwrap().mnemonic, material.mnemonic);
        assert_eq!(
            backup.open_with_hone_wallet(&recipient_hide_signing).unwrap().mnemonic,
            material.mnemonic
        );
    }

    #[test]
    fn all_policy_rejects_single_recipient_when_multiple_required() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let recipient_hide_signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let recipient_hide_pub_hex = hex::encode(recipient_hide_signing.verifying_key().to_bytes());

        let backup = SealedBackup::seal(
            &material,
            |dek| {
                Ok(vec![
                    Recipient::wrap_password(dek, "pw")?,
                    Recipient::wrap_hone_wallet(dek, "alice", &recipient_hide_pub_hex)?,
                ])
            },
            Policy::All,
        )
        .unwrap();

        // Neither single unlock should be sufficient under policy: all.
        assert!(backup.open_with_password("pw").is_err());
        assert!(backup.open_with_hone_wallet(&recipient_hide_signing).is_err());
    }

    #[test]
    fn public_index_is_readable_without_any_recipient() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let backup = SealedBackup::seal_with_password(&material, "pw").unwrap();
        assert_eq!(backup.public_index.account, "alice");
        assert!(!backup.public_index.chain_addresses.is_empty());
    }

    #[test]
    fn tampered_ciphertext_is_rejected() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let mut backup = SealedBackup::seal_with_password(&material, "pw").unwrap();
        let mut ct = hex::decode(&backup.payload_ct).unwrap();
        ct[0] ^= 0xff;
        backup.payload_ct = hex::encode(ct);
        assert!(backup.open_with_password("pw").is_err());
    }

    #[test]
    fn bad_magic_is_rejected() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let mut backup = SealedBackup::seal_with_password(&material, "pw").unwrap();
        backup.magic = "NOT-HONE".to_string();
        assert!(backup.open_with_password("pw").is_err());
    }

    #[test]
    fn json_round_trips() {
        let material = WalletMaterial::from_phrase(TEST_MNEMONIC, "alice").unwrap();
        let backup = SealedBackup::seal_with_password(&material, "pw").unwrap();
        let json = backup.to_json().unwrap();
        let reloaded = SealedBackup::from_json(&json).unwrap();
        assert_eq!(reloaded.open_with_password("pw").unwrap().mnemonic, material.mnemonic);
    }

    #[test]
    fn hide_pubkey_conversion_is_stable() {
        // Sanity: converting the same hex twice gives the same X25519 point.
        let signing = SigningKey::generate(&mut rand::rngs::OsRng);
        let hex_pub = hex::encode(signing.verifying_key().to_bytes());
        let a = hide_pubkey_hex_to_x25519(&hex_pub).unwrap();
        let b = hide_pubkey_hex_to_x25519(&hex_pub).unwrap();
        assert_eq!(a.as_bytes(), b.as_bytes());
    }
}
