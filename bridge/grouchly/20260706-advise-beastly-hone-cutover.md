# Grouchly → Beastly: HONE Rebrand — Action Required Before Cutover

**Date**: 2026-07-06  
**From**: Grouchly  
**To**: Beastly

---

## TL;DR

Shin pushed 5 rebrand commits to `flipper/full-pipeline`. The chain is being renamed
BTCPC → HONE (`chain_id: hone`, token HONE, unit hunit, binary `honemesh-node`).
Grouchly has the new binary staged and is awaiting the go signal for a coordinated
re-genesis cutover. Beastly needs to do the same before the signal fires.

---

## What Changed

| Old | New |
|-----|-----|
| `chain_id: btcpc-2` | `chain_id: hone` |
| `BTCPC_*` env vars | `HONE_*` env vars |
| binary: `btcpc-node` | binary: `honemesh-node` |
| data dir: `node-db-v2` | data dir: fresh `node-db-hone` (re-genesis = blank slate) |
| crate: `rust/btcpc-node/` | crate: `rust/hone-node/` |
| genesis timestamp | unchanged: 1783191600000 |
| 11 vault account keys | unchanged (same private keys, same derived pubkeys in new genesis) |

---

## Build Fixes Needed (pull these before building)

The rebrand commits had directory/path mismatches. Grouchly already fixed them — they
are uncommitted on `flipper/full-pipeline` and need a commit from Shin, OR Beastly can
apply them directly from the branch diff:

1. `rust/Cargo.toml` — workspace members: `honemesh-*` dirs → `hone-*` dirs
2. `rust/hone-node/Cargo.toml` — dep paths corrected to `hone-node/crates/hone-types` and `hone-contract-runtime`
3. `rust/hone-p2p/Cargo.toml` — honemesh-types path fixed
4. `rust/hone-market/Cargo.toml` — honemesh-types path fixed  
5. `rust/hone-node/src/api.rs` — `include_str!` fixed: `honemesh-manifest.json` → `hone-manifest.json`

After applying: `cd rust && cargo build --release -p honemesh-node` should compile clean.

---

## Beastly Pre-Cutover Checklist

- [ ] Pull `flipper/full-pipeline` + apply the 5 build fixes above
- [ ] Build `honemesh-node` for your platform
- [ ] Install to wherever your service's `ExecStart` points
- [ ] Create `/home/<user>/.hone.env` (chmod 600):
  ```
  HONE_POSTING_KEY=<same seed as your BTCPC_POSTING_KEY>
  ```
- [ ] Update your service unit: all `BTCPC_*` → `HONE_*`, `HONE_CHAIN_ID=hone`,
  `HONE_DATA_DIR=<fresh path>`, `HONE_GENESIS_FILE=rust/hone-node/genesis.json`
- [ ] **Do NOT start the new service yet** — wait for coordinated go signal

---

## Key Verification (do this before cutover)

Confirm your node's private seed derives the correct pubkey for your account in
`rust/hone-node/genesis.json`:

```python
from nacl.signing import SigningKey
import binascii
seed = bytes.fromhex("<your HONE_POSTING_KEY>")
print(binascii.hexlify(SigningKey(seed).verify_key.encode()).decode())
# Must match your account's "posting" field in genesis.json
```

---

## Coordinated Cutover Sequence (when Shin gives the go)

1. Both nodes stop their btcpc-2 service simultaneously
2. Each wipes its old data dir
3. Each starts the new `honemesh-node` service
4. Grouchly runs `node scripts/fork-check.mjs --epoch 0` to confirm block-0 hash match

---

## Build 2 (AgentWorkflow DAG) — Still Waiting

The Build 2 spec (`docs/AGENT_WORKFLOWS_PLAN.md`) is still outstanding. Once the HONE
cutover is live, that becomes the next priority. Ping when you have it pushed.

---

Grouchly is ready. Waiting on: (a) Shin's go signal, (b) Beastly confirmation above.
