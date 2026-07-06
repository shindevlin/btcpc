# Wiiv Protocol

**Version 0.1 — July 2026**

---

## Overview

Wiiv is HONE's decentralized **rendering platform** — a native marketplace protocol for
turning a creative brief into a finished rendered artifact using the network's supply of
compute, models, and human specialists. It is **modality-agnostic**: image, 3D, audio,
and video renders are all first-class, and the same job/settlement machinery serves any
of them.

Wiiv is baked into the chain as native ledger entry types, not a smart contract or
sidechain. Render jobs, worker bids, milestones, artifacts, escrow, acceptance, and
disputes are first-class chain primitives. A buyer arrives with rough intent and leaves
with a finished deliverable; Wiiv coordinates local models, distributed GPU workers,
human specialists, storage nodes, and reviewers as one production supply chain, and
settles payment against buyer acceptance.

The reserved accounts `wiiv` and `wiiv-escrow` are seeded at block 0 and cannot be
created or destroyed by any participant.

**Design principle — subjective quality is never a consensus input.** The chain records
*facts* (a job was posted, a bid was made, an artifact CID was delivered, the buyer
accepted, escrow was released). Whether a render is *good* is decided by the buyer, by a
reviewer market, and by reputation — never by chain validators. Nodes never vote on
aesthetics.

---

## Render Modalities

A Wiiv job declares one primary `modality`. The protocol is identical across them; only
the required worker capabilities and deliverable kinds differ.

| Modality | Example deliverables | Typical capabilities |
|---|---|---|
| `image` | stills, keyframes, thumbnails, textures | `image_generation`, `upscaling` |
| `video` | commercials, shorts, episodes, films | `video_generation`, `voice_synthesis`, `music_generation`, `editing_assembly`, `subtitles` |
| `audio` | voiceover, music beds, sound design | `voice_synthesis`, `music_generation` |
| `threed` | 3D models, scenes, printable geometry from references | `threed_generation`, `upscaling` |
| `composite` | mixed-media projects that fan out across the above | any combination |

Modalities are an open set: new ones are added by registering the capability classes a
worker advertises (see Capability Discovery), not by changing the entry schema.

---

## Key Roles

Wiiv reuses HONE's role-key model. A participant signs each entry with the appropriate
role key.

| Role | What they sign |
|---|---|
| **posting** | `WiivRenderJobPost`, `WiivWorkerRegister`, `WiivMilestoneDeliver` — day-to-day job and worker operations |
| **active** | Escrow funding on `WiivJobFund`; the payer role in settlement |
| **memo** | `WiivRenderBid` — a worker's bid, optionally carrying an encrypted note to the buyer |
| **seek** | `WiivArtifactDeliver` — attaches encrypted final artifacts for the buyer |
| **hide** | Receives encrypted artifact payloads for private jobs; decrypts with their private key |

---

## Entry Types

