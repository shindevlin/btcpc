# Reputation-Weighted Quorum

Status: **DESIGN — not implemented.** Consensus change; gate before landing, Shin-in-person to merge.
Author intent captured from Shin, 2026-07-30.

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
w_i = stake_term(i) · uptime_term(i) · (1 − outlier_penalty(i))
```

All three inputs already exist on-chain — this composes them, it does not invent new tracking:

- **stake_term** — from `role_stake:clock:{node}` / `stake_weight` / `clock_min_stake`. A clock is
  bound to a **per-device stake** (§5). **Diminishing returns (§4):** `stake_term = sqrt(stake)` or
  `log1p(stake)`, NOT linear — so weight can't be bought 1:1, and a whale can't dominate by staking.
- **uptime_term** — an EMA of `seals / epochs_elapsed_while_registered` (the same signal that
  already scales clock reward, `CLOCK_UPTIME_MIN_EPOCHS`). A device that's up 40% of the time gets
  ~0.4× weight. This is the reputation core: presence measured over time.
- **outlier_penalty** — from the existing timestamp-outlier scoring (`clock.rs:573`, "non-voting
  outliers get scored down"). A clock that repeatedly submits out-of-consensus timestamps decays
  toward zero weight.

**Probation:** a freshly registered clock starts at `uptime_term ≈ 0` and low stake, so `w_i ≈ 0`.
It seals and its signatures are *recorded* (so it builds uptime), but it contributes ~nothing to
quorum. It earns weight only by proving reliable. This is what makes registering a flaky laptop
safe: it can't stall the chain (the established clocks carry the 51%) and it can't be a free rider.

Weights are **deterministic** — computed on every node from the same on-chain inputs at a fixed
epoch anchor, exactly like the reward derivation. They are NOT read from wall-clock or local
observation (that was the unsound-rule mistake).

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

Even with diminishing returns, cap a single clock's weight at some fraction of the total (e.g. no
clock may exceed `1/3` of `Σ w_registered`). Reliability is rewarded with influence, but no single
box can ever carry a majority alone. Belt-and-suspenders with §4.

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

## 10. Open questions for Shin / Beastly

- Exact `stake_term` curve (`sqrt` vs `log1p`) and the clock-reward diminishing-returns shape.
- The per-clock weight cap fraction (`1/3`? `1/4`?).
- uptime EMA window (fast enough to reward returning devices, slow enough that a brief outage
  doesn't tank a good clock).
- Whether weight also feeds the **reward split** (reputation → influence AND modest reward), or
  influence only. Leaning: reward tracks uptime as it already does; keep the two aligned but
  diminishing.

---
*Builds on existing primitives: `clock.rs` quorum + outlier scoring, `CLOCK_UPTIME_MIN_EPOCHS`,
`role_stake:clock` / `stake_weight`, `hardware.rs` device identity. No new tracking — this composes
signals the chain already records.*
