//! `wiiv` — CLI for HONE's rendering platform.
//!
//! Skeleton. Today it can print protocol info and probe the local node. Job
//! posting is intentionally NOT wired: the off-chain MCP render layer (src/mcp/)
//! drives dry-run planning, and live chain routes land behind scoped auth per
//! docs/WIIV_PROTOCOL.md. This binary exists so the crate has a runnable entry
//! point matching the other HONE service crates.

use anyhow::Result;
use wiiv::{Capability, RenderModality};

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(|s| s.as_str()).unwrap_or("help");

    match cmd {
        "info" => print_info(),
        "modalities" => print_modalities(),
        "node" => probe_node().await?,
        _ => print_help(),
    }
    Ok(())
}

fn print_help() {
    println!(
        "wiiv — HONE rendering platform CLI\n\n\
         Usage:\n  \
         wiiv info         Show protocol summary and safety state\n  \
         wiiv modalities   List supported render modalities\n  \
         wiiv node         Probe the local hone-node (HONE_API)\n\n\
         Job posting is dry-run only in the MCP layer until scoped auth + chain\n\
         routes exist (see docs/WIIV_PROTOCOL.md)."
    );
}

fn print_info() {
    println!("Wiiv — HONE decentralized rendering platform");
    println!("Reserved accounts: {}, {}", wiiv::ACCOUNT_WIIV, wiiv::ACCOUNT_WIIV_ESCROW);
    println!("Schema version: {}", wiiv::SCHEMA_VERSION);
    println!("Live posting: DISABLED (dry-run only until scoped auth + chain routes)");
}

fn print_modalities() {
    for m in [
        RenderModality::Image,
        RenderModality::Video,
        RenderModality::Audio,
        RenderModality::Threed,
        RenderModality::Composite,
    ] {
        println!("{:?}", m);
    }
    // Touch Capability so the type is exercised in the binary too.
    let _caps = [Capability::ImageGeneration, Capability::VideoGeneration];
}

async fn probe_node() -> Result<()> {
    let client = wiiv::api::Client::from_env();
    match client.node_info().await {
        Ok(info) => {
            let chain = info.get("chain_id").and_then(|v| v.as_str()).unwrap_or("?");
            let epoch = info.get("epoch").and_then(|v| v.as_u64()).unwrap_or(0);
            println!("node ok — chain_id={chain} epoch={epoch}");
            println!("signing identity: {}", if client.has_signing_identity() { "present" } else { "none (read-only)" });
        }
        Err(e) => println!("node probe failed: {e}"),
    }
    Ok(())
}
