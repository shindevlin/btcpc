//! Verified late-join state synchronization.
//!
//! A joiner must obtain the same `(epoch, state_root)` from two independent
//! HTTP peers, verify the imported balances locally, and persist the reward
//! watermark before it is allowed to seal.  Blocks are backfilled for serving
//! and validation only; they are never replayed as rewards.

use std::sync::{Arc, atomic::{AtomicBool, Ordering}};
use std::time::Duration;
use anyhow::{Context, Result, anyhow};
use serde::Deserialize;
use crate::{chain::Chain, net::NetworkHandle};

#[derive(Debug, Deserialize, Clone)]
struct Snapshot {
    epoch: u64,
    state_root: String,
    #[serde(default)] balances: Vec<Balance>,
}

#[derive(Debug, Deserialize, Clone)]
struct Balance { account: String, token: String, amount: u64 }

#[derive(Debug, Deserialize)]
struct Blocks { #[serde(default)] blocks: Vec<serde_json::Value> }

fn agreed_snapshot(snaps: &[(String, Snapshot)]) -> Option<(String, Snapshot)> {
    snaps.iter()
        .find(|(_, s)| snaps.iter().filter(|(_, x)| x.epoch == s.epoch && x.state_root == s.state_root).count() >= 2)
        .cloned()
}

pub fn requires_catchup(chain: &Chain) -> bool {
    match chain.store.state_get("state_sync_status").as_deref() {
        Some(b"complete") => false,
        Some(b"pending") => true,
        _ => chain.store.latest_epoch().is_none() && chain.store.scan_balance_entries().is_empty(),
    }
}

/// Retry until two peers agree and the local import verifies.  Failure is
/// fail-closed: `ready` remains false, which prevents a cold joiner sealing.
pub async fn run(chain: Arc<Chain>, net: NetworkHandle, ready: Arc<AtomicBool>) {
    if !requires_catchup(&chain) { ready.store(true, Ordering::Release); return; }
    let _ = chain.store.state_set("state_sync_status", b"pending");
    let client = match reqwest::Client::builder().timeout(Duration::from_secs(8)).build() {
        Ok(c) => c, Err(e) => { tracing::error!("[sync] HTTP client: {}", e); return; }
    };
    loop {
        let peers = net.peer_http_urls();
        let mut snaps: Vec<(String, Snapshot)> = Vec::new();
        for base in peers {
            let url = format!("{}/api/sync/snapshot", base.trim_end_matches('/'));
            if let Ok(resp) = client.get(url).send().await {
                if let Ok(resp) = resp.error_for_status() {
                    if let Ok(s) = resp.json::<Snapshot>().await {
                        snaps.push((base.clone(), s));
                    }
                }
            }
            if snaps.len() >= 2 { break; }
        }
        if snaps.len() < 2 { tokio::time::sleep(Duration::from_secs(5)).await; continue; }
        let agreed = agreed_snapshot(&snaps);
        let Some((peer_base, snapshot)) = agreed else {
            tracing::error!("[sync] peers disagree on snapshot epoch/state_root; refusing import");
            tokio::time::sleep(Duration::from_secs(10)).await; continue;
        };
        let entries: Vec<(String, String, u64)> = snapshot.balances.iter()
            .map(|b| (b.account.clone(), b.token.clone(), b.amount)).collect();
        if entries.is_empty() && snapshot.state_root != "0".repeat(64) {
            tracing::error!("[sync] agreed snapshot omitted balances; refusing import");
            tokio::time::sleep(Duration::from_secs(10)).await; continue;
        }
        if let Err(e) = import_verified(&chain, &client, &peer_base, &snapshot, &entries).await {
            tracing::error!("[sync] catch-up failed closed: {:#}", e);
            tokio::time::sleep(Duration::from_secs(10)).await; continue;
        }
        ready.store(true, Ordering::Release);
        tracing::info!("[sync] verified snapshot epoch {} root {}; sealing enabled", snapshot.epoch, snapshot.state_root);
        return;
    }
}

async fn import_verified(chain: &Chain, client: &reqwest::Client, peer_base: &str, snapshot: &Snapshot, entries: &[(String, String, u64)]) -> Result<()> {
    let old = chain.store.scan_balance_entries();
    chain.store.replace_balance_entries(entries).context("replace balances")?;
    if chain.store.balance_merkle_root() != snapshot.state_root {
        chain.store.replace_balance_entries(&old).context("rollback balances")?;
        return Err(anyhow!("snapshot state_root recomputation mismatch"));
    }
    chain.store.set_reward_watermark(snapshot.epoch).context("persist reward watermark")?;
    // Backfill sealed blocks for serving/validation only.  Never feed these
    // blocks through reward application: the snapshot already contains their
    // balance effects.
    let url = format!("{}/api/sync/blocks?from={}", peer_base.trim_end_matches('/'), snapshot.epoch);
    let blocks: Blocks = client.get(url).send().await?.error_for_status()?.json().await?;
    for raw in blocks.blocks {
        let epoch = raw.get("epoch").and_then(|v| v.as_u64()).unwrap_or(0);
        let block: hone_types::Block = serde_json::from_value(serde_json::json!({
            "header": raw.get("header").cloned().unwrap_or_default(),
            "payload": raw.get("payload").cloned().unwrap_or_default(),
        })).context("decode backfill block")?;
        chain.store.write_block(epoch, &block.to_bytes()).context("write backfill block")?;
    }
    chain.store.state_set("state_sync_status", b"complete").context("persist sync completion")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{agreed_snapshot, Snapshot};

    fn snapshot(epoch: u64, state_root: &str) -> Snapshot {
        Snapshot { epoch, state_root: state_root.to_owned(), balances: Vec::new() }
    }

    #[test]
    fn state_sync_accepts_two_matching_peer_commitments() {
        let peers = vec![
            ("http://peer-a".to_owned(), snapshot(7, "a")),
            ("http://peer-b".to_owned(), snapshot(7, "a")),
            ("http://peer-c".to_owned(), snapshot(8, "b")),
        ];

        let (peer, selected) = agreed_snapshot(&peers).expect("two peers agree");
        assert_eq!(peer, "http://peer-a");
        assert_eq!(selected.epoch, 7);
        assert_eq!(selected.state_root, "a");
    }

    #[test]
    fn state_sync_rejects_unmatched_peer_commitments() {
        let peers = vec![
            ("http://peer-a".to_owned(), snapshot(7, "a")),
            ("http://peer-b".to_owned(), snapshot(7, "b")),
            ("http://peer-c".to_owned(), snapshot(8, "c")),
        ];

        assert!(agreed_snapshot(&peers).is_none());
    }
}
