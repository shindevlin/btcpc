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
use sha2::{Sha256, Digest as _};
use crate::{chain::Chain, net::NetworkHandle, store::AtomicSyncState};

#[derive(Debug, Deserialize, Clone)]
struct Snapshot {
    epoch: u64,
    state_root: String,
    #[serde(default)] digest: String,
    #[serde(default)] balances: Vec<Balance>,
    #[serde(default)] accounts: Vec<AccountEntry>,
    #[serde(default)] clock_registrations: Vec<MetaEntry>,
    #[serde(default)] alive_epochs: Vec<AliveEntry>,
    #[serde(default)] emission_state: Vec<MetaEntry>,
}

#[derive(Debug, Deserialize, Clone)]
struct Balance { account: String, token: String, amount: u64 }

#[derive(Debug, Deserialize, Clone)]
struct AccountEntry { id: String, state: serde_json::Value }

#[derive(Debug, Deserialize, Clone)]
struct MetaEntry { key: String, value_hex: String }

#[derive(Debug, Deserialize, Clone)]
struct AliveEntry { account: String, epoch: u64 }

/// Deterministic digest over the FULL restored consensus state — balances,
/// accounts, clock-registration state, and the alive-epoch map — not just the
/// balance root.  `AtomicSyncState`'s collections are already sorted by key, so
/// this is stable across nodes independent of RocksDB iteration order.  Bound to
/// `epoch` so a peer that agrees on data but mislabels the epoch cannot pass
/// agreement.  Used both to require ≥2-peer agreement before import (trust
/// minimization) and, after import, to verify the restored state matches
/// byte-for-byte before enabling sealing (fail-closed, same shape as the
/// pre-existing balance-root check).
pub(crate) fn consensus_digest(epoch: u64, state: &AtomicSyncState) -> String {
    let mut h = Sha256::new();
    h.update(b"epoch:");
    h.update(epoch.to_le_bytes());
    h.update(b"|balances:");
    for (account, token, amount) in &state.balances {
        h.update(account.as_bytes());
        h.update(b"\0");
        h.update(token.as_bytes());
        h.update(b"=");
        h.update(amount.to_le_bytes());
        h.update(b";");
    }
    h.update(b"|accounts:");
    for (id, value) in &state.accounts {
        h.update(id.as_bytes());
        h.update(b"=");
        h.update(serde_json::to_vec(value).unwrap_or_default());
        h.update(b";");
    }
    h.update(b"|clock_registrations:");
    for (key, value) in &state.clock_registrations {
        h.update(key.as_bytes());
        h.update(b"=");
        h.update(value);
        h.update(b";");
    }
    h.update(b"|alive_epochs:");
    for (account, epoch) in &state.alive_epochs {
        h.update(account.as_bytes());
        h.update(b"=");
        h.update(epoch.to_le_bytes());
        h.update(b";");
    }
    h.update(b"|emission_state:");
    for (key, value) in &state.emission_state {
        h.update(key.as_bytes());
        h.update(b"=");
        h.update(value);
        h.update(b";");
    }
    format!("{:x}", h.finalize())
}

