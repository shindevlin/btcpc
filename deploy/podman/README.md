# HONE node — podman-first, docker-compatible

One `Containerfile` builds a self-contained `hone-node` image: `hone-node` + the
`hone` CLI, compiled from source (multi-stage — a `rust:1.90-bookworm` build
stage, a `debian:bookworm-slim` runtime stage with no toolchain or source
left in it). Text inference is **Candle, in-process** — no Ollama sidecar
(`rust/hone-node/src/inference_engine.rs`, feature `inference-embedded`, on
by default). Image/video generation (ComfyUI) is a separate, optional pod
member — see [ComfyUI (optional)](#comfyui-optional-imagevideo) below.

This supersedes the repo-root `Dockerfile` / `Dockerfile.clock` /
`docker-compose.yml`, which predate this build and don't build cleanly
against the current workspace layout (missing workspace members in their
COPY steps, `rust:1.80` base vs. the repo's pinned `1.90.0` toolchain, and
`docker-compose.yml` is the pre-Rust-rewrite Node.js/Mongo stack). Those
files are left in place but are not the supported path going forward —
everything under `deploy/podman/` is the one coherent story.

## One-command bring-up

```sh
cp deploy/podman/.env.example deploy/podman/.env
# edit deploy/podman/.env — at minimum set HONE_ACCOUNT

podman build -t localhost/hone-node:latest -f Containerfile .

# Podman-native (recommended):
set -a; source deploy/podman/.env; set +a
envsubst < deploy/podman/kube.yaml | podman play kube -

# — or, the Docker-compatible path: —
docker compose --env-file deploy/podman/.env -f deploy/podman/compose.yaml up -d --build
```

Either way:

```sh
curl http://localhost:4242/api/node/info
```

should return JSON with your `account`, `chain_id`, role flags and version
once the node is up (the healthcheck in both compose.yaml and the Quadlet
unit polls the same endpoint).

Tear down: `podman play kube --down deploy/podman/kube.yaml` or
`docker compose -f deploy/podman/compose.yaml down`.

`envsubst` (package `gettext-base` / `gettext` on most distros) is required
for the podman path — `podman play kube` does not do `.env` substitution
itself, unlike `docker compose`.

## "Always runs" — Quadlet (systemd)

For a node that survives reboots and crash-restarts without a login session
running docker-compose:

```sh
mkdir -p ~/.config/containers/systemd
cp deploy/podman/hone-node.container deploy/podman/hone-data.volume \
   ~/.config/containers/systemd/
cp deploy/podman/.env ~/.config/containers/systemd/hone-node.env
systemctl --user daemon-reload
systemctl --user enable --now hone-node.service
loginctl enable-linger "$USER"   # starts at boot, survives logout
```

`systemctl --user status hone-node.service` / `journalctl --user -u
hone-node -f` for logs. Podman regenerates the systemd unit from the
`.container`/`.volume` files automatically — edit those, not generated
unit files under `/run`.

## Isolated / testnet

Never point a container build at mainnet genesis for testing. Set in `.env`:

```
HONE_ISOLATED=true
HONE_CHAIN_ID=hone-testnet
HONE_GENESIS_FILE=/app/testnet-genesis.json   # baked into the image
```

and pass `-e HONE_GENESIS_FILE=/app/testnet-genesis.json` (or the compose/kube
env equivalent) — the image ships both `genesis.json` and
`testnet-genesis.json` at `/app/`.

## CPU vs. GPU

- **CPU-only (default, always works):** the shipped image's candle backend
  defaults to `HONE_INFER_DEVICE=cpu` even when a CUDA-capable binary is
  used (see `inference_engine.rs::resolve_device` — this is deliberate, for
  mining-consensus determinism, not an oversight). A node with no GPU runs
  fine as a clock-only/relay contributor, or with CPU-bound candle inference
  if a model is configured. No extra flags needed.
- **GPU (mining):**
  - Podman: `podman run --device nvidia.com/gpu=all ...` (CDI). Requires
    `nvidia-container-toolkit` on the host with `nvidia-ctk cdi generate`
    run once to produce `/etc/cdi/nvidia.yaml`. In Quadlet, uncomment
    `AddDevice=nvidia.com/gpu=all` in `hone-node.container`.
  - Docker: `docker run --gpus all ...`, or uncomment the
    `deploy.resources.reservations.devices` block in `compose.yaml`.
  - `podman play kube` has no reliable per-container CDI device request —
    use the Quadlet path for a GPU node, not `kube.yaml`.
  - The shipped `Containerfile` builds the CPU-only candle feature set. A
    CUDA-enabled binary needs `--features cuda` compiled against an
    `nvidia/cuda:*-devel` builder image (candle-core's `cuda` feature pulls
    in `cudarc`); this was **not build-tested** in this change (no GPU/CUDA
    toolchain available in the dev sandbox that produced this image) — treat
    a from-source CUDA build as a follow-up to validate on real GPU hardware
    before relying on it for mining. Until then, GPU passthrough +
    `HONE_INFER_DEVICE=cuda` on a CPU-feature binary will not accelerate
    inference (the CPU backend just runs on CPU regardless of device
    visibility) — this is the honest current state, not a hidden bug.

## Models — chain-approved only (load-bearing)

`hone-node` never auto-downloads a model
(`rust/hone-node/src/inference_engine.rs`: "there is NO hardcoded default
model and the node never auto-downloads one" — by design, so the chain can
guarantee which model produced an inference). The container's
`entrypoint.sh` owns fetching instead:

1. If `HONE_MODEL` names a file already present in the `/data/models`
   volume, it's used as-is — no network activity.
2. Else if `HONE_MODEL_URL` is set, the entrypoint downloads it (retrying
   with backoff), verifies its sha256 against `HONE_MODEL_SHA256` if you
   pinned one, then cross-checks the model's filename against the chain's
   `GET /api/chain/approved_models` list (open/empty list = any name
   accepted, per `api.rs::get_approved_models`) once the node's own API is
   reachable. A model that fails either check is refused and deleted; the
   node keeps running clock-only/relay in the meantime — it never crash-loops
   on a bad model.
3. If neither is set, the node runs clock-only/relay from the start.

**Known gap, flagged rather than papered over:** the chain also supports a
*stricter* per-hash allowlist, `chain_param:approved_model_hashes`
(`chain.rs` ~line 1015, checked against a `Mine` entry's `model_hash` at
mining time) — but there is **no HTTP endpoint that reads it**
(`api.rs` only exposes `GET`/`POST /api/chain/approved_models`, by name).
This container can enforce the name-list and an operator-pinned sha256, but
cannot fetch and enforce the chain's hash allowlist end-to-end because
hone-node doesn't expose it yet. That's a real HONE feature gap, not a
container-build problem — track it as a separate order (add a
`GET /api/chain/approved_model_hashes` endpoint) rather than inventing
client-side governance here.

Model weights are **always** volume-mounted (`/data/models`), never baked
into the image.

## ComfyUI (optional, image/video)

Only nodes doing image/video generation work need this — text/clock/other
roles should leave it disabled. It's a separate, large Python+CUDA service
(not Rust, not in-process) that will eventually be replaced; see
`rust/wiiv/src/comfy.rs` for the Rust-side supervisor that drives it today
for the Wan2.2 video model.

- compose: `docker compose --profile image-video -f deploy/podman/compose.yaml up -d`
- podman: `podman play kube deploy/podman/kube-comfyui.yaml` alongside `kube.yaml`
- Quadlet: also install `hone-comfyui.container`,
  `hone-comfyui-models.volume`, `hone-comfyui-output.volume`

No ComfyUI image is vendored by this repo — set `COMFYUI_IMAGE` in `.env`
(compose/kube) or edit `Image=` directly in `hone-comfyui.container`
(Quadlet unit files don't expand env vars in their own directives).

## Self-healing

Implemented in `entrypoint.sh` (container-level) plus what's already in
`hone-node` itself:

| Behavior | Where |
|---|---|
| Retry model download with backoff | `entrypoint.sh` |
| Refuse/fallback on bad model (hash or chain-approval mismatch) | `entrypoint.sh` |
| Clock-only when no/failed model | `entrypoint.sh` (never blocks node startup) |
| Persist secrets passphrase across restarts | `entrypoint.sh` (`/data/.secrets_passphrase`) |
| Skip taken ports (self-hosted child nodes) | `services.rs::find_free_port` (in-binary) |
| Reconnect peers with backoff | libp2p / `discovery.rs` (in-binary) |
| Restart on crash | `Restart=unless-stopped`/`always` (compose/Quadlet), not the entrypoint |

## Secrets / keys

Wallet keys and the encrypted secret store (`secret_store.rs`,
`~/.hone/secrets.enc` inside the container → `/data/secrets.enc` via
`HONE_DATA_DIR=/data`) live only in the `/data` volume. `HONE_POSTING_KEY`
and `HONE_SECRETS_PASSPHRASE`, if you set them yourself instead of letting
the entrypoint generate/persist one, belong in `.env` (gitignored) or a
podman secret (`podman secret create` + `Secret=` in the Quadlet unit) —
never in the image, never in Ferryman, never committed.
