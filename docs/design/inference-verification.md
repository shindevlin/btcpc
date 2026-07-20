# HONE — Inference Verification & Engine-Ownership Design

**Status:** draft v0.1 — living doc. Branch `feat/candle-local-infer`.
**Scope boundary:** design only. No consensus/clock code, no genesis/chain-id/launch/gate is touched by this note.
**Last verified against source:** 2026-07-20 (`shindevlin/hone`, `rust/hone-node`).

---

## 1. Framing: the clock is consensus, inference is earning

HONE consensus is **proof-of-clock**. Inference is **not** part of consensus — it is a
marketplace where operators earn by running jobs. This single fact removes the hardest
problem other on-chain-inference designs impale themselves on:

- Inference does **not** need to be bit-identical across machines, because it never orders
  blocks. Cross-hardware floating-point nondeterminism is therefore a non-issue for
  consensus.
- "Make inference deterministic so the chain can agree on it" is the **wrong requirement**.
  The right requirement is: gate *what is allowed to earn*, and make earned work
  *verifiable* and *cheating unprofitable*.

An external model that called strong gating "impossible" was answering the consensus
question. We are not asking it.

## 2. Threat model (the real constraint)

The engine runs on the **earner's** hardware, which the earner fully controls. The critical
distinction:

> A hash of the binary proves **the copy at rest is genuine**. It does **not** prove **the
> genuine copy is what executed and produced the submitted result**. The reward is for the
> second claim.

Why a self-reported hash cannot close the gap:

- The chain authenticates the **signing key**, not the **process**. A submission is signed
  bytes on a socket; nothing in it proves which binary emitted it.
- The signing key lives on the operator's machine and is extractable by them. They can sign
  an identical, valid claim from *any* code — patched engine, script, raw `curl`. The honest
  wrapper's output and a hand-forged claim are bit-for-bit indistinguishable to the chain.
- A self-embedded "I am hash X" field is just a field the operator can set to X regardless of
  what ran.

There are **exactly two** ways to bind code-identity to a submission on untrusted hardware:
1. **Hardware-sealed key (TEE)** — a chip signs "measured code X produced this" with a key the
   operator cannot extract.
2. **Re-execution** — don't trust the submission; redo the work and compare.

Everything else is **economic deterrence** (make cheating negative-EV), not proof.

## 3. Design principles

- **Own the software to defeat the many; use economics to defeat the few.** Ownership stops
  casual cheating outright and lets us bake in seed-recording, canaries, deterministic-replay
  mode, and attestation hooks. It does not stop a sophisticated operator on their own
  hardware — that tail is what the verification economy is for.
- **AI is not deterministic, and we do not make it so.** Forcing greedy/temperature-0 decoding
  would destroy output value. Instead: full sampling (temperature, top-p) for value, plus a
  **recorded seed** so an honest run is reproducible *on demand*. Determinism-on-demand is not
  a deterministic product.
- **Verify by similarity, not equality.** The recorded seed makes "similar" a sharp test
  rather than a fuzzy one.

## 4. Engine ownership (fork vs pin)

- **candle → full fork (`hone-candle`).** Cheap, in-language (already vendored into
  `hone-node`), a fork is a pinned commit + patch set. Instrument: seeded-replay mode, seed
  recording, attestation hook, stripped nondeterminism sources. Clear yes.
- **llama.cpp → pin + wrap, don't fork the tree (yet).** It is a large, fast-moving C++/ggml
  codebase; forking it all buys a rebase treadmill. Instead: pin an exact upstream commit (its
  hash goes in the manifest), build it reproducibly, and own a thin `hone-infer` wrapper — the
  wrapper is the part that actually gets gamed. Fork the engine itself only if upstream won't
  take a change we need.
- **Reproducible builds are a prerequisite, not a nicety.** The manifest's power is "this exact
  binary hash." A non-deterministic build (unpinned toolchain, embedded timestamps) makes the
  hash drift and the allowlist meaningless. Pin toolchain + lock deps first.
- **"Ours" = we govern the canonical build + manifest, not that it is secret.** A patched clone
  works the same whether or not the source is public; security lives in the verification
  economy, not in obscurity (secret *challenge nonces* / watermarks help only at the margin).

### Manifest (the spec that defines "correct")

Signed allowlist keyed by `{engine_id, semver, binary_hash, weights_hash, quant}`. Additive,
Shin-signed, genesis-pinnable. A reward claim carries the manifest hash it ran under; unlisted
or mismatched → rejected at the gate, no payout. Version bumps are additive signed entries via
the in-person gate; old entries stay valid so nodes don't desync mid-epoch. llama.cpp is simply
another entry once its build is signed.

