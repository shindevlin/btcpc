#!/usr/bin/env bash
# Disposable Phase-4 Pass-B state-sync gate. Branch-only; namespace/netem isolated.
set -euo pipefail

need() { command -v "$1" >/dev/null || { echo "missing command: $1" >&2; exit 2; }; }
for c in bash curl grep ip jq mktemp openssl python3 tc; do need "$c"; done
sudo -n id -u >/dev/null || { echo "sudo -n is required" >&2; exit 2; }
[ "$(git rev-parse --abbrev-ref HEAD 2>/dev/null || true)" = "feat/statesync-catchup" ] || { echo "wrong branch" >&2; exit 2; }
ROOT=$(git rev-parse --show-toplevel)
BIN=${HONE_3CLOCK_BIN:-$ROOT/rust/target/release/hone-node}
[ -x "$BIN" ] || { echo "binary not executable: $BIN" >&2; exit 2; }
DELAY=${HONE_3CLOCK_NETEM_DELAY:-150ms}; LOSS=${HONE_3CLOCK_NETEM_LOSS:-2%}
AB_SECS=${HONE_3CLOCK_AB_SECS:-105}; REJOIN_SECS=${HONE_3CLOCK_REJOIN_SECS:-90}
TS=${HONE_3CLOCK_GENESIS_TIMESTAMP:-1783191600000}; CHAIN=${HONE_3CLOCK_CHAIN_ID:-hone-passb-throwaway}
RUN_ID="pb$$"; WORK=$(mktemp -d "/tmp/hone-passb-${RUN_ID}.XXXXXX"); BR="${RUN_ID}-br"
NS=("${RUN_ID}-a" "${RUN_ID}-b" "${RUN_ID}-c"); HV=("${RUN_ID}-ha" "${RUN_ID}-hb" "${RUN_ID}-hc"); NV=("${RUN_ID}-na" "${RUN_ID}-nb" "${RUN_ID}-nc")
IP=(10.77.31.11 10.77.31.12 10.77.31.13); API=(4242 4242 4242); P2P=(6953 6954 6955); WS=(4953 4954 4955); PIDS=()

cleanup() { set +e; for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null; sudo -n kill "$pid" 2>/dev/null; done; for pid in "${PIDS[@]:-}"; do wait "$pid" 2>/dev/null; done; for n in "${NS[@]}"; do sudo -n ip netns del "$n" 2>/dev/null; done; sudo -n ip link del "$BR" 2>/dev/null; echo "work_dir=$WORK"; echo "cleanup=complete"; }
trap cleanup EXIT INT TERM

mkdir -p "$WORK"/home-a "$WORK"/home-b "$WORK"/home-c "$WORK"/seed-a "$WORK"/seed-b "$WORK"/seed-c
python3 - "$WORK/bootstrap-genesis.json" "$CHAIN" "$TS" <<'PY'
import json,sys
out,chain,ts=sys.argv[1:]
json.dump({'genesis_timestamp':int(ts),'chain_id':chain,'accounts':{'__treasury__':{},'__recycle_fund__':{},'__testnet_fund__':{}}},open(out,'w'),indent=2)
PY

# Generate disposable wallets in disposable HOME/data dirs. Seeds are never printed.
for x in a b c; do
  seed=$(openssl rand -hex 32); export "SEED_${x^^}=$seed"; account="clock${x}"
  timeout 8s env HOME="$WORK/home-$x" HONE_ACCOUNT="$account" HONE_POSTING_KEY="$seed" HONE_CLOCK=false HONE_MINER=false HONE_ISOLATED=true HONE_CHAIN_ID="$CHAIN" HONE_GENESIS_FILE="$WORK/bootstrap-genesis.json" HONE_GENESIS_TIMESTAMP="$TS" HONE_DATA_DIR="$WORK/seed-$x" HONE_API_PORT="$((4400 + $(printf '%d' "'${x}")))" HONE_P2P_PORT="$((7400 + $(printf '%d' "'${x}")))" "$BIN" >"$WORK/keygen-$x.log" 2>&1 || true
done
python3 - "$WORK" "$CHAIN" "$TS" <<'PY'
import json,sys
w,chain,ts=sys.argv[1:]; accounts={}
for x in 'abc':
    d=json.load(open(f'{w}/seed-{x}/wallet.key')); accounts[f'clock{x}']={'keys':{'posting':d['hone_public_key']}}
accounts.update({'__treasury__':{},'__recycle_fund__':{},'__testnet_fund__':{}})
json.dump({'genesis_timestamp':int(ts),'chain_id':chain,'accounts':accounts},open(f'{w}/genesis.json','w'),indent=2)
PY
echo "throwaway_genesis=$WORK/genesis.json"; echo "netem_delay=$DELAY netem_loss=$LOSS"

