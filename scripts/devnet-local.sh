#!/usr/bin/env bash
# devnet-local.sh — stand up an N-node isolated local devnet for manual/CI proof-of-life
# checks (quorum formation, epoch sealing, reward accounting, restart-safety).
#
# NOT for production. HONE_ISOLATED=true — nodes dial ONLY each other, no public
# discovery, no mainnet bootstrap, no genesis/chain-id flip.
#
# Usage:
#   bash scripts/devnet-local.sh start [N]     # start N nodes (default 4)
#   bash scripts/devnet-local.sh stop          # kill all devnet nodes
#   bash scripts/devnet-local.sh status        # show each node's /api/latest + state_root
#   bash scripts/devnet-local.sh restart <idx> # kill+restart one node (0-based) to prove rejoin

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${HONE_DEVNET_BIN:-$ROOT_DIR/rust/target/release/hone-node}"
BASE_DIR="${HONE_DEVNET_DIR:-/tmp/hone-devnet}"
BASE_API_PORT="${HONE_DEVNET_API_BASE:-14242}"
BASE_P2P_PORT="${HONE_DEVNET_P2P_BASE:-16942}"
CHAIN_ID="${HONE_DEVNET_CHAIN_ID:-hone-testnet}"
GENESIS_TS_FILE="$BASE_DIR/genesis_timestamp"

mkdir -p "$BASE_DIR"

node_dir()   { echo "$BASE_DIR/node$1"; }
node_api()   { echo $((BASE_API_PORT + $1)); }
node_p2p()   { echo $((BASE_P2P_PORT + $1)); }
node_pid_f() { echo "$(node_dir "$1")/node.pid"; }
node_log_f() { echo "$(node_dir "$1")/node.log"; }

bootstrap_peers_for() {
    local self_idx="$1" n="$2" peers=""
    for ((i = 0; i < n; i++)); do
        [[ "$i" == "$self_idx" ]] && continue
        local addr="/ip4/127.0.0.1/tcp/$(node_p2p "$i")"
        peers="${peers:+$peers,}$addr"
    done
    echo "$peers"
}

start_node() {
    local idx="$1" n="$2" ts="$3"
    local dir; dir="$(node_dir "$idx")"
    mkdir -p "$dir"
    HONE_DATA_DIR="$dir" \
    HONE_ACCOUNT="devnode$idx" \
    HONE_NODE_ID="devnode$idx" \
    HONE_API_PORT="$(node_api "$idx")" \
    HONE_P2P_PORT="$(node_p2p "$idx")" \
    HONE_BOOTSTRAP_PEERS="$(bootstrap_peers_for "$idx" "$n")" \
    HONE_CHAIN_ID="$CHAIN_ID" \
    HONE_GENESIS_TIMESTAMP="$ts" \
    HONE_ISOLATED=true \
    HONE_CLOCK=true \
    HONE_MINER=true \
    HONE_SIM=false \
    HONE_THROTTLE_DISABLED=1 \
    HONE_LOG_LEVEL="hone_node=info" \
    "$BIN" >"$(node_log_f "$idx")" 2>&1 &
    echo $! >"$(node_pid_f "$idx")"
    echo "node$idx: pid=$! api=$(node_api "$idx") p2p=$(node_p2p "$idx") data=$dir"
}

cmd_start() {
    local n="${1:-4}"
    if [[ -f "$GENESIS_TS_FILE" ]]; then
        ts="$(cat "$GENESIS_TS_FILE")"
    else
        ts="$(date +%s%3N)"
        echo "$ts" >"$GENESIS_TS_FILE"
    fi
    echo "devnet: starting $n nodes, chain_id=$CHAIN_ID genesis_ts=$ts"
    for ((i = 0; i < n; i++)); do
        start_node "$i" "$n" "$ts"
    done
}

cmd_stop() {
    for pidf in "$BASE_DIR"/node*/node.pid; do
        [[ -f "$pidf" ]] || continue
        pid="$(cat "$pidf")"
        kill "$pid" 2>/dev/null || true
        rm -f "$pidf"
    done
    echo "devnet: stopped"
}

cmd_status() {
    for dir in "$BASE_DIR"/node*; do
        [[ -d "$dir" ]] || continue
        idx="${dir##*/node}"
        port="$(node_api "$idx")"
        echo "-- node$idx (api $port) --"
        curl -s "http://127.0.0.1:$port/api/latest" || echo "(unreachable)"
        echo
        curl -s "http://127.0.0.1:$port/api/chain/state_root" || echo "(unreachable)"
        echo
    done
}

cmd_restart() {
    local idx="$1"
    local n
    n="$(ls -d "$BASE_DIR"/node*/ 2>/dev/null | wc -l)"
    ts="$(cat "$GENESIS_TS_FILE")"
    pidf="$(node_pid_f "$idx")"
    if [[ -f "$pidf" ]]; then
        kill "$(cat "$pidf")" 2>/dev/null || true
        rm -f "$pidf"
    fi
    sleep 2
    start_node "$idx" "$n" "$ts"
}

case "${1:-}" in
    start)   shift; cmd_start "$@" ;;
    stop)    cmd_stop ;;
    status)  cmd_status ;;
    restart) shift; cmd_restart "$@" ;;
    *) echo "usage: $0 {start [N]|stop|status|restart <idx>}"; exit 1 ;;
esac
