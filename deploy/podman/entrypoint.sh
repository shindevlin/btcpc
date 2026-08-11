#!/usr/bin/env bash
# Self-healing entrypoint for the hone-node container.
#
# Scope (see README.md "Self-healing" + docs/SELF_HEAL_PRD.md, adapted for the
# candle-in-process design — hone-node itself never auto-downloads a model,
# see rust/hone-node/src/inference_engine.rs, so the container owns fetch):
#   - persist a secrets passphrase across restarts (hw-fingerprint identity
#     is not stable across container recreation/host moves)
#   - fetch + chain-approved-hash-verify a model into the volume-mounted
#     model cache, retrying with backoff, WITHOUT blocking node startup
#   - fall back to clock-only/relay operation when no model is configured
#     or fetch/verification fails, instead of crash-looping
#   - hand off to `exec` so the node is PID 1 and receives signals directly
#
# KNOWN GAP (flag for a separate order, do not paper over here): hone-node's
# chain-governed model allowlist (chain_param:approved_models, chain.rs) is
# only readable by name via GET /api/chain/approved_models. The stricter
# chain_param:approved_model_hashes check (chain.rs ~line 1015) that a Mine
# entry's model_hash is validated against has NO HTTP read endpoint today —
# there is nothing this container can query to fetch that hash allowlist.
# Until hone-node exposes it, this entrypoint verifies the fetched model
# against an operator-pinned HONE_MODEL_SHA256 (if given) and against the
# chain's approved *name* list once the node is reachable; it cannot enforce
# the on-chain hash allowlist end-to-end.
set -euo pipefail

log() { printf '[hone-entrypoint] %s\n' "$*" >&2; }

# Only wrap the node's own startup. `podman run img hone wallet ...` or any
# other command passed as CMD should execute as-is, untouched.
if [ "${1:-}" != "hone-node" ]; then
  exec "$@"
fi

DATA_DIR="${HONE_DATA_DIR:-/data}"
MODEL_DIR="${HONE_MODEL_DIR:-$DATA_DIR/models}"
mkdir -p "$DATA_DIR" "$MODEL_DIR"

# ── Persist the secrets passphrase across restarts ─────────────────────────
# secret_store.rs derives the AES-256-GCM key from HONE_SECRETS_PASSPHRASE,
# falling back to a hardware fingerprint. Hardware fingerprints are not
# stable across container recreation or host moves, so pin one explicitly
# and keep it in the same volume as the data it protects.
PASSPHRASE_FILE="$DATA_DIR/.secrets_passphrase"
if [ -z "${HONE_SECRETS_PASSPHRASE:-}" ]; then
  if [ -f "$PASSPHRASE_FILE" ]; then
    HONE_SECRETS_PASSPHRASE="$(cat "$PASSPHRASE_FILE")"
  else
    log "no HONE_SECRETS_PASSPHRASE set — generating and persisting one to $PASSPHRASE_FILE"
    HONE_SECRETS_PASSPHRASE="$(head -c 32 /dev/urandom | sha256sum | cut -d' ' -f1)"
    umask 077
    printf '%s' "$HONE_SECRETS_PASSPHRASE" > "$PASSPHRASE_FILE"
  fi
  export HONE_SECRETS_PASSPHRASE
fi

# ── Model fetch + verify (backgrounded — node runs clock-only meanwhile) ───
fetch_and_verify_model() {
  local url="$1" dest="$2" want_sha="${3:-}"
  local tmp="$dest.part"
  local attempt=1 max_attempts=6 delay=5

  while [ "$attempt" -le "$max_attempts" ]; do
    log "model fetch attempt $attempt/$max_attempts: $url"
    if curl -fsSL --retry 0 -o "$tmp" "$url"; then
      break
    fi
    log "fetch failed, retrying in ${delay}s"
    sleep "$delay"
    delay=$((delay * 2))
    attempt=$((attempt + 1))
  done
  if [ ! -f "$tmp" ]; then
    log "model fetch exhausted retries — continuing without a model (clock-only/relay)"
    return 1
  fi

  local got_sha
  got_sha="$(sha256sum "$tmp" | cut -d' ' -f1)"
  if [ -n "$want_sha" ] && [ "$got_sha" != "$want_sha" ]; then
    log "REFUSING model: sha256 $got_sha does not match HONE_MODEL_SHA256 $want_sha"
    rm -f "$tmp"
    return 1
  fi

  # Cross-check against the chain's approved-model name list once the node's
  # own API is up (it starts before this check completes — see below). An
  # empty list means "open era, any model accepted" (api.rs get_approved_models).
  local api="http://127.0.0.1:${HONE_API_PORT:-4242}"
  local name
  name="$(basename "$dest")"
  for _ in $(seq 1 12); do
    if approved_json="$(curl -fsS "$api/api/chain/approved_models" 2>/dev/null)"; then
      if printf '%s' "$approved_json" | grep -q '"open":true'; then
        log "chain approved_models list is open — accepting $name (sha256 $got_sha)"
        break
      fi
      if printf '%s' "$approved_json" | grep -q "\"$name\""; then
        log "chain approved_models includes $name — accepting (sha256 $got_sha)"
        break
      fi
      log "REFUSING model: $name is not in the chain's approved_models list ($approved_json)"
      rm -f "$tmp"
      return 1
    fi
    sleep 5
  done

  mv "$tmp" "$dest"
  log "model ready at $dest (sha256 $got_sha)"
  return 0
}

if [ -n "${HONE_MODEL:-}" ]; then
  MODEL_PATH="$MODEL_DIR/${HONE_MODEL##*/}"
  case "$HONE_MODEL" in
    /*) MODEL_PATH="$HONE_MODEL" ;;
  esac
  if [ -f "$MODEL_PATH" ]; then
    log "model already present: $MODEL_PATH"
  elif [ -n "${HONE_MODEL_URL:-}" ]; then
    ( fetch_and_verify_model "$HONE_MODEL_URL" "$MODEL_PATH" "${HONE_MODEL_SHA256:-}" || true ) &
  else
    log "HONE_MODEL=$HONE_MODEL set but no file present and no HONE_MODEL_URL to fetch it from — starting clock-only/relay until an operator places it in $MODEL_DIR"
  fi
else
  log "no HONE_MODEL configured — starting as clock-only/relay contributor (hardware_probe/HONE_WORKER decide mining eligibility)"
fi

log "starting hone-node (account=${HONE_ACCOUNT:-genesis} chain_id=${HONE_CHAIN_ID:-hone} api_port=${HONE_API_PORT:-4242} p2p_port=${HONE_P2P_PORT:-6942})"
exec /usr/local/bin/hone-node
