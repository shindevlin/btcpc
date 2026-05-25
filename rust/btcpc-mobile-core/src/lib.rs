//! btcpc-mobile-core — UniFFI-bound library for the BTCPC Android client.
//!
//! Surface area is narrow by design:
//!   - SLIP-10 posting key derivation (matching wallet.rs exactly)
//!   - Ed25519 signing of canonical entry messages
//!   - HTTP to /api/node/info and /api/entries
//!   - BLE frame parsing for Flipper Zero sensor data
//!
//! This crate does NOT:
//!   - Store or export private keys
//!   - Implement wallet custody (that belongs in Capacitor Secure Storage)
//!   - Run a node, sync chain state, or maintain a peer connection
//!   - Handle mnemonic backup/restore UI (that belongs in the Capacitor layer)
//!
//! See ARCHITECTURE.md for trust boundary notes.

uniffi::include_scaffolding!("btcpc_mobile_core");

mod derive;
mod sign;
mod http;
mod ble;

// Re-export the public API that UniFFI exposes via the UDL.
pub use derive::{derive_posting_pubkey, derive_all_pubkeys};
pub use sign::{
    sign_transfer, sign_stake, sign_unstake, sign_account_create,
    sign_node_capability, sign_inference_bid, sign_inference_complete,
    sign_coverage_report, sign_sensor_commit,
    sign_with_role,
};
pub use http::{
    fetch_node_info, fetch_balance, fetch_account,
    post_transfer, post_stake, post_unstake, post_account_create,
    post_node_capability, fetch_inference_jobs, post_inference_bid,
    post_inference_complete,
    post_coverage_report, post_sensor_register, post_sensor_commit,
};
pub use ble::{parse_ble_frame, verify_ble_frame_sig, BleFrame};

// ── Error type ────────────────────────────────────────────────────────────────

#[derive(Debug, thiserror::Error)]
pub enum MobileCoreError {
    #[error("invalid mnemonic: {0}")]
    InvalidMnemonic(String),
    #[error("key derivation failed: {0}")]
    DerivationError(String),
    #[error("signing failed: {0}")]
    SigningError(String),
    #[error("network error: {0}")]
    NetworkError(String),
    #[error("parse error: {0}")]
    ParseError(String),
    #[error("invalid BLE frame: {0}")]
    InvalidFrame(String),
    #[error("signature verification failed")]
    SignatureInvalid,
}

impl From<anyhow::Error> for MobileCoreError {
    fn from(e: anyhow::Error) -> Self {
        MobileCoreError::ParseError(e.to_string())
    }
}
