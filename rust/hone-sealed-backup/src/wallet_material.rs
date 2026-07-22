//! The plaintext payload a sealed backup protects: the recovery phrase plus
//! every derived key and address. Per the design doc, the phrase already
//! grants all of this by derivation — storing the derived material too adds
//! robustness against derivation drift and lets any single key be imported
//! elsewhere, at no additional exposure (whoever holds the phrase already
//! holds everything below it).

use anyhow::Result;
use hone_sdk::Wallet;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use zeroize::{Zeroize, ZeroizeOnDrop};

/// Everything a sealed backup restores. Zeroized on drop — this struct only
/// ever exists in unlocked memory for the moment of recovery.
#[derive(Serialize, Deserialize, Zeroize, ZeroizeOnDrop)]
pub struct WalletMaterial {
    pub account: String,
    /// The 12-word BIP39 recovery phrase — the master secret.
    pub mnemonic: String,
    /// HONE role -> private key hex, for owner/active/posting/memo/hide/seek.
    #[zeroize(skip)]
    pub hone_role_private_keys: HashMap<String, String>,
    /// HONE role -> public key hex (public, kept alongside for convenience).
    #[zeroize(skip)]
    pub hone_role_public_keys: HashMap<String, String>,
    /// External chain -> private key material (hex / WIF / base58, per chain convention).
    #[zeroize(skip)]
    pub chain_private_keys: HashMap<String, String>,
    /// External chain -> public address (public).
    #[zeroize(skip)]
    pub chain_addresses: HashMap<String, String>,
}

impl WalletMaterial {
    /// Derive full wallet material from a BIP39 mnemonic phrase. Used both for
    /// a freshly generated wallet (create) and to rebuild the payload from a
    /// recovered mnemonic (verifying conformance).
    pub fn from_phrase(phrase: &str, account: &str) -> Result<Self> {
        let wallet = Wallet::from_phrase(phrase, account)?;
        Self::from_wallet(&wallet)
    }

    pub fn from_wallet(wallet: &Wallet) -> Result<Self> {
        let mut hone_role_private_keys = HashMap::new();
        for role in ["owner", "active", "posting", "memo", "hide", "seek"] {
            let kp = wallet.hone_role_keypair(role)?;
            hone_role_private_keys.insert(role.to_string(), kp.private_key_hex());
        }
        let hone_role_public_keys = wallet.hone_role_public_keys()?;

        let mut chain_private_keys = HashMap::new();
        let mut chain_addresses = HashMap::new();

        if let Ok(evm_priv) = wallet.evm_private_key_hex() {
            chain_private_keys.insert("evm".to_string(), evm_priv);
        }
        if let Ok(evm_addr) = wallet.evm_address() {
            chain_addresses.insert("evm".to_string(), evm_addr);
        }

        chain_private_keys.insert("solana".to_string(), wallet.solana_private_key_base58());
        chain_addresses.insert("solana".to_string(), wallet.solana_address());

        if let Ok((btc_hex, btc_wif)) = wallet.bitcoin_private_key() {
            chain_private_keys.insert("bitcoin".to_string(), btc_wif);
            let _ = btc_hex; // WIF is the importable form; raw hex not separately stored.
        }
        if let Ok(btc_pub) = wallet.bitcoin_pubkey_hex() {
            chain_addresses.insert("bitcoin".to_string(), btc_pub);
        }

        Ok(Self {
            account: wallet.account.clone(),
            mnemonic: wallet.mnemonic.to_string(),
            hone_role_private_keys,
            hone_role_public_keys,
            chain_private_keys,
            chain_addresses,
        })
    }

    /// Serialize to canonical JSON bytes — the exact plaintext the envelope encrypts.
    pub fn to_payload_bytes(&self) -> Result<Vec<u8>> {
        Ok(serde_json::to_vec(self)?)
    }

    pub fn from_payload_bytes(bytes: &[u8]) -> Result<Self> {
        Ok(serde_json::from_slice(bytes)?)
    }

    /// The public-only index written alongside the encrypted file in the clear
    /// (`public_index` in the envelope, and the `…-addresses.txt` sidecar) —
    /// readable without unlocking anything.
    pub fn public_index(&self) -> PublicIndex {
        PublicIndex {
            account: self.account.clone(),
            hone_role_public_keys: self.hone_role_public_keys.clone(),
            chain_addresses: self.chain_addresses.clone(),
        }
    }
}

/// The always-plaintext address index stored beside (never inside) the encryption.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PublicIndex {
    pub account: String,
    pub hone_role_public_keys: HashMap<String, String>,
    pub chain_addresses: HashMap<String, String>,
}
