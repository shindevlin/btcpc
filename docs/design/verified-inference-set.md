# Verified Inference Set + Model Rotation

**Draft — July 2026. Companion to `docs/MODEL_REGISTRY_PROTOCOL.md`.**
**Data file: `rust/hone-node/verified-inference-set.json` (Shin-signed, versioned).**

---

## Why this exists (and why it does NOT break sovereignty)

`MODEL_REGISTRY_PROTOCOL.md` is deliberate: HONE blesses no model, hardcodes no
default, auto-downloads nothing, and keeps subjective quality off-chain. That stays
true. This document adds ONE thing on top, scoped to exactly the tier that needs it.

There are two tiers of inference on HONE:

1. **Open tier** — unchanged. An operator runs whatever GGUF they want via
   `hone model list/enable`. Local, un-refereed, un-slashable. HONE picks nothing.

2. **Verified tier** — paid jobs whose output is checked by random *denier*
   re-execution and a staked similarity jury, with stake/slash on divergence. This
   tier CANNOT work on arbitrary weights: if two nodes run different models (or the
   same model at a different quant), their outputs legitimately differ and the jury
   can't tell honest divergence from fraud. So the verified tier needs a small,
   agreed, content-hash-pinned allowlist of engine+model pairs. That allowlist is
   the **Verified Inference Set (VIS)**.

VIS membership is not a quality ranking and not a default. It only means: *a node
may earn VERIFIED, slashable rewards serving this exact engine+weights, because
every referee is running the same thing.* Operators still run anything they like on
the open tier. Nothing about the open registry changes.

This is the "permissioned and versioned, for chain safety" requirement — applied to
the one tier that structurally requires it, and nowhere else.

### Founder / prime-node phase (now)

During bootstrap the founding fleet **is** the network — the prime nodes. So the VIS
is simply the prime nodes' direct choice of what the chain runs: Shin signs the set,
the prime nodes serve it, done. There are no independent operators to override yet, so
"blessing these models" is just the network deciding for itself, which is the prime
nodes' job right now. This is not a special case bolted on — it's the same VIS
mechanism, exercised by the only operators that currently exist. Because the open tier
already exists in the architecture, nothing here has to be undone when outside
operators later join: they inherit a signed set to referee against, and gain the open
tier for anything else. We choose now; we don't have to walk it back later.

## The Set

`verified-inference-set.json` is the signed data. Initial members:

| name | tier | role | arch | quant | ~VRAM |
|---|---|---|---|---|---|
| qwen3.6-27b | flagship | general reasoning | qwen3 dense | Q4_K_M | ~22 GB |
| qwen3.6-35b-a3b | flagship | general, fastest | qwen3 MoE (3B active) | IQ4_XS | ~22 GB |
| gpt-oss-20b | flagship | tool-calling | gpt-oss dense | Q5_K_M | ~18 GB |
| mistral-small-3.2-24b | standard | general | mistral dense | Q4_K_M | ~16 GB |
| qwen3.6-14b | standard | general | qwen3 dense | Q4_K_M | ~11 GB |
| qwen3.6-8b | lite | general | qwen3 dense | Q4_K_M | ~6.5 GB |
| qwen3.6-4b | micro | general | qwen3 dense | Q4_K_M | ~3.5 GB |

The three flagships are the named targets; the standard/lite/micro rows are the
"lesser models" so weaker cards (and CPU/edge) still earn on the verified tier.

## Content-hash pinning (the crux, and the honest part)

The registry key is `weights_sha256` — the sha256 of the *exact* GGUF bytes. It is
what makes membership meaningful: "qwen3.6-27b Q4_K_M" is a label, but the hash is
the identity a referee re-executes against. In the data file every `weights_sha256`
is `PENDING-INGESTION`. That is not a placeholder to paper over — it is the required
process:

1. **Ingest.** On a fleet machine, fetch the exact GGUF for each row (pin the source
   repo + revision), then `sha256sum` the file.
2. **Populate.** Write the real hash into each row; confirm `architecture`, `quant`,
   `context`, and file size against the GGUF header (`hone model show` already reads
   these).
3. **Pin the engine.** Add the candle engine `semver` + `binary_hash` the verified
   tier runs against.
4. **Sign.** Shin signs *this exact populated version* with the vault key, in person,
   and `set_version` bumps from 0 → 1. Only then is the set usable for verified
   rewards. **No agent may sign or bump the set.** Additive changes re-sign.

A hash may never be invented. An unsigned or hash-incomplete set is `draft` and earns
nothing on the verified tier.

## Rotation (the shuffler)

Today `~/.hone/model.json` holds a single `enabled` model. Rotation extends this
without changing the single-model default behavior:

```jsonc
{
  "enabled": "qwen3.6-27b.gguf",          // unchanged: the fallback resident model
  "rotation": {
    "enabled": false,                      // OFF by default — opt-in, like everything
    "set": ["qwen3.6-27b", "gpt-oss-20b"], // VIS members this operator will serve
    "base_resident": "qwen3.6-27b",        // stays loaded; the anchor
    "policy": "demand-weighted",           // pick next by job value / urgency
    "vram_budget_mb": 24000,
    "swap_hysteresis_secs": 120,           // don't thrash; min dwell before a swap
    "swap_cooldown_secs": 60
  }
}
```

Selection each cycle (node-side shuffler, per the model-shuffler design):

- **Candidates** = VIS members ∩ (fits this machine's detected hardware, reusing
  `hone_provision::Requirements::met_by` — the same gate `hone model` already uses)
  ∩ the operator's opted-in `set`. Never the whole VIS; never anything unfitting.
- **Score** a pending job's model as `reward * urgency(age) / (compute + swap_cost)`.
  A job for a model already resident has `swap_cost = 0`, so the base model wins ties
  and the node doesn't thrash.
- **Hysteresis + cooldown** bound swaps so a burst of mixed jobs can't ping-pong the
  GPU. LoRA/adapter jobs hot-swap the *adapter* on top of the resident base rather
  than reloading a base model (VRAM budget permitting).
- **Depletion / no demand** → sit on `base_resident`. With rotation off, behavior is
  exactly today's single enabled model.

Role-aware routing falls out of the `role` field: tool-calling jobs prefer
`gpt-oss-20b`, general reasoning prefers `qwen3.6-27b`, throughput-sensitive batches
prefer the `35b-a3b` MoE — all still gated by hardware fit and operator opt-in.

## What must be built (fleet) vs. what this is (design + data)

This doc + `verified-inference-set.json` are the design and the (unsigned, un-pinned)
data. Still to build, on a warm-cache fleet machine, branch-only, never main:

1. Ingest + hash-pin + header-verify each row; populate `weights_sha256`.
2. Add engine `semver`/`binary_hash` pins; wire the verified tier to reject a job
   whose engine+weights aren't a signed VIS member.
3. Extend `model.json` with the `rotation` block and implement the shuffler
   selection above (base-resident + adapter hot-swap + hysteresis).
4. Tests: candidates never include unfitting or non-VIS models; rotation off ==
   today's behavior; a swap respects hysteresis/cooldown; an unsigned set earns
   nothing on the verified tier.

Then Shin signs the populated set (`set_version` 0 → 1) in person, and only after
that does the verified tier honor it.
