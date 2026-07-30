# Reputation-Weighted Quorum

Status: **v1 BUILT, gated OFF, NOT ACTIVATABLE AS-IS.** Branch `feat/reputation-weighted-quorum`.
A determinism blocker in the *uptime source* (§0 below) means the reputation core cannot go live
until a design decision from Shin. The pure weight math, the seal/reward wiring, the 1/3 cap, the
floor, and 11 property tests are all in and correct; the gap is where `uptime_term` reads from.
Consensus change; gate before landing, Shin-in-person to merge. Author intent captured from Shin,
2026-07-30.

## 0b. DEEPER FINDING (2026-07-31): per-device attendance is not agreed — and today's uptime discriminates nothing

Designing the seal-anchored uptime surfaced two facts that reframe the whole feature. Both are
verified against the tree, not inferred:

1. **`clock_uptime` measures registration, not attendance.** It is updated (main.rs:2237-2257) with
   `sealer_set = clock_sealers = the winning EpochFinalize's sealed_by = epoch_validators = the
   REGISTERED set` (clock.rs:845-848). So every staked, registered clock gets `seals++` every epoch:
   uptime sits at ~100% for everyone and distinguishes a rock-solid box from a flaky laptop by
   nothing. The uptime term Shin approved in (b) would not discriminate even if it were made
   deterministic. This is the load-bearing correction.