## 5. Verification stack

1. **Manifest = spec.** Defines the exact engine+model that "correct" means.
2. **Seeded reproduction.** Operator records the sampling seed with the job. An honest re-run
   under the same seed on the manifest engine lands very close to the original. This is what
   gives the similarity test teeth — without a shared seed, two honest sampled runs legitimately
   diverge and "similar" loses meaning.
3. **Random-denier re-execution.** A randomly selected challenger ("denier") re-runs the job on
   the manifest engine with the recorded seed and compares to the claim. Optimistic: rewards sit
   in escrow for a challenge window measured by the **clock** (proof-of-clock gives a trustworthy,
   ungameable timer). Only challenged jobs get re-run → cheap. High-value jobs upgrade to N-of-M
   redundant quorum up front.
4. **Similarity judged by a staked jury.** Comparison is by **semantic similarity**, not
   bit-equality. Multiple nodes score similarity; quorum/median decides pass/fail. The similarity
   metric is a **manifest-pinned small scorer** (embedding/equivalence model — itself a fixed,
   cheap inference) for the mechanical part, with the staked jury resolving the gray zone.
   Tolerance threshold is **calibrated from measured variance**, not guessed (see §6).
5. **Canaries.** The chain silently injects jobs whose correct answer it already knows, at a
   random rate. A wrong answer on a canary = instant slash. Cheap, and it puts risk on *every*
   job because the operator can't tell which is the trap.
6. **Stake + slash + reputation.** Operators bond stake > the gain from cheating; audit
   probability is set so E[cheat] < 0 even without proving every job. Reputation from passed
   challenges/canaries lowers an honest operator's audit rate and raises job value; a caught
   cheat wipes stake and reputation, and pays the challenger from the slash.
7. **TEE tier (optional, additive).** Operators with an attestable enclave seal the signing key
   to measured code — their submission *is* cryptographic proof-of-execution. Reward with higher
   trust weight, lower audit rate, premium jobs. Kept optional so no chip-vendor dependency is
   forced on the network; sovereignty preserved.

**Future lane — ZKML.** A zero-knowledge proof that a specific model produced a specific output
would give cryptographic proof on *any* hardware with no TEE and no re-run. Currently too
expensive to prove for real-size models; watch, don't build on. The stack above is structured so
ZKML slots in beside TEE later with no rework.

## 6. The hard part (stated honestly)

- **Similarity-tolerance calibration is where this lives or dies**, and it is empirical. Build a
  measurement harness: run the same jobs across the real hardware fleet and across same-seed
  re-runs, measure the output-divergence distribution, and set the tolerance from that
  distribution. Do not spec the threshold on paper.
- **Jury game theory.** Defend against the lazy juror (always votes "similar") and collusion:
  Schelling-point scoring + stake + occasional canary *judgments* (known-answer similarity cases
  that catch rubber-stamping).
- **Open-ended high-variance tasks.** Even with a seed, a borderline probability can tip one
  token early and diverge the whole continuation, so "similar to a re-run" is a weak signal for
  wildly creative generation. Mitigations: route such jobs to redundant-quorum or TEE lanes,
  bound max-tokens for verifiable jobs, or compare at the logit level on early tokens.

## 7. What HONE already has vs. the gap (verified 2026-07-20)

**Already built (`rust/hone-node`):**
- Staking: `/api/stake`, `/api/unstake`, `/api/node/role/stake`, `/api/node/role/unstake`.
- Inference reputation: `/api/task/reputation/:node`, `get_inference_reputation`.
- Escrow: `fee_escrow` in `agent_session.rs` (debit / refund / protocol-fee / sweep-refund).
- Inference job lifecycle ledger entries in `api.rs` (~L2327+):
  `InferenceJobPost · InferenceJobBid · InferenceJobCommit · InferenceJobComplete ·
  InferenceJobVerify · InferenceJobClaim · InferenceJobCancel` — i.e. a
  commit → verify → claim scaffold already exists.
- Challenge primitive: `chain_challenges` (nonce + issued_at) in `api.rs`.

**The gap this design fills:**
- The **verify policy** behind `InferenceJobVerify`: denier selection, seed-reproduction check,
  similarity scoring, jury quorum.
- **Slash-on-mismatch** wired to the existing stake balances.
- **Canary injection** at a controlled random rate.
- The **manifest allowlist** + **reproducible-build/sign pipeline**.
- **Seed recording** inside the forked engine / `hone-infer` wrapper.

## 8. Non-goals / boundaries

- Touches no consensus, clock, genesis, chain-id, launch, or gate code.
- Branch-only; this is a design note, not wiring.
- Any move from this design to live code goes through the normal in-person gate.
