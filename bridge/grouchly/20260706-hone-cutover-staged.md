# Grouchly: HoneMesh Cutover — Staged, Awaiting Authorization

**Date**: 2026-07-06  
**Status**: All pre-flight checks passed. Waiting for Shin's go signal.

---

## What's Staged

### Binaries (both built clean)
- x86_64: `rust/target/release/honemesh-node` — 69.4M ✓  
- aarch64: `rust/target/aarch64-unknown-linux-gnu/release/honemesh-node` — 64.6M ✓

### Service file
`~/.config/systemd/user/btcpc-node.service.new` — ready to replace the active service.  
Key changes from current:
- `ExecStart` → `/home/ubuntclaw/.local/bin/honemesh-node`
- All `BTCPC_*` env vars → `HONE_*`
- `HONE_CHAIN_ID=hone`
- `HONE_DATA_DIR=/mnt/btcpc-storage/btcpc/node-db-hone` (fresh dir, clean chain)
- `HONE_GENESIS_FILE` → `rust/hone-node/genesis.json`
- `SyslogIdentifier=hone-node`

### Env file
`~/.hone.env.new` (chmod 600) — `HONE_POSTING_KEY` copied from `.btcpc-v2.env`.

---

## Pre-flight Verification

| Check | Result |
|-------|--------|
| natoshisakamoto key → genesis pubkey | ✓ MATCH |
| x86_64 binary runs (port conflict expected) | ✓ |
| aarch64 binary is valid ARM ELF | ✓ |
| No cutover directive in bridge/comm channel | ✓ (awaiting signal) |

---

## Build Fixes Applied (need commit)

The rebrand commits had mismatched directory paths vs package names:

1. `rust/Cargo.toml` — workspace members: `honemesh-*` → `hone-*`, added `hone-contract-runtime`
2. `rust/hone-node/Cargo.toml` — dep paths: `honemesh-node/crates/honemesh-types` → `hone-node/crates/hone-types`, same for contract-runtime
3. `rust/hone-p2p/Cargo.toml` — same honemesh-types path fix
4. `rust/hone-market/Cargo.toml` — same honemesh-types path fix
5. `rust/hone-node/src/api.rs` — `include_str!("../../../honemesh-manifest.json")` → `hone-manifest.json`

---

## Cutover Steps (on go signal)

**Grouchly:**
1. `cp btcpc-node.service.new btcpc-node.service && cp .hone.env.new .hone.env`
2. `cp rust/target/release/honemesh-node ~/.local/bin/honemesh-node`
3. `systemctl --user daemon-reload`
4. `systemctl --user stop btcpc-node`
5. `mkdir -p /mnt/btcpc-storage/btcpc/node-db-hone`
6. `systemctl --user start btcpc-node`
7. Verify `curl localhost:4242/api/node/info` → `chain_id: "hone"`

**Nebra (Pi, aarch64):**
1. `scp -o ConnectTimeout=300 aarch64 binary → /tmp/honemesh-node-new`
2. SSH: stop btcpc-node, wipe `/home/pi/.btcpc-v2`, update service BTCPC_* → HONE_*, start
3. Verify block-0 hash matches Grouchly

**Fork check:**
```
node scripts/fork-check.mjs --epoch 0
```
