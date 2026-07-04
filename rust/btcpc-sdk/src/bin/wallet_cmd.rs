//! `btcpc wallet` — recoverable wallet creation and unlock.
//!
//! Every wallet created here writes an encrypted, recoverable keystore
//! (`<account>.keystore.json`, Argon2id + AES-256-GCM) into a local vault, AND
//! displays the recovery phrase once. This is the fix for the gap that made
//! accounts unrecoverable (see docs/GENESIS_JULY4_2026.md): you can no longer
//! create a wallet that leaves no recoverable file.
//!
//! Password handling: the keystore password is read from the BTCPC_WALLET_PASSWORD
//! env var (never a CLI arg — args leak into process lists and shell history).
//! The password is never printed, and never written anywhere except as the
//! Argon2id-derived encryption of the mnemonic.
//!
//! Subcommands:
//!   new      --account NAME --vault DIR
//!   import   --account NAME --vault DIR         (mnemonic via BTCPC_WALLET_MNEMONIC)
//!   unlock   --keystore FILE                    (verifies password; prints PUBLIC keys)
//!   pubkeys  --keystore FILE                    (public keys for a genesis entry)

use anyhow::{anyhow, bail, Context, Result};
use btcpc_sdk::keystore::Keystore;
use btcpc_sdk::Wallet;
use std::path::{Path, PathBuf};

pub fn run(args: &[String]) -> Result<i32> {
    // `args` is everything after "wallet", so the subcommand is at index 0.
    match args.get(0).map(String::as_str) {
        Some("new") => cmd_new(&args[1..]),
        Some("import") => cmd_import(&args[1..]),
        Some("unlock") => cmd_unlock(&args[1..]),
        Some("pubkeys") => cmd_pubkeys(&args[1..]),
        _ => {
            eprintln!(
                "usage:\n  \
                 btcpc wallet new    --account NAME --vault DIR\n  \
                 btcpc wallet import --account NAME --vault DIR   (BTCPC_WALLET_MNEMONIC=...)\n  \
                 btcpc wallet unlock --keystore FILE\n  \
                 btcpc wallet pubkeys --keystore FILE\n\n\
                 Password comes from BTCPC_WALLET_PASSWORD (never a CLI arg)."
            );
            Ok(1)
        }
    }
}

fn flag<'a>(args: &'a [String], name: &str) -> Option<&'a str> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).map(String::as_str)
}

fn require_password() -> Result<String> {
    let pw = std::env::var("BTCPC_WALLET_PASSWORD")
        .map_err(|_| anyhow!("set BTCPC_WALLET_PASSWORD (the keystore password). It is never printed or stored in plaintext."))?;
    if pw.is_empty() {
        bail!("BTCPC_WALLET_PASSWORD is empty");
    }
    Ok(pw)
}

fn keystore_path(vault: &str, account: &str) -> PathBuf {
    Path::new(vault).join(format!("{account}.keystore.json"))
}

/// Print the wallet's public keys (never private) and the derived genesis entry.
fn print_public(wallet: &Wallet) -> Result<()> {
    let roles = wallet.btcpc_role_public_keys()?;
    println!("account: {}", wallet.account);
    println!("public keys (roles):");
    // Stable order for readability.
    for role in ["owner", "active", "posting", "memo", "hide", "seek"] {
        if let Some(pk) = roles.get(role) {
            println!("  {role:8} {pk}");
        }
    }
    // Genesis entry snippet — posting key is what genesis.json uses.
    if let Some(posting) = roles.get("posting") {
        println!("\ngenesis.json entry:");
        println!(
            "  \"{}\": {{ \"keys\": {{ \"posting\": \"{}\" }} }}",
            wallet.account, posting
        );
    }
    Ok(())
}

