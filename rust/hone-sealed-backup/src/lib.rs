//! HONE Sealed Backup — portable core: derivation + envelope + address generation.
//!
//! Spec: `docs/design/hone-sealed-backup.md`. This crate is the single portable
//! brain behind every platform shell (desktop native, mobile bindings, wasm) —
//! the cryptography never forks per platform, so a `.hone-backup` sealed on one
//! platform must open byte-for-byte on any other.
//!
//! Scope of this pass (see the design doc's "what functions today" tier):
//! - Wallet derivation + address generation, reusing `hone-sdk::Wallet` so the
//!   sealed backup derives byte-identical keys to the rest of HONE.
//! - The sealed envelope: one random DEK encrypts the payload once
//!   (AES-256-GCM); the DEK is wrapped per recipient.
//! - Recipients: password (Argon2id) and HONE-wallet (hide-key X25519 ECDH).
//!   External-wallet sign-to-derive and k-of-n guardian recovery are deferred.
//! - Policy: `any` (OR) and `all` (AND) over the recipient list.
//!
//! This is a **create + recover** tool only. It never signs a transaction —
//! signing is the wallet app's job, documented there, not here.

pub mod envelope;
pub mod recipient;
pub mod wallet_material;
pub mod x25519_hide;

pub use envelope::{Policy, SealedBackup};
pub use recipient::Recipient;
pub use wallet_material::WalletMaterial;

#[cfg(test)]
mod conformance_vectors;
