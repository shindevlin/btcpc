//! hone-seal — the standalone, air-gapped wallet-creation & sealed-backup tool.
//!
//! This binary NEVER touches the network. It has no network dependency and makes
//! no outbound connection. It is meant to be run on an offline / air-gapped
//! machine. Subcommands:
//!   create   generate a brand-new HONE wallet, write a password-sealed backup
//!   restore  take an EXISTING recovery phrase, write a password-sealed backup
//!   recover  open a sealed backup with its password and show its contents
//!   inspect  read a sealed backup's PUBLIC address index WITHOUT unlocking it
//!
//! Secrets (recovery phrase + private keys) are printed only when you pass
//! `--show-secrets`, and then only to stderr on your local terminal — they are
//! otherwise written nowhere but the encrypted `.hone-backup` file.
#![forbid(unsafe_code)]

use std::fs;
use std::io::Read;
use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use hone_sdk::Wallet;
use hone_sealed_backup::envelope::SealedBackup;
use hone_sealed_backup::wallet_material::{PublicIndex, WalletMaterial};

const HONE_ROLES: [&str; 6] = ["owner", "active", "posting", "memo", "hide", "seek"];

#[derive(Parser)]
#[command(
    name = "hone-seal",
    version,
    about = "Offline HONE wallet creation & sealed backup (no network, ever)"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Generate a brand-new wallet and write a password-sealed backup file.
    Create {
        /// HONE account name to derive under.
        #[arg(long)]
        account: String,
        /// Output path for the sealed backup (.hone-backup JSON).
        #[arg(long)]
        out: PathBuf,
        /// Read the sealing password from stdin (first line) instead of prompting.
        #[arg(long)]
        password_stdin: bool,
        /// Also print the recovery phrase + private keys to this terminal (stderr).
        #[arg(long)]
        show_secrets: bool,
    },
    /// Re-seal an EXISTING recovery phrase into a new password-sealed backup.
    Restore {
        /// HONE account name to derive under.
        #[arg(long)]
        account: String,
        /// Output path for the sealed backup (.hone-backup JSON).
        #[arg(long)]
        out: PathBuf,
        /// Read the recovery phrase from stdin (first line).
        #[arg(long)]
        phrase_stdin: bool,
        /// Read the sealing password from stdin (the line after the phrase).
        #[arg(long)]
        password_stdin: bool,
        /// Also print the recovery phrase + private keys to this terminal (stderr).
        #[arg(long)]
        show_secrets: bool,
    },
    /// Open a sealed backup with its password and show its contents.
    Recover {
        /// Path to the sealed backup file.
        #[arg(long = "in")]
        input: PathBuf,
        /// Read the password from stdin (first line) instead of prompting.
        #[arg(long)]
        password_stdin: bool,
        /// Also print the recovery phrase + private keys to this terminal (stderr).
        #[arg(long)]
        show_secrets: bool,
    },
    /// Read a sealed backup's PUBLIC address index without unlocking it.
    Inspect {
        /// Path to the sealed backup file.
        #[arg(long = "in")]
        input: PathBuf,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.cmd {
        Cmd::Create { account, out, password_stdin, show_secrets } => {
            cmd_create(&account, &out, password_stdin, show_secrets)
        }
        Cmd::Restore { account, out, phrase_stdin, password_stdin, show_secrets } => {
            cmd_restore(&account, &out, phrase_stdin, password_stdin, show_secrets)
        }
        Cmd::Recover { input, password_stdin, show_secrets } => {
            cmd_recover(&input, password_stdin, show_secrets)
        }
        Cmd::Inspect { input } => cmd_inspect(&input),
    }
}

fn cmd_create(account: &str, out: &PathBuf, password_stdin: bool, show_secrets: bool) -> Result<()> {
    let mut si = maybe_stdin(password_stdin);
    let password = read_password(&mut si, password_stdin, true)?;
    let wallet = Wallet::generate(account).context("generating a new wallet")?;
    let material = WalletMaterial::from_wallet(&wallet)?;
    let backup = SealedBackup::seal_with_password(&material, &password)?;
    write_backup(out, &backup)?;
    println!("New wallet created and sealed to {}", out.display());
    print_public(&material);
    if show_secrets {
        print_secrets(&material);
    } else {
        println!("\n(Recovery phrase + private keys are sealed inside the file. Re-run");
        println!(" `recover --in {} --show-secrets` on an offline machine to view them.)", out.display());
    }
    Ok(())
}

fn cmd_restore(
    account: &str,
    out: &PathBuf,
    phrase_stdin: bool,
    password_stdin: bool,
    show_secrets: bool,
) -> Result<()> {
    let mut si = maybe_stdin(phrase_stdin || password_stdin);
    let phrase = read_phrase(&mut si, phrase_stdin)?;
    let password = read_password(&mut si, password_stdin, true)?;
    let material = WalletMaterial::from_phrase(phrase.trim(), account)
        .context("rebuilding wallet from the supplied phrase")?;
    let backup = SealedBackup::seal_with_password(&material, &password)?;
    write_backup(out, &backup)?;
    println!("Existing phrase re-sealed to {}", out.display());
    print_public(&material);
    if show_secrets {
        print_secrets(&material);
    }
    Ok(())
}