| Entry type | Who signs | What it does |
|---|---|---|
| `WiivWorkerRegister` | worker (posting) | Advertises a worker's capability classes, modalities, price hints, and hardware attestation. Stake-backed; slashable for fraud. |
| `WiivWorkerHeartbeat` | worker (posting) | Keeps a worker's capability listing live and updates availability. Stale workers drop out of matching. |
| `WiivRenderJobPost` | buyer (posting) | Posts a render job: modality, compiled plan (milestones, deliverable kinds, required capabilities), max budget, revision policy, artifact retention policy, visibility. |
| `WiivJobFund` | buyer (active) | Commits escrow to a posted job. In dry-run / pre-cutover builds this is simulated and never moves value. |
| `WiivRenderBid` | worker (memo) | A worker bids on a job or a specific milestone: price, ETA, and the capabilities it will supply. |
| `WiivBidAward` | buyer (posting) | Awards a job/milestone to a chosen worker (or worker set). Locks the awarded portion of escrow. |
| `WiivMilestoneDeliver` | worker (posting) | Submits a milestone deliverable: artifact CIDs + a provenance record. Moves the milestone to `delivered`. |
| `WiivArtifactDeliver` | worker (seek) | Attaches the final artifact bundle (CIDs), encrypted to the buyer's hide key for private jobs. |
| `WiivMilestoneAccept` | buyer (posting) | Buyer (or reviewer market on the buyer's behalf) accepts a milestone. Releases that milestone's escrow tranche. |
| `WiivRevisionRequest` | buyer (posting) | Sends a delivered milestone back with notes. Bounded by the job's revision policy; may carry a fee. |
| `WiivJobAccept` | buyer (posting) | Accepts the full deliverable. Triggers final settlement. |
| `WiivJobSettle` | escrow (system) | Releases remaining escrow to workers and records the job as settled. System entry — applied on acceptance, not user-submitted. |
| `WiivDisputeOpen` | buyer (posting) | Opens a dispute when the deliverable does not match the accepted plan. Engages the reviewer market; freezes settlement. |
| `WiivDisputeResolve` | reviewers (posting) | Records a reviewer-market outcome (release / partial / refund). Reputation-weighted; never decided by chain validators. |
| `WiivStorageExtend` | buyer (posting) | Pays to retain specific artifact CIDs beyond the default retention window, like `LinkGitStorageExtend`. |

---

## Render Job Lifecycle

A job's top-level status:

```
drafted → quoted → funded → in_production → in_review → accepted → settled
                       │            │             │           │
                       └── cancelled┘             └── disputed ┘
```

Milestones have their own state machine so long-form / multi-shot work settles piecewise:

```
pending → active → delivered → accepted
                       │            
                       ├── revision_requested → active
                       └── rejected
```

- A milestone may only become `active` once every milestone it `depends_on` is
  `accepted` (dependency gating — the compiler emits a linear chain by default; workers
  can be fanned out once capability discovery is live).
- Each milestone carries its **deliverable kinds** (from the vocabulary below), its
  **required capabilities**, and its **dependency edges**. This is the shape the MCP
  render layer compiles to (`src/mcp/`), so an off-chain plan maps 1:1 onto a
  `WiivRenderJobPost`.

### Deliverable kinds

`creative_brief`, `script`, `storyboard`, `shot_list`, `generated_scene`, `generated_image`,
`generated_model`, `voiceover`, `music`, `sound_design`, `edit_assembly`, `captions`,
`color_grade`, `upscale`, `final_render`, `project_bundle`, `provenance`.

---

## Capability Discovery

Workers advertise what they can do via `WiivWorkerRegister` + `WiivWorkerHeartbeat`. A
registration declares:

- **capability classes** it serves (`image_generation`, `video_generation`,
  `voice_synthesis`, `music_generation`, `threed_generation`, `editing_assembly`,
  `upscaling`, `subtitles`, `storage`, `review`),
- the **modalities** it covers,
- **price hints** (per unit of work) and throughput/ETA hints,
- a **hardware attestation** (GPU serial / machine-id, reusing HONE's anti-sybil
  hardware identity) and a **stake**.

Matching is off-chain: a buyer (or Claude acting as producer) reads the live worker set,
filters by the job's required capabilities, and awards bids. The chain records the
awards and deliveries, not the matching algorithm. Registrations are stake-backed and
slashable for capability fraud (advertising work a worker cannot deliver) or
censorship / double-award abuse.

---

## Artifacts & Provenance

Every delivered artifact is a content-addressed blob (CID) in HONE-FS. A
`WiivMilestoneDeliver` / `WiivArtifactDeliver` attaches:

- the artifact **CID(s)** and their deliverable kinds,
- a **provenance record**: which worker produced it, which capability/model was used,
  the input CIDs it derived from, and the plan/milestone it satisfies.

Provenance is append-only and travels with the job, so a finished deliverable carries a
verifiable chain of custody from brief to final render. Private-job artifacts are
encrypted to the buyer's hide key before storage — no storage node can read them without
the key. Default retention is bounded; `WiivStorageExtend` pays to keep specific CIDs
alive longer.

---

## Escrow & Settlement

- **Funding**: `WiivJobFund` commits escrow up to the job's `max_hunits` budget cap. The
  cap is a hard ceiling; awards can never lock more than the buyer funded.
- **Piecewise release**: accepting a milestone (`WiivMilestoneAccept`) releases that
  milestone's tranche to its awarded worker(s). This lets long-form jobs pay out as they
  progress instead of all-or-nothing at the end.
- **Final settlement**: `WiivJobAccept` triggers `WiivJobSettle` (a system entry from the
  `wiiv-escrow` account) releasing any remaining escrow and marking the job settled.
- **Disputes**: `WiivDisputeOpen` freezes settlement and engages the reviewer market;
  `WiivDisputeResolve` records the reputation-weighted outcome (release / partial /
  refund). Reviewers earn from a dispute fee; bad-faith reviewing is slashable.

Fees and prices are denominated in **hunits** (1 HONE = 10^10 hunits).

**Safety boundary (pre-cutover).** Until wallet-scoped authorization, spend caps, and the
live chain routes above exist, the off-chain Wiiv/MCP layer runs **dry-run only**: it
plans, quotes, and simulates jobs but posts nothing on-chain and moves no value. Live
posting is gated behind an explicit opt-in **and** a hard "not wired yet" stop. Do not
weaken this guardrail until scoped auth + confirmation flows exist.

---

## Reserved Accounts

| Account | Purpose |
|---|---|
| `wiiv` | Protocol-level operations account for the Wiiv rendering service (worker registry, matching hints). |
| `wiiv-escrow` | Holds job escrow and signs `WiivJobSettle` releases. |

Both accounts are provisioned in `genesis.json` with no keys. Keys are registered at
service startup via `AccountUpdateKey`.

---

## Working with Wiiv today

The Wiiv render layer is exposed to LLM producers over MCP (see
`docs/reports/HONE_MCP_FOR_CLAUDE_DESIGN.md` and `src/mcp/`). Claude acts as the creative
producer — understanding intent, drafting the brief, compiling the plan, quoting, and
walking the job through its lifecycle — while HONE workers execute and the chain settles.

The `rust/wiiv/` crate provides the Rust-side job/worker types and a client against the
node API. On-chain entry types land as the chain routes are implemented; until then the
MCP layer operates in dry-run against these shapes so plans and jobs built today map
cleanly onto future `Wiiv*` ledger entries.

---

_Wiiv plans the render, HONE renders it, the chain settles it._
