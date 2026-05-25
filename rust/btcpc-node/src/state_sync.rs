//! Fast-sync — downloads a RocksDB state snapshot from a healthy peer when this
//! node's finalized epoch is more than SYNC_THRESHOLD epochs behind.
//!
//! Called at startup BEFORE Store::open. If a sync succeeds, the local state/
//! directory is replaced and startup continues normally against the fresh state.

use std::path::Path;
use anyhow::{bail, Result};
use tracing::{info, warn};

/// Gap (in finalized epochs) that triggers a snapshot download rather than gossip catch-up.
/// 100 epochs ≈ 50 minutes of missed blocks.
const SYNC_THRESHOLD: u64 = 100;

/// Entry point — call before Store::open in main.
/// Returns true if a snapshot was downloaded and applied.
pub async fn maybe_sync(
    data_dir: &Path,
    chain_id: &str,
    sync_peers: &[String],
) -> Result<bool> {
    if sync_peers.is_empty() {
        return Ok(false);
    }

    let state_dir = data_dir.join("state");
    let local_finalized = read_local_finality(&state_dir);

    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(15))
        .build()?;

    for peer in sync_peers {
        let info = match query_sync_info(&client, peer).await {
            Ok(i)  => i,
            Err(e) => { warn!("[fast-sync] peer {} unreachable: {}", peer, e); continue; }
        };

        if info.chain_id != chain_id {
            warn!("[fast-sync] peer {} is chain '{}', expected '{}' — skipping",
                peer, info.chain_id, chain_id);
            continue;
        }

        let gap = info.finalized_epoch.saturating_sub(local_finalized);
        info!("[fast-sync] local={} peer={} gap={} threshold={}",
            local_finalized, info.finalized_epoch, gap, SYNC_THRESHOLD);

        if gap <= SYNC_THRESHOLD {
            return Ok(false);
        }

        if !info.snapshot_ready {
            warn!("[fast-sync] peer {} gap={} but snapshot not ready — skipping", peer, gap);
            continue;
        }

        info!("[fast-sync] gap {} > {} — downloading snapshot from {}", gap, SYNC_THRESHOLD, peer);
        match download_and_apply(&client, peer, data_dir, &state_dir).await {
            Ok(()) => {
                info!("[fast-sync] snapshot applied — resuming from epoch ~{}", info.finalized_epoch);
                return Ok(true);
            }
            Err(e) => {
                warn!("[fast-sync] download from {} failed: {} — trying next peer", peer, e);
                restore_backup_if_needed(data_dir);
                continue;
            }
        }
    }

    Ok(false)
}

// ── Types ─────────────────────────────────────────────────────────────────────

#[derive(serde::Deserialize)]
struct SyncInfo {
    chain_id:        String,
    finalized_epoch: u64,
    snapshot_ready:  bool,
}

// ── Helpers ───────────────────────────────────────────────────────────────────

async fn query_sync_info(client: &reqwest::Client, peer: &str) -> Result<SyncInfo> {
    let url  = format!("{}/api/sync/info", peer.trim_end_matches('/'));
    let resp = client.get(&url).send().await?;
    if !resp.status().is_success() {
        bail!("HTTP {}", resp.status());
    }
    Ok(resp.json::<SyncInfo>().await?)
}

async fn download_and_apply(
    client:    &reqwest::Client,
    peer:      &str,
    data_dir:  &Path,
    state_dir: &Path,
) -> Result<()> {
    use tokio::io::AsyncWriteExt;

    let url = format!("{}/api/sync/state", peer.trim_end_matches('/'));
    let mut resp = client
        .get(&url)
        .timeout(std::time::Duration::from_secs(300))
        .send()
        .await?;

    if !resp.status().is_success() {
        bail!("HTTP {}", resp.status());
    }

    // Stream to a temp file rather than buffering the whole snapshot in RAM.
    let tmp_path = data_dir.join("state_sync.tmp.tar.gz");
    {
        let mut file = tokio::fs::File::create(&tmp_path).await?;
        while let Some(chunk) = resp.chunk().await? {
            file.write_all(&chunk).await?;
        }
        file.flush().await?;
    }

    // Rotate: move current state aside, extract snapshot, then clean up backup.
    let bak_dir = data_dir.join("state.bak");
    if state_dir.exists() {
        if bak_dir.exists() { std::fs::remove_dir_all(&bak_dir)?; }
        std::fs::rename(state_dir, &bak_dir)?;
        info!("[fast-sync] old state backed up to {:?}", bak_dir);
    }

    let status = std::process::Command::new("tar")
        .args(["-xzf",
               tmp_path.to_str().unwrap_or(""),
               "-C",
               data_dir.to_str().unwrap_or(".")])
        .status()?;

    std::fs::remove_file(&tmp_path).ok();

    if !status.success() {
        bail!("tar extraction failed (exit {:?})", status.code());
    }

    // Each node must generate its own libp2p identity — never copy a peer's key.
    for name in &["p2p-key", "node.lock"] {
        std::fs::remove_file(data_dir.join(name)).ok();
    }

    if bak_dir.exists() { std::fs::remove_dir_all(&bak_dir).ok(); }

    Ok(())
}

fn restore_backup_if_needed(data_dir: &Path) {
    let state_dir = data_dir.join("state");
    let bak_dir   = data_dir.join("state.bak");
    if bak_dir.exists() && !state_dir.exists() {
        if let Err(e) = std::fs::rename(&bak_dir, &state_dir) {
            warn!("[fast-sync] could not restore backup: {}", e);
        } else {
            info!("[fast-sync] restored previous state from backup");
        }
    }
}

/// Open RocksDB briefly, read latest finalized epoch, then drop the handle.
/// Returns 0 if the state dir doesn't exist yet (fresh install).
fn read_local_finality(state_dir: &Path) -> u64 {
    if !state_dir.exists() {
        return 0;
    }
    match crate::store::Store::open(state_dir) {
        Ok(store) => store.latest_finality().unwrap_or(0),
        Err(_)    => 0,
    }
}
