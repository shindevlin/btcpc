# BTCPC Roadmap

Roadmap policy:
- This roadmap must be updated whenever scope or implementation meaningfully changes.
- Version roadmap updates alongside the code they describe, so the public plan and the repo stay in sync.
- Treat roadmap entries as living release commitments, not static notes.

## Phase 0 — Genesis (current)

- [x] Sovereign chain with 42M supply, 10 decimal precision
- [x] BIP-39 mnemonic wallets with multi-chain derivation (7 chains)
- [x] Proof of Useful Work mining built around real inference, storage, sensors, service hosting, and clock timing
- [x] P2P inference routing (async submit/poll)
- [x] Dynamic pricing (tokens × verified param count)
- [x] Verified reward splitting by actual parameter count
- [x] Storage and sensor reward rails as first-class work inputs, not side features
- [x] RAG — context documents with inference requests
- [x] MCP — user-specified tool servers, saved to profile
- [x] Telegram bots (thin HTTP clients via bot API)
- [x] Posting key signature verification for identity linking
- [x] Project registration, transfer, billing
- [x] Cross-chain wallet derivation on registration
- [x] Whitepaper v0.4 with consensus, RAG, MCP, MPC design
- [x] Genesis dreams (soulbound NFTs per block)
- [x] Shared epochs — multiple miners submit to same epoch, never double rewards
- [x] Finalization delay (60s) to wait for all miners before splitting

## Phase 0.1 — Chain Truth & P2P Hardening

- [ ] Canonicalize chain truth to on-chain block height and finalized state only
- [ ] Remove localhost from every chain-truth P2P path, seed list, and advertised peer identity
- [ ] Make clock nodes derive height from on-chain P2P agreement and reject one-node truth
- [ ] Persist node P2P addresses in ledger state so live nodes publish connectable endpoints
- [ ] Keep storage data segregated and encrypted on storage nodes, invisible to normal users
- [ ] Tighten health checks so they go green only when the network is actually truth-bearing
- [ ] Add regression tests for epoch/current, node list publication, localhost rejection, and two-node truth
- [ ] Implementation plan: [docs/CHAIN_TRUTH_IMPLEMENTATION_PLAN.md](CHAIN_TRUTH_IMPLEMENTATION_PLAN.md)

## Phase 0.2 — Start-First UX, Legal, and Controller Onboarding

- [ ] Canonicalize `/start` as the public first-stop route for humans and agents
- [ ] Turn `/start` into a single-step wizard with persistent progress and a machine-readable manifest
- [ ] Turn the install flow into one guided path instead of many disconnected choices
- [ ] Add Terms and Privacy pages and link them from the homepage, install page, controller page, and app shell
- [ ] Make controller mode turnkey inside the existing BTCPC web and desktop surfaces
- [ ] Make mobile controller approval QR/deeplink friendly and one-tap where possible
- [ ] Add versioned public notes so the site, README, roadmap, and whitepaper stay aligned
- [ ] Implementation plan: [docs/START_FIRST_ROLLOUT_PLAN.md](START_FIRST_ROLLOUT_PLAN.md)

## Pending (user-requested, not yet scheduled)

- [ ] Wallet bot alerts only for users whose miner earned tokens (not broadcast to all)
- [ ] OpenClaw feature parity audit — identify missing features
- [ ] Inference cron jobs routed through real blockchain (not localhost)
- [ ] Auto-updater: stage updates, notify miner, apply on restart (not auto-apply)
- [ ] Systemd services for bots (auto-restart, no zombies)
- [ ] Cloudflare webhook mode for bots (eliminate polling entirely)
- [ ] Scrub git history to remove leaked .env / bot tokens
- [ ] Hardware claim registry — bind device hardware hashes to posting keys, log paid takeovers in USDC/USDT/DAI, and add chain-level revoke/bad-actor events
- [ ] Public testnet surface — replace localhost-first docs with `https://btcpc.net/testnet` and keep chain truth off local fallback paths
- [ ] `BTCPCTEST` separate testnet chain — mint native testnet rewards for public testnet nodes, mirror the mainnet-style role allotment in BTCPCTEST, keep public testnet report-only by default, and add a small BTCPC side bonus without polluting mainnet economics

## Phase 1 — Multi-Miner

- [ ] 3-miner consensus per model (N=3 when 3+ miners serve a model)
- [ ] Work proof mempool — gossip proofs for block validation
- [ ] Variable block size (1 to thousands of proofs per epoch)
- [ ] Cross-chain signatures — accept ETH/Solana/TON keys as BTCPC auth
- [ ] Expand proof-of-work sources so storage, sensors, and hosting continue to scale as core work pools
- [ ] Cascading bid system (escalate when initial bid rejected)
- [ ] Model auto-download on demand broadcast
- [ ] Streaming inference (/v1/chat/completions SSE)

## Phase 2 — Token Launch + Cross-Chain

- [ ] Cross-chain wallet watcher — monitor linked addresses on all 7 chains
- [ ] Proof of life: detect signed transactions from BTCPC-linked wallets
- [ ] Cross-chain reputation scoring (active wallets = higher trust)
- [ ] wBTCPC ERC-20 deployment on Base
- [ ] Bridge narrative keeps BTCPC work sources front and center: inference, storage, sensors, hosting
- [ ] Bridge signer (shindevlin → multisig transition)
- [ ] Bridge transaction detection via chain watcher
- [ ] wBTCPC/USDC liquidity pool on Base DEX
- [ ] Solana wBTCPC SPL token + pool
- [ ] TON claim manager
- [ ] Purchase Hive account "shindevlin"
- [ ] npm publish @btcpc/sdk

## Phase 3 — Privacy

- [ ] MPC sharded inference (N miners, no single miner sees full data)
- [ ] Premium pricing tier (N× standard rate)
- [ ] Proof of Silicon — hardware attestation for miner verification
- [ ] Encrypted inference with client-side decryption
- [ ] Private authorization stack — BTCPC spends approved by Bitcoin, Lightning, zkVM, or other supported chains
  - Implementation plan: [docs/PRIVATE_AUTH_IMPLEMENTATION_PLAN.md](PRIVATE_AUTH_IMPLEMENTATION_PLAN.md)
  - Future notes: [docs/PRIVATE_AUTH_FUTURE.md](PRIVATE_AUTH_FUTURE.md)
- [ ] BTCPC-native ZK verifier backend for private authorization receipts

## Phase 4 — Maturity

- [ ] Explorer / dashboard UI
- [ ] Light client proofs for cross-chain verification
- [ ] Multisig bridge (replace single signer)
- [ ] Governance — token-weighted voting on protocol parameters
- [ ] E2E integration tests
- [ ] Third-party miner onboarding documentation