#[derive(Debug, Deserialize)]
struct Blocks { #[serde(default)] blocks: Vec<serde_json::Value> }

/// Epoch represented by the durable balance snapshot.  The block tip can be
/// ahead of reward application by the finality depth, so labeling balances with
/// `latest_epoch()` would let a joiner import a root whose epoch lies in the
/// middle of an unfinished reward window.
pub fn snapshot_epoch(chain: &Chain) -> u64 {
    let done: std::collections::HashSet<u64> = chain.store
        .state_scan_prefix("epoch_finalized_done:")
        .into_iter()
        .filter_map(|(k, _)| k.rsplit(':').next().and_then(|s| s.parse::<u64>().ok()))
        .collect();
    contiguous_finalized(&done, chain.store.reward_watermark())
}

/// Highest epoch reachable by an unbroken run of finalized epochs, anchored at the
/// LOWEST finalized epoch rather than at 1.
///
/// HONE's genesis is backdated, so a chain boots at a high epoch and a launch
/// cohort only ever finalizes epochs from its launch epoch F onward — the key
/// `epoch_finalized_done:1` never exists.  Anchoring the walk at 1 therefore broke
/// on the first step and returned 0, labeling a real, fully-populated snapshot as
/// epoch 0.  The importer's `epoch == 0 && state_root != 0*64` guard then refused
/// the import (correctly — the guard is not the bug), so a late joiner could never
/// complete catch-up and never enabled sealing.  Anchoring at `min(done)` yields
/// the true highest contiguous finalized epoch.  The finality-depth-behind-tip
/// semantics are unchanged and intended: the block tip may be ahead of reward
/// application, so labeling with `latest_epoch()` would hand a joiner a root from
/// the middle of an unfinished reward window.
pub(crate) fn contiguous_finalized(done: &std::collections::HashSet<u64>, watermark: u64) -> u64 {
    if done.is_empty() {
        return watermark;
    }
    let min = done.iter().copied().min().unwrap_or(0);
    let max = done.iter().copied().max().unwrap_or(0);
    let mut e = min;
    while e < max && done.contains(&(e + 1)) {
        e += 1;
    }
    e.max(watermark)
}

/// Peer agreement is over the FULL consensus-state digest, not just
/// (epoch, state_root) — two peers could agree on balances while disagreeing on
/// clock-registration or alive-epoch state, which would still fork the joiner.
fn agreed_snapshot(snaps: &[(String, Snapshot)]) -> Option<(String, Snapshot)> {
    snaps.iter()
        .find(|(_, s)| snaps.iter().filter(|(_, x)|
            x.epoch == s.epoch && x.state_root == s.state_root && x.digest == s.digest
        ).count() >= 2)
        .cloned()
}

pub fn requires_catchup(chain: &Chain) -> bool {
    // The isolated Pass-B harness labels its launch cohort explicitly.  This
    // avoids mistaking a brand-new genesis node (which must be allowed to form
    // the initial cohort) for a late joiner, while the late-joiner label forces
    // the exact cold-start path under test.  Production leaves this unset.
    match std::env::var("HONE_STATE_SYNC_MODE").ok().as_deref() {
        Some("launch") => return false,
        Some("late-joiner") => return true,
        _ => {}
    }
    match chain.store.state_get("state_sync_status").as_deref() {
        Some(b"complete") => false,
        Some(b"pending") => true,
        // A cold joiner necessarily has genesis block 0, so `latest_epoch() ==
        // None` cannot identify it.  Treat a store containing only genesis and
        // no balances as unsynced; a verified snapshot then flips the durable
        // status to complete before sealing is enabled.
        _ => chain.store.latest_epoch().map(|e| e <= 0).unwrap_or(true)
            && chain.store.scan_balance_entries().is_empty(),
    }
}

/// Retry until two peers agree and the local import verifies.  Failure is
/// fail-closed: `ready` remains false, which prevents a cold joiner sealing.
pub async fn run(chain: Arc<Chain>, net: NetworkHandle, ready: Arc<AtomicBool>) {
    if !requires_catchup(&chain) { ready.store(true, Ordering::Release); return; }
    let _ = chain.store.state_set("state_sync_status", b"pending");
    // Test-only delay used by the isolated Pass-B harness to create a window
    // where gossip reaches the joiner while verified HTTP catch-up is still
    // pending.  The default is zero and production never sets this variable.
    if let Ok(seconds) = std::env::var("HONE_STATE_SYNC_TEST_DELAY_SECS")
        .ok().and_then(|s| s.parse::<u64>().ok()).ok_or(())
    {
        tokio::time::sleep(Duration::from_secs(seconds)).await;
    }
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
        if snapshot.epoch == 0 && snapshot.state_root != "0".repeat(64) {
            tracing::error!("[sync] non-zero snapshot has no durable reward checkpoint; refusing import");
            tokio::time::sleep(Duration::from_secs(10)).await; continue;
        }
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

/// Restore ALL prior categories from `old`, undoing a partial import.  Same
/// fail-closed shape as the pre-existing balance-only rollback, extended to the
/// clock-registration and alive-epoch state added by the snapshot-completeness
/// fix — a partially-imported joiner (balances updated, clock-registration or
/// alive state not yet verified) must not be left in a mixed state.
fn rollback_to(chain: &Chain, old: &AtomicSyncState) -> Result<()> {
    chain.store.replace_balance_entries(&old.balances).context("rollback balances")?;
    chain.store.replace_account_entries(&old.accounts).context("rollback accounts")?;
    chain.store.replace_meta_prefixed_entries(
        &["role_stake:clock:", "clock_reg:", "alive:"],
        &old.clock_registrations.iter().cloned()
            .chain(old.alive_epochs.iter().map(|(a, e)| (format!("alive:{}", a), e.to_le_bytes().to_vec())))
            .collect::<Vec<_>>(),
    ).context("rollback clock-registration/alive state")?;
    chain.store.replace_meta_prefixed_entries(&["layer_a:"], &old.emission_state)
        .context("rollback emission state")?;
    Ok(())
}

async fn import_verified(chain: &Chain, client: &reqwest::Client, peer_base: &str, snapshot: &Snapshot, entries: &[(String, String, u64)]) -> Result<()> {
    let old = chain.store.read_atomic_sync_state();

    chain.store.replace_balance_entries(entries).context("replace balances")?;
    if chain.store.balance_merkle_root() != snapshot.state_root {
        let _ = rollback_to(chain, &old);
        return Err(anyhow!("snapshot state_root recomputation mismatch"));
    }

    // Clock-registration state (order item 1): without this, a joiner's own
    // `registered_clock_nodes` view comes up short relative to the clock module's
    // live view, `epoch_meta.sealed_by` ends up empty for epochs it self-seals,
    // and the reward driver recycles the block reward instead of crediting
    // sealers — the snapshot-completeness bug this fix closes.
    chain.store.replace_account_entries(
        &snapshot.accounts.iter().map(|a| (a.id.clone(), a.state.clone())).collect::<Vec<_>>(),
    ).context("restore accounts")?;
    let mut clock_and_alive: Vec<(String, Vec<u8>)> = Vec::new();
    for reg in &snapshot.clock_registrations {
        let bytes = hex::decode(&reg.value_hex).context("decode clock-registration value")?;
        clock_and_alive.push((reg.key.clone(), bytes));
    }
    // Per-account alive-epoch map (order item 2): without this, `apply_entropy_decay`
    // (finalize.rs) reads `get_alive_epoch` as 0 for every imported account and
    // applies the wrong decay relative to peers that have the real history.
    for alive in &snapshot.alive_epochs {
        clock_and_alive.push((format!("alive:{}", alive.account), alive.epoch.to_le_bytes().to_vec()));
    }
    chain.store.replace_meta_prefixed_entries(&["role_stake:clock:", "clock_reg:", "alive:"], &clock_and_alive)
        .context("restore clock-registration/alive state")?;

    // Layer-A emission EMAs: running state the emission split reads from CF_META and
    // that no amount of balance data reconstructs.  Absent, `read_layer_a_ema` falls
    // back to LAYER_A_SCALAR_DENOM ("full health"), so the joiner scales the epoch
    // pool by 1.000 while the cohort is already damped.  Observed live in run 6: at
    // every credited epoch C ran 6 EMA ticks behind A/B — 99006 C f=9999 vs A f=9993,
    // adjusted 20000000000 vs 19996000000 — so C's state_root could never converge
    // even with sealed_by fully populated.  This is the same class as the
    // clock-registration and alive-epoch gaps above.
    let mut emission: Vec<(String, Vec<u8>)> = Vec::new();
    for kv in &snapshot.emission_state {
        let bytes = hex::decode(&kv.value_hex).context("decode emission-state value")?;
        emission.push((kv.key.clone(), bytes));
    }
    chain.store.replace_meta_prefixed_entries(&["layer_a:"], &emission)
        .context("restore emission state")?;

    // Trust-minimize (order item 3): the ≥2-peer agreement already covered this
    // digest (`agreed_snapshot`); recompute it locally over what was ACTUALLY
    // written and roll back on mismatch, same fail-closed shape as the
    // balance-root check above.
    let restored = chain.store.read_atomic_sync_state();
    let local_digest = consensus_digest(snapshot.epoch, &restored);
    if !snapshot.digest.is_empty() && local_digest != snapshot.digest {
        let _ = rollback_to(chain, &old);
        return Err(anyhow!("snapshot consensus-digest recomputation mismatch"));
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
    use super::{agreed_snapshot, contiguous_finalized, Snapshot};
    use std::collections::HashSet;

    fn snapshot(epoch: u64, state_root: &str) -> Snapshot {
        Snapshot {
            epoch, state_root: state_root.to_owned(), digest: String::new(),
            balances: Vec::new(), accounts: Vec::new(),
            clock_registrations: Vec::new(), alive_epochs: Vec::new(),
            emission_state: Vec::new(),
        }
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

    /// Regression: HONE's genesis is backdated, so a launch cohort finalizes epochs
    /// from a high launch epoch F onward and `epoch_finalized_done:1` never exists.
    /// The old walk anchored at 1, broke immediately, and labeled a real snapshot
    /// epoch 0 — which the importer refused, so no joiner could ever catch up.
    #[test]
    fn snapshot_epoch_anchors_at_first_finalized_epoch_for_backdated_genesis() {
        let done: HashSet<u64> = (95_460..=95_475).collect();
        assert_eq!(contiguous_finalized(&done, 0), 95_475);
    }

    #[test]
    fn snapshot_epoch_stops_at_the_first_gap_above_the_anchor() {
        let done: HashSet<u64> = [95_460, 95_461, 95_462, 95_470, 95_471].into_iter().collect();
        assert_eq!(contiguous_finalized(&done, 0), 95_462);
    }

    #[test]
    fn snapshot_epoch_handles_empty_and_single_finalized_sets() {
        assert_eq!(contiguous_finalized(&HashSet::new(), 0), 0);
        // Empty set falls back to the durable reward watermark.
        assert_eq!(contiguous_finalized(&HashSet::new(), 95_400), 95_400);
        let single: HashSet<u64> = [95_460].into_iter().collect();
        assert_eq!(contiguous_finalized(&single, 0), 95_460);
    }

    #[test]
    fn snapshot_epoch_never_regresses_below_the_reward_watermark() {
        // A late joiner imports a snapshot and persists its watermark without ever
        // applying those epochs locally, so `done` is empty or starts above them.
        let done: HashSet<u64> = (95_480..=95_482).collect();
        assert_eq!(contiguous_finalized(&done, 95_490), 95_490);
    }
}