fn cmd_recover(input: &PathBuf, password_stdin: bool, show_secrets: bool) -> Result<()> {
    let mut si = maybe_stdin(password_stdin);
    let password = read_password(&mut si, password_stdin, false)?;
    let json = fs::read_to_string(input).with_context(|| format!("reading {}", input.display()))?;
    let backup = SealedBackup::from_json(&json)?;
    let material = backup.open_with_password(&password)?;
    println!("Opened sealed backup {}", input.display());
    print_public(&material);
    if show_secrets {
        print_secrets(&material);
    } else {
        println!("\n(Pass --show-secrets to print the recovery phrase + private keys.)");
    }
    Ok(())
}

fn cmd_inspect(input: &PathBuf) -> Result<()> {
    let json = fs::read_to_string(input).with_context(|| format!("reading {}", input.display()))?;
    let backup = SealedBackup::from_json(&json)?;
    println!("Public address index for {} (not unlocked):", input.display());
    print_public_index(&backup.public_index);
    Ok(())
}

/// A one-shot reader over stdin lines, consumed in a fixed order (phrase, then
/// password) so a caller can pipe several secrets in on a single stdin.
struct SecretInput {
    lines: std::vec::IntoIter<String>,
}

impl SecretInput {
    fn read_all() -> Self {
        let mut buf = String::new();
        // Blocks until EOF — intended for piped/non-interactive use.
        let _ = std::io::stdin().lock().read_to_string(&mut buf);
        let lines: Vec<String> = buf.lines().map(|s| s.to_string()).collect();
        SecretInput { lines: lines.into_iter() }
    }

    fn next_line(&mut self, what: &str) -> Result<String> {
        self.lines.next().with_context(|| format!("expected {what} on stdin"))
    }
}

/// Build a stdin reader only when at least one secret is coming from stdin.
fn maybe_stdin(needed: bool) -> Option<SecretInput> {
    if needed {
        Some(SecretInput::read_all())
    } else {
        None
    }
}

fn read_phrase(si: &mut Option<SecretInput>, from_stdin: bool) -> Result<String> {
    if from_stdin {
        let s = si
            .as_mut()
            .expect("stdin reader present when phrase_stdin set")
            .next_line("recovery phrase")?;
        return Ok(s);
    }
    let phrase = rpassword::prompt_password("Recovery phrase (hidden): ")
        .context("reading recovery phrase")?;
    if phrase.trim().is_empty() {
        bail!("recovery phrase must not be empty");
    }
    Ok(phrase)
}

fn read_password(si: &mut Option<SecretInput>, from_stdin: bool, confirm: bool) -> Result<String> {
    if from_stdin {
        let pw = si
            .as_mut()
            .expect("stdin reader present when password_stdin set")
            .next_line("password")?;
        if pw.is_empty() {
            bail!("password must not be empty");
        }
        return Ok(pw);
    }
    let pw = rpassword::prompt_password("Password: ").context("reading password")?;
    if confirm {
        let pw2 = rpassword::prompt_password("Confirm password: ").context("reading password")?;
        if pw != pw2 {
            bail!("passwords did not match");
        }
    }
    if pw.is_empty() {
        bail!("password must not be empty");
    }
    Ok(pw)
}

fn print_public(material: &WalletMaterial) {
    print_public_index(&material.public_index());
}

fn print_public_index(idx: &PublicIndex) {
    println!("\nAccount: {}", idx.account);
    println!("HONE role public keys:");
    for role in HONE_ROLES {
        if let Some(pk) = idx.hone_role_public_keys.get(role) {
            println!("  {role:<8} {pk}");
        }
    }
    println!("Chain addresses:");
    let mut chains: Vec<_> = idx.chain_addresses.iter().collect();
    chains.sort_by(|a, b| a.0.cmp(b.0));
    for (chain, addr) in chains {
        println!("  {chain:<8} {addr}");
    }
}

/// Print the recovery phrase and private keys to STDERR (never stdout), guarded
/// behind the explicit `--show-secrets` flag. Intended only for a trusted,
/// offline terminal.
fn print_secrets(material: &WalletMaterial) {
    eprintln!("\n=== SECRETS — write these down offline, never share, never photograph ===");
    eprintln!("Recovery phrase:");
    eprintln!("  {}", material.mnemonic);
    eprintln!("HONE role private keys:");
    for role in HONE_ROLES {
        if let Some(sk) = material.hone_role_private_keys.get(role) {
            eprintln!("  {role:<8} {sk}");
        }
    }
    eprintln!("Chain private keys:");
    let mut chains: Vec<_> = material.chain_private_keys.iter().collect();
    chains.sort_by(|a, b| a.0.cmp(b.0));
    for (chain, sk) in chains {
        eprintln!("  {chain:<8} {sk}");
    }
    eprintln!("=== end secrets ===");
}

fn write_backup(out: &PathBuf, backup: &SealedBackup) -> Result<()> {
    let json = backup.to_json()?;
    fs::write(out, json).with_context(|| format!("writing {}", out.display()))?;

    // Plaintext public-address sidecar, next to the sealed file.
    let sidecar = out.with_extension("addresses.txt");
    let idx = &backup.public_index;
    let mut s = String::new();
    s.push_str(&format!("account: {}\n", idx.account));
    for role in HONE_ROLES {
        if let Some(pk) = idx.hone_role_public_keys.get(role) {
            s.push_str(&format!("hone.{role}: {pk}\n"));
        }
    }
    let mut chains: Vec<_> = idx.chain_addresses.iter().collect();
    chains.sort_by(|a, b| a.0.cmp(b.0));
    for (chain, addr) in chains {
        s.push_str(&format!("{chain}: {addr}\n"));
    }
    fs::write(&sidecar, s).with_context(|| format!("writing {}", sidecar.display()))?;
    Ok(())
}