/// Create a new wallet: generate mnemonic, show it once, write encrypted keystore
/// + a public identity file, print public keys.
fn cmd_new(args: &[String]) -> Result<i32> {
    let account = flag(args, "--account").ok_or_else(|| anyhow!("--account NAME required"))?;
    let vault = flag(args, "--vault").ok_or_else(|| anyhow!("--vault DIR required"))?;
    let password = require_password()?;

    let ks_path = keystore_path(vault, account);
    if ks_path.exists() {
        bail!("keystore already exists at {} — refusing to overwrite. Delete it first if you really mean to.", ks_path.display());
    }

    let wallet = Wallet::generate(account).context("generating wallet")?;
    let phrase = wallet.mnemonic.to_string();

    // Encrypt the mnemonic into the recoverable keystore.
    let ks = Keystore::seal(account, &phrase, &password)?;
    ks.save(&ks_path)?;

    // Also write the public identity file (no secrets) next to the keystore.
    let id_path = Path::new(vault).join(format!("{account}.wallet.json"));
    wallet.save_to_file(&id_path)?;

    // Show the recovery phrase ONCE. This is the layer-2 backup the old flow
    // never actually delivered.
    println!("╔══════════════════════════════════════════════════════════════════╗");
    println!("║  RECOVERY PHRASE for '{account}' — WRITE THIS DOWN NOW.            ");
    println!("║  It is shown once. It is the ultimate backup if the keystore file  ║");
    println!("║  and its password are ever lost. Keep it offline and private.      ║");
    println!("╚══════════════════════════════════════════════════════════════════╝");
    println!("\n    {phrase}\n");
    println!("Keystore (encrypted, recoverable): {}", ks_path.display());
    println!("Identity file (public only):        {}", id_path.display());
    println!();
    print_public(&wallet)?;
    println!("\nStore the keystore file safely — with your password it fully recovers this account.");
    Ok(0)
}

/// Import an existing mnemonic (via BTCPC_WALLET_MNEMONIC) into a keystore.
fn cmd_import(args: &[String]) -> Result<i32> {
    let account = flag(args, "--account").ok_or_else(|| anyhow!("--account NAME required"))?;
    let vault = flag(args, "--vault").ok_or_else(|| anyhow!("--vault DIR required"))?;
    let password = require_password()?;
    let phrase = std::env::var("BTCPC_WALLET_MNEMONIC")
        .map_err(|_| anyhow!("set BTCPC_WALLET_MNEMONIC (the phrase to import). It is never printed."))?;
    let phrase = phrase.trim();

    let wallet = Wallet::from_phrase(phrase, account).context("invalid mnemonic")?;
    let ks_path = keystore_path(vault, account);
    if ks_path.exists() {
        bail!("keystore already exists at {} — refusing to overwrite.", ks_path.display());
    }
    let ks = Keystore::seal(account, phrase, &password)?;
    ks.save(&ks_path)?;
    let id_path = Path::new(vault).join(format!("{account}.wallet.json"));
    wallet.save_to_file(&id_path)?;

    println!("Imported '{account}' into recoverable keystore: {}", ks_path.display());
    print_public(&wallet)?;
    Ok(0)
}

/// Unlock a keystore (verify password) and print PUBLIC keys. Never prints the
/// mnemonic or any private key.
fn cmd_unlock(args: &[String]) -> Result<i32> {
    let ks_file = flag(args, "--keystore").ok_or_else(|| anyhow!("--keystore FILE required"))?;
    let password = require_password()?;
    let ks = Keystore::load(Path::new(ks_file))?;
    let phrase = ks.open(&password)?; // errors on wrong password / tamper
    let wallet = Wallet::from_phrase(&phrase, &ks.account)?;
    println!("unlocked ✓ (password correct, file intact)");
    print_public(&wallet)?;
    // phrase drops here; never printed.
    Ok(0)
}

/// Print only the public keys for a keystore — same as unlock but framed for
/// building a genesis entry. Requires the password (must decrypt to derive).
fn cmd_pubkeys(args: &[String]) -> Result<i32> {
    cmd_unlock(args)
}