sudo -n ip link add "$BR" type bridge; sudo -n ip addr add 10.77.31.1/24 dev "$BR"; sudo -n ip link set "$BR" up
for i in 0 1 2; do
  sudo -n ip netns add "${NS[$i]}"; sudo -n ip link add "${HV[$i]}" type veth peer name "${NV[$i]}"; sudo -n ip link set "${HV[$i]}" master "$BR"; sudo -n ip link set "${HV[$i]}" up; sudo -n ip link set "${NV[$i]}" netns "${NS[$i]}"
  sudo -n ip -n "${NS[$i]}" link set lo up; sudo -n ip -n "${NS[$i]}" addr add "${IP[$i]}/24" dev "${NV[$i]}"; sudo -n ip -n "${NS[$i]}" link set "${NV[$i]}" up
  sudo -n ip netns exec "${NS[$i]}" tc qdisc add dev "${NV[$i]}" root netem delay "$DELAY" loss "$LOSS"
  echo "qdisc_${i}=$(sudo -n ip netns exec "${NS[$i]}" tc qdisc show dev "${NV[$i]}")"
done

start() {
  local i=$1 boot=$2 x account dir seed_var seed
  case "$i" in 0) x=a;; 1) x=b;; 2) x=c;; *) echo "bad node index" >&2; exit 2;; esac
  account="clock$x"; dir="$WORK/$x"; mkdir -p "$dir"; seed_var="SEED_${x^^}"; seed=${!seed_var}
  local sync_mode=launch delay=0; [ "$x" = c ] && sync_mode=late-joiner && delay=60
  sudo -n ip netns exec "${NS[$i]}" env HOME="$WORK/home-$x" HONE_ACCOUNT="$account" HONE_POSTING_KEY="$seed" HONE_CLOCK=true HONE_MINER=false HONE_WORK_GENERATOR=false HONE_SIM=false HONE_ISOLATED=true HONE_ISOLATED_BIND_IP="${IP[$i]}" HONE_STATE_SYNC_MODE="$sync_mode" HONE_STATE_SYNC_TEST_DELAY_SECS="$delay" HONE_REWARD_DEPTH=2 HONE_CHAIN_ID="$CHAIN" HONE_GENESIS_FILE="$WORK/genesis.json" HONE_GENESIS_TIMESTAMP="$TS" HONE_DATA_DIR="$dir/data" HONE_API_PORT="${API[$i]}" HONE_P2P_PORT="${P2P[$i]}" HONE_WS_PORT="${WS[$i]}" HONE_BOOTSTRAP_PEERS="$boot" "$BIN" >"$dir/run.log" 2>&1 &
  PIDS+=("$!")
}
peer_id() { grep -oE '12D3KooW[A-Za-z0-9]+' "$1" 2>/dev/null | head -1 || true; }
wait_peer() { local id log=$1; for _ in $(seq 1 60); do id=$(peer_id "$log"); [ -n "$id" ] && { echo "$id"; return; }; sleep 1; done; echo "startup_error=peer_id_missing log=$log" >&2; exit 1; }
api() { sudo -n ip netns exec "$1" curl -fsS --max-time 8 "http://127.0.0.1:$2$3"; }

# A+B form the launch cohort. C is not started until after the gap.
start 0 ""; PA=$(wait_peer "$WORK/a/run.log"); start 1 "/ip4/${IP[0]}/tcp/${P2P[0]}/p2p/$PA"
echo "phase=ab_seal seconds=$AB_SECS"; sleep "$AB_SECS"; sleep 10
ROOT_A_TARGET=$(api "${NS[0]}" "${API[0]}" /api/chain/state_root); ROOT_B_TARGET=$(api "${NS[1]}" "${API[1]}" /api/chain/state_root)
echo "target_root_a=$ROOT_A_TARGET"; echo "target_root_b=$ROOT_B_TARGET"; echo "target_root_ab_equal=$( [ "$ROOT_A_TARGET" = "$ROOT_B_TARGET" ] && echo true || echo false )"

echo "phase=c_rejoin seconds=$REJOIN_SECS"; start 2 "/ip4/${IP[0]}/tcp/${P2P[0]}/p2p/$PA"; sleep 12
echo "c_sync_status=$(api "${NS[2]}" "${API[2]}" /api/sync/status || true)"; echo "c_refuse_lines=$(grep -iE 'refusing to seal epoch|verified catch-up|sealing enabled' "$WORK/c/run.log" 2>/dev/null | head -20 || true)"; sleep "$REJOIN_SECS"
ROOT_A=$(api "${NS[0]}" "${API[0]}" /api/chain/state_root); ROOT_B=$(api "${NS[1]}" "${API[1]}" /api/chain/state_root); ROOT_C=$(api "${NS[2]}" "${API[2]}" /api/chain/state_root)
echo "final_root_a=$ROOT_A"; echo "final_root_b=$ROOT_B"; echo "final_root_c=$ROOT_C"; echo "final_roots_equal=$( [ "$ROOT_A" = "$ROOT_B" ] && [ "$ROOT_B" = "$ROOT_C" ] && echo true || echo false )"
echo "qdisc_final_a=$(sudo -n ip netns exec "${NS[0]}" tc -s qdisc show dev "${NV[0]}")"; echo "qdisc_final_b=$(sudo -n ip netns exec "${NS[1]}" tc -s qdisc show dev "${NV[1]}")"; echo "qdisc_final_c=$(sudo -n ip netns exec "${NS[2]}" tc -s qdisc show dev "${NV[2]}")"; echo "raw_output_complete=true"
