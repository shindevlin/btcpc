# Finality Announcement State Root v2

**Status:** branch design for `fix/finality-real-state-root`.

## Problem

The v1 external finality announcement signs the `state_root` field from the
finality snapshot. That snapshot has been a proxy value:

```text
sha256(latest_block_hash || epoch_le_bytes)
```

It is deterministic, but it is not a commitment to account balances. Two nodes
can diverge in balances while holding the same latest block hash and epoch, then
emit the same external finality announcement hash.

## New Snapshot Source

Starting at the coordinated cut-over epoch, `finalize_epoch` writes:

```text
state_root = store.balance_merkle_root()
```

This is the same balance Merkle root used by the node's state-facing APIs and by
the quorum-driven `EpochFinalize` entry path. `CrossChainFinalityModule` already
builds `announcement_hash` from the snapshot `state_root`, so no external
announcement can remain identical across divergent balance state once the
snapshot is populated from the real root.

## Flag-Day Transition

This is an outward-facing commitment change. Operators must treat the first epoch
that uses the real balance root as an announcement schema transition:

- Pre-cut-over announcements are v1 semantics, even though the JSON field name is
  `state_root`.
- Cut-over and later announcements are v2 semantics: `state_root` commits to all
  balances via `balance_merkle_root()`.
- External-chain publishers and verifiers must not mix v1 and v2 semantics for
  the same epoch. Pick one coordinated cut-over epoch, publish that epoch in the
  release notes / operator runbook, and reject duplicate announcements for the
  same `(chain_id, finality_epoch)` that use the other semantics.
- Do not inscribe or publish the v2 format to any external chain until Shin gives
  the in-person outward-facing gate.

## Compatibility

No genesis, clock registration, HONE_CLOCK, or mainnet flip is changed by this
branch. The local JSON shape remains compatible for internal readers, but the
meaning of `state_root` changes at the flag day and must be versioned by the
operator cut-over epoch.