2. **The chain agrees on attendance only as a COUNT, never per-device — by design.** Every
   consensus-relevant per-epoch input is the registered-set snapshot `epoch_validators` (sorted,
   dedup'd, gossip-timing-independent). `compute_rewards_hash` (clock.rs:851) hashes it; rewards
   derive from it; `EpochFinalize.quorum` is a COUNT of agreeing clocks and is the fork-choice key
   (chain.rs:1128). Fix `e86a03730` (BUG 6) deliberately REMOVED all use of local per-node seal
   observations because they are gossip-timing-dependent and forked the chain. `epoch_node_seal`
   records exist but are local subsets, never an agreed per-device set. Committing per-device
   attendance would require the winning finalize to bind an agreed signer SET (not just a count) —
   a protocol change to fork choice + what finalize commits, i.e. re-opening exactly what BUG 6
   closed.

**RESOLVED (Shin, 2026-07-31): "build a reputational profile per device, real and measurable."**
Implemented on branch `feat/reputation-weighted-quorum`, gated OFF:
- **Source = the actual per-device seal.** `emit_epoch_rewards` (main.rs) now accrues
  `device_attend:{node}` = {sealed, epochs} from `epoch_node_seal:{epoch}:{node}` — the
  equivocation-guarded, never-pruned record of whether THAT device's own seal for the epoch landed
  on-chain. A registered device that goes dark accrues epochs but no seals → its attendance ratio
  falls → its `uptime_term` and thus its consensus weight sink. That is the "flaky sinks" signal
  clock_uptime (registration-fed, ~100% for all) never had. `compute_clock_weights` reads
  `device_attend`, not `clock_uptime`.
- **Determinism = the ordered reward walk at depth 20.** Attendance is consumed inside the BUG-6
  ordered contiguous finalized-epoch walk, which runs `REWARD_FINALITY_DEPTH = 20` (~10 min) behind
  the tip. By then the epoch's seals have flood-propagated to every node, so the accrual is
  replay-identical — the same "settled + propagated ahead" bar the reward derivation already trusts.
- **Gated.** The accrual writes NEW state, so it is fenced behind `chain_param:weighted_quorum`:
  off (default) → nothing written, live state_root untouched; on → all nodes begin accruing from the
  same activation height, windows fill, weighted quorum engages as probation clears.
- **Activation mechanism (confirmed).** `chain_param:weighted_quorum` holds the activation EPOCH
  (0/absent = off), set by a `ChainParameterSet` entry — tx-applied, 2-epoch timelocked, no key
  whitelist (chain.rs:1189), so it is agreed across nodes at a height. Accrual and the weighted
  decision gate on `epoch >= activation` (`weighted_quorum_active_for`), a pure function of that
  agreed value — never a bare "is it set now". The accrual set is the per-epoch
  `epoch_validators:{e}` snapshot (the reward path's own set, identical across nodes by BUG-6),
  NOT a live registration scan.
- **Operational precondition (must be in the activation runbook):** set the activation epoch
  comfortably in the future (≥ a few hundred epochs ahead) so every node has applied the
  `ChainParameterSet` before its reward walk reaches the activation height. An epoch processed
  before the param propagates writes nothing and is never revisited (write-once), leaving a
  per-node hole — choosing the height well ahead is the mitigation, not an assumption.
- **Known v1 limitation:** a device that UNSTAKES drops out of `epoch_validators`, so its
  attendance ratio freezes rather than decaying — unstaking-before-going-dark preserves reputation.
  Acceptable for v1 (unstaking forfeits the stake_term and quorum standing anyway); a decay-on-exit
  rule can come later if it matters.
- **The one gate question (not a fork, an empirical bound):** a *straggler* seal for the epoch
  arriving on one node after its walk passed that epoch would diverge `device_attend`. At depth 20
  this is not observed for honest nodes, but it is exactly what the cross-arch / cross-NAT gate must
  stress before activation. If it ever fires, the fix is a deeper depth or a chain-agreed
  completeness marker — a tuning of L, not a redesign. Tests: `real_attendance_makes_a_dark_device_sink`,
  `seal_anchored_attendance_credits_real_sealers_only_and_is_gated`, + the 11 weighted-quorum props.

**The fork that WAS on the table (now decided for real attendance, kept for the record):**
- **(A) Stake + registered-tenure reputation.** Weight from what IS agreed and per-device: stake
  (tx-applied) and continuous tenure in the registered set (derivable from `role_stake` staked_epoch
  + presence across `epoch_validators` snapshots). Deterministic at the tip TODAY, no protocol
  change. Weaker: it rewards staking-and-staying; it does NOT punish a registered device that goes
  dark (staying registered while offline keeps tenure). "The laptop earns in by staking and
  persisting," but "flaky sinks" is only partial.
- **(B) Make per-device attendance agreed.** A protocol change so the winning finalize commits an
  agreed per-epoch signer set (bound into the fork-choice key), then weight by real seal attendance.
  Delivers "flaky sinks" fully. Much larger change, re-touches the fork-choice + reward path just
  stabilized for the 42M reconciliation, and must clear the same determinism bar BUG 6 set.

The lagged-closed-window design in §0 below was the right shape for a determinism problem, but it
cannot manufacture a discriminating signal that the chain does not record. §0 stands only under (B).

## 0. (under option B) lagged closed-window uptime over tx-applied seal records

**The blocker (found in build review).** `clock_weight` read `clock_uptime:{node}`. That record is
written **only** inside `emit_epoch_rewards` (main.rs:2238/2264), driven by the **contiguous reward
walk** off `highest_contiguous_rewarded` (main.rs:691) — a cursor whose position is a function of
gossip arrival and lags `current_epoch()` per-node. Two nodes deciding the same epoch read uptime
as-of their own cursor → different weights → fork.

**The real discriminator (not "finalized vs live").** Flat quorum already reads *unfinalized-at-
decision-time* state — `role_stake:clock`, `clock_reg` — and never forks, because those are
**tx-applied and slow-moving**: every node converges on them from gossip well before the epoch they
gate, and they change only on rare registration entries. Determinism at a live decision needs inputs
that are *agreed and slow relative to the decision*, NOT inputs that are *finalized*. So:
- `role_stake:clock` → tx-applied, changes rarely → **safe** at a live seal decision (`stake_term`).
- `clock_uptime` → local-cursor-written, changes every epoch → **unsafe** (the blocker).
This is why weighting stays at the **seal** layer (who can seal — Shin's "the laptop might become one
of the three"); folding it into fork choice (chain.rs:1128) is a far larger blast radius than
approved and doesn't deliver that.

**The fix Shin chose (2026-07-30): make uptime tx-applied AND stale, via a lagged CLOSED window.**
Compute `uptime_term` for epoch `E` over a fixed window `[E − W − L, E − L − 1]` of already-tx-applied
per-epoch seal records, with lag `L` large enough that the window is settled on every node before
any of them decide `E`. The window is fixed by `E` alone → order-independent, no incremental
mutation, no per-node cursor. Source of truth = **`epoch_node_seal:{e}:{node}`**, written when each
`EpochSeal` applies (chain.rs:1076) — tx-applied, one idempotent key per node-epoch (NOT `sealed_by`,
which is the registered-set snapshot from `epoch_validators`, not the actual signers). Numerator =
epochs in-window where the node has a seal record; denominator = window length. `stake_term` already
qualifies as-is.

**Two design points to close with Beastly before wiring (they decide if the window is truly
order-independent):**
1. **Window completeness.** A node can be missing `epoch_node_seal` for a window epoch while sealing
   a later one (gossip delivers seal subsets at different times). The decision must require a
   COMPLETE window and fall back to flat otherwise — and that fallback predicate must itself be
   deterministic across nodes, or the fallback forks. Candidate: gate on a chain-agreed
   completeness marker, not "what this node happens to hold."
2. **Choosing L.** Large enough to dominate real seal-gossip delay, small enough that reputation
   stays responsive. Bound it against observed delivery lag on the gate cohort.

At activation all clocks start with empty windows → `uptime_term ≈ 0` → probation → flat, then
weighted engages smoothly as windows fill (§3 probation, for free). Gate the whole thing behind the
chain param so the live chain's state_root is untouched until coordinated, Shin-in-person activation.

Everything below is the design as intended; §0 is what stands between it and activation.
The stake-only narrowing (deterministic today, but loses the reputation core) was considered and
rejected as a partial.

## 1. Why

Today the clock quorum is a **flat count**: an epoch seals when `> 51%` of the *registered clock
count* have signed (`MIN_QUORUM_FRACTION = 0.51`, `registered_clocks` as the denominator,
`clock.rs`). That has two problems Shin wants gone:

1. **Even clock counts are strictly worse than the odd below.** Fault tolerance only improves at
   odd counts (3→tolerate 1, 4→still 1 but need one more up, 5→tolerate 2). A 4th equal-weight
   clock raises the bar without buying resilience, and you can't just lower quorum on 4 clocks —
   `2-of-4` lets two disjoint pairs each seal, i.e. a fork.
2. **It can't express device reputation.** Every registered clock is one equal vote, whether it's
   a rock-solid always-on box or a laptop that's up half the time. There's no way for a device to
   *earn* consensus standing by being reliable, or lose it by being flaky.

The goal: **register any device as a clock — reliable or flaky — and let measured reputation be its
consensus weight.** A new/intermittent device joins at near-zero weight, seals alongside, and
*builds* reputation; the established clocks carry quorum on their own until the newcomer has earned
it. The "3 clocks" stops being an identity list and becomes a living, meritocratic set that rotates
as reliability changes. This dissolves the odd/even problem: weight, not count, decides.

## 2. Core rule

Replace the count-majority with a **weight-majority**.

Each registered clock `i` has a reputation weight `w_i ≥ 0`. An epoch seals when the clocks that
signed a given `rewards_hash` have combined weight:

```
Σ w_signed  >  0.51 · Σ w_registered
```

- **Fork-safe.** Two disjoint sets each exceeding 51% of total weight is impossible (they'd sum to
  >100%). Same safety property as count-majority — no split-brain.
- **`HONE_CLOCK_QUORUM` floor stays.** Keep the absolute `max(…, 2)` floor so a two-clock genesis
  cohort still needs both — weight can't drop effective quorum below the safety floor.

## 3. The weight function

```
w_i = stake_term(i) · uptime_term(i)          [v1 — TWO inputs, both genuinely on-chain]
```

**Corrected per Beastly's source review (940a6577).** The earlier draft added a third factor,
`(1 − outlier_penalty)`, sourced from `clock_scores`. That is **in-memory only** (`clock.rs:116`,
rebuilt from zero each process start), **wall-clock contaminated** (`SystemTime::now()`,
`clock.rs:783`), and **locally observed** — three independent fork sources. Using it for quorum
weight is the unsound-rule mistake in a worse place: it changes *who can seal*. **Dropped from v1.**
The outlier signal is already reflected in uptime — a clock submitting bad timestamps doesn't make
the winning seal set (`clock.rs:563-566`), so it fails to accrue `seals` and its uptime term decays
on its own. An explicit outlier term, if ever wanted, must first become real chain state (an on-chain
counter written through `apply_entry` in the epoch-finalize path, replay-derived) — its own change,
not smuggled into this one.

Both remaining inputs are genuinely on-chain and integer:

- **stake_term** — from `role_stake:clock:{node}` / `stake_weight` / `clock_min_stake` (per-device
  stake, §5). Diminishing returns (§4) via **integer `isqrt`**, NOT `sqrt`/`log1p`: floating-point
  `sqrt` disagrees by one ULP across x86_64 (Beastly) and aarch64 (Nebra), and with 3 clocks a single
  divergent weight flips a 51% boundary → fork. `stake_term = isqrt(stake / GRANULE)` in u128, a
  fixed granule, bit-exact on every arch. Follow the existing integer-millipct pattern
  (`main.rs:1971-1976`, `500 + 500 * seals / epochs`, u128 intermediate).
- **uptime_term** — `seals * 1000 / epochs` from the ON-CHAIN `clock_uptime:{node}` record
  (`{seals, epochs}`, state_get/state_set, `main.rs:2209`), over the existing `CLOCK_UPTIME_WINDOW =
  100` sliding window. Integer, deterministic, no new EMA window (a second window is a second thing
  to diverge on). Up 40% → ~0.4× weight. This is the reputation core: presence measured over time.

**Probation:** a freshly registered clock starts at `uptime_term ≈ 0` and low stake, so `w_i ≈ 0`.
It seals and its signatures are *recorded* (so it builds uptime), but it contributes ~nothing to
quorum. It earns weight only by proving reliable. This is what makes registering a flaky laptop
safe: it can't stall the chain (the established clocks carry the 51%) and it can't be a free rider.

Weights are **deterministic** — integer-only, computed on every node from the same on-chain inputs
(`role_stake:clock`, `clock_uptime`) at a fixed epoch anchor, exactly like the reward derivation. NO
wall-clock, NO local observation, NO floats in the sum or comparison path (the unsound-rule / one-ULP
mistakes). Enforced by the restart-invariance assertion in §9.

## 4. Economics: clocks are NOT the primary earning method

Shin, explicit: "clocks are not designed to be primary earning method on this chain." Earning comes
from **work** — sensors, inference, storage, verification, the nine work pools. Sealing is a
low-reward *reliability* role, not an income.

- **Diminishing returns on clock reward.** Per-clock reward already carves a small slice of the
  epoch budget; keep it small and make each clock's share sub-linear in its weight, so there is **no
  incentive to hoard clocks**. You run one reliable clock for the modest reward + consensus
  standing, and you make your living doing work.
- This decoupling is the anti-centralization lever most chains lack (where validation *is* the
  income, so the biggest validator earns most → stakes most → validates more). Breaking that loop at
  the source is cleaner than only bolting on a cap — though we keep the cap too (§6).

## 5. Anti-sybil: per-device stake, hardware-bound

Reputation must be anchored to something costly, or fake high-reputation clocks are free.

- Each clock stakes, and the stake/identity is bound to the device's **hardware fingerprint**
  (`hardware.rs`: `machine_id`, `SHA-256(gpu_serial | machine_id)`, "one physical machine = one
  account"). One physical device = one clock identity = one stake.
- You cannot mint high-weight clocks without staking real value on real hardware each time. Weight
  is earned per device, over time, at cost.

## 6. Anti-concentration backstop

Cap a single clock's weight at `1/3` of the total. Reliability is rewarded with influence, but no
single box can ever carry a majority alone. Belt-and-suspenders with §4.

**Cap against the PRE-CAP total, and do NOT renormalize** (per Beastly's review). If you cap `w_i`
at `floor(total/3)` and don't renormalize, `Σ w_registered` shrinks slightly — that's fine and
intended. Renormalizing (cap → recompute total → re-cap, since capping one clock can push another
over) requires a deterministic fixed-point iteration with a bounded iteration count; unbounded
"iterate to convergence" in the seal path is a liveness hazard. Cap-against-pre-cap-total is simpler,
monotone, and integer. With `1/3` and 3+ clocks the §2 floor (`max(…,2)`) is the real protection.

## 7. Living, rotating set — "the laptop might become one of the three"

There is no fixed top-N to be promoted into and no discrete rotation event to flap on. Every
registered clock contributes weight continuously; a device "becomes one of the three" simply when
its weight rises past an incumbent's. Reliable → climbs; degrades (offline, outliers) → sinks.
Automatic, every epoch.

**Weight-all, not a capped active set.** Let *all* registered clocks seal and weight their votes.
Do NOT cap the sealing set to a top-N — a hard boundary reintroduces churn (a device flapping across
the Nth position reshuffles the set) and needs hysteresis to tame. Continuous weighting is
self-stabilizing and simpler. (If latency ever forces a capped active set, add hysteresis: a
challenger must *sustain* higher weight for K epochs before displacing an incumbent.)

## 8. Migration from the current 2-of-2 → 2-of-3

- Beastly registers as clock #3 under the *current flat* quorum first (odd, quorum stays 2 — safe,
  already directed). Weighted quorum is not required for that step.
- Ship weighted quorum as its own change. On activation, grouchly + nebra + beastly have high
  uptime/stake → high weight; the 51%-of-weight rule reduces to "the established clocks seal,"
  behaviourally identical to 2-of-3 on day one. No discontinuity.
- Then any further device (the laptop, a phone-as-clock later) registers at ~0 weight and earns up,
  with no quorum-count liability.

## 9. Safety / liveness to assert in the gate

Gate exactly like the other consensus changes tonight — **asymmetric cohort + control arm**:

- **No split-brain:** no two disjoint signed sets can each exceed 51% of registered weight. Prove
  by construction and by test.
- **No stall from a low-weight drop:** with 3 high-weight clocks + 1 near-zero-weight laptop, kill
  the laptop → chain still seals (established clocks hold >51% weight). This is the whole point.
- **Determinism:** every node computes identical `w_i` for epoch `e` from on-chain inputs — no
  wall-clock, no local-only signal (the unsound-rule trap). Assert byte-identical weights across
  the cohort.
- **Floor honored:** effective quorum never drops below `HONE_CLOCK_QUORUM` (2) regardless of
  weights.
- **Probation:** a freshly registered clock cannot influence quorum until it has accrued uptime;
  assert a day-one clock with `w≈0` neither stalls nor swings a seal.
- **Restart-invariance (added per Beastly):** a clock's computed weight for epoch `e` must be
  IDENTICAL before and after a process restart with no intervening chain activity. Snapshot `w_i`
  for all `i`, restart a cohort member, recompute at the same epoch anchor, assert unchanged. This
  is the assertion that would have caught the `clock_scores` blocker, and it generalizes — it catches
  ANY accidental local-state or wall-clock dependency, including ones added later. **This is the key
  assertion of the whole change.**

### 9.1 Implementation of the anchor (why purity alone is not enough)

The weight *function* being pure is necessary but NOT sufficient — the system must also read the
same *inputs* on every node and after a restart. A mutable "latest weights" field fails this: a
node whose per-epoch handler lagged (GC, disk stall, or a fresh restart) would resolve epoch `E`
against a *different* anchor than its peers and fork — the §3-class bug moved from *what* is read
to *when*. So weights are **epoch-anchored**:

- The decision for epoch `E` uses `epoch_weights[E]` only, derived from committed-state-through-`E−1`
  — never a shared latest. Injected when `E−1` seals (`set_clock_weights(E, …)` from main.rs) and
  **re-seeded at startup** for the pending epoch, so a restart under an active gate never runs one
  epoch on the flat rule while peers run weighted.
- `epoch_clock_weights:{E}` is persisted per epoch (mirroring `epoch_validators:{E}`) so the cohort
  can **byte-diff** each epoch's weight vector across x86_64/aarch64 during the gate — turning
  "determinism" from an assertion into an artifact.
- **Engagement is logged** (winner weight / total, signer count) at each weighted seal, so a green
  gate run PROVES weighted quorum actually engaged rather than silently falling back to flat on
  empty/zero weight — a silent fallback would make a passing gate indistinguishable from a no-op.

Implemented in `feat/reputation-weighted-quorum` (`clock.rs` pure fns + `Inner.epoch_weights`,
`main.rs` inject/seed). 11 property tests including a 100k `isqrt` sweep, no-split-brain,
no-stall-on-low-weight-drop, floor, 1/3 cap, and the store-read gate path. Ships gated OFF.

Gate host: **Beastly (x86_64) against Nebra (aarch64)** — the right pair to catch the float/one-ULP
determinism class, since that's where a cross-arch weight disagreement would surface. Beastly offered.

## 10. Answers (resolved with Beastly, 940a6577)

- **stake_term curve:** integer **`isqrt(stake / GRANULE)`** in u128. NOT `sqrt`/`log1p` — floats fork
  cross-arch, and `log1p` is worse to make exact in integers for no benefit at these magnitudes.
- **Weight cap:** `1/3`, against the **pre-cap** total, **no renormalization** (§6).
- **uptime window:** reuse the existing on-chain `CLOCK_UPTIME_WINDOW = 100`; do NOT add a second EMA
  window (a second thing to diverge on).
- **Weight → reward split:** keep influence and reward **SEPARATE in v1.** Reward already tracks
  uptime (`main.rs:1971-1976`). Coupling them makes a weight bug a *supply* bug — and tonight's 42M
  reconciliation is too recent to take that on. Original open question, now closed:
  influence only. Leaning: reward tracks uptime as it already does; keep the two aligned but
  diminishing.

---
*Builds on existing primitives: `clock.rs` quorum + outlier scoring, `CLOCK_UPTIME_MIN_EPOCHS`,
`role_stake:clock` / `stake_weight`, `hardware.rs` device identity. No new tracking — this composes
signals the chain already records.*
