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
# Genesis must be placed so the RUN sits inside the clock bootstrap grace window
# (hone_types::CLOCK_BOOTSTRAP_GRACE_END_EPOCH = 100_000). Outside grace,
# registered_clock_nodes drops the stake-0 founder clocks this harness creates, the
# registered set is EMPTY, and every epoch recycles its whole block reward with no
# sealer credited on ANY node — the gate silently stops exercising the credit path
# it exists to test.
#
# The old hardcoded 1783191600000 was inside grace when it was written (runs landed
# at epoch ~97_900) but the epoch is derived from WALL CLOCK, so it drifts upward in
# real time. It crossed 100_000 on 2026-08-10 and runs from that point read
# "reward-path registered clock count = 0" on every node, launch and joiner alike.
# Anchoring genesis relative to now pins the run at ~epoch 99_000 permanently.
TS=${HONE_3CLOCK_GENESIS_TIMESTAMP:-$(( ($(date +%s) - 99000 * 30) * 1000 ))}
CHAIN=${HONE_3CLOCK_CHAIN_ID:-hone-passb-throwaway}
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
  sudo -n ip netns exec "${NS[$i]}" env HOME="$WORK/home-$x" HONE_ACCOUNT="$account" HONE_POSTING_KEY="$seed" HONE_CLOCK=true HONE_MINER=false HONE_WORK_GENERATOR=false HONE_SIM=false HONE_ISOLATED=true HONE_ISOLATED_BIND_IP="${IP[$i]}" HONE_STATE_SYNC_MODE="$sync_mode" HONE_STATE_SYNC_TEST_DELAY_SECS="$delay" HONE_REWARD_DEPTH=2 HONE_CHAIN_ID="$CHAIN" HONE_GENESIS_FILE="$WORK/genesis.json" HONE_GENESIS_TIMESTAMP="$TS" HONE_DATA_DIR="$dir/data" HONE_API_PORT="${API[$i]}" HONE_P2P_PORT="${P2P[$i]}" HONE_WS_PORT="${WS[$i]}" HONE_BOOTSTRAP_PEERS="$boot" HONE_TRACE_FUND_WRITES="${HONE_TRACE_FUND_WRITES:-}" "$BIN" >"$dir/run.log" 2>&1 &
  PIDS+=("$!")
}
peer_id() { grep -oE '12D3KooW[A-Za-z0-9]+' "$1" 2>/dev/null | head -1 || true; }
wait_peer() { local id log=$1; for _ in $(seq 1 60); do id=$(peer_id "$log"); [ -n "$id" ] && { echo "$id"; return; }; sleep 1; done; echo "startup_error=peer_id_missing log=$log" >&2; exit 1; }
api() { sudo -n ip netns exec "$1" curl -fsS --max-time 8 "http://127.0.0.1:$2$3"; }

# A+B form the launch cohort. C is not started until after the gap.
start 0 ""; PA=$(wait_peer "$WORK/a/run.log"); start 1 "/ip4/${IP[0]}/tcp/${P2P[0]}/p2p/$PA"
# Balance/epoch sampler — see the TIME SERIES note in the root-diff probe below.
(
  seq=0
  while sleep 15; do
    seq=$((seq+1))
    for i in 0 1 2; do
      n=$(case $i in 0) echo a;; 1) echo b;; 2) echo c;; esac)
      ep=$(api "${NS[$i]}" "${API[$i]}" /api/sync/status 2>/dev/null | jq -r '.reward_watermark' 2>/dev/null || echo NA)
      { echo "# node=$n seq=$seq watermark=$ep"
        api "${NS[$i]}" "${API[$i]}" /api/sync/snapshot 2>/dev/null \
          | jq -r '.balances[] | "\(.account)\t\(.token)\t\(.amount)"' 2>/dev/null | sort
      } > "$WORK/series-$(printf '%03d' $seq)-$n.tsv" 2>/dev/null || true
    done
  done
) & SAMPLER_PID=$!; PIDS+=("$SAMPLER_PID")
echo "phase=ab_seal seconds=$AB_SECS"; sleep "$AB_SECS"; sleep 10
ROOT_A_TARGET=$(api "${NS[0]}" "${API[0]}" /api/chain/state_root); ROOT_B_TARGET=$(api "${NS[1]}" "${API[1]}" /api/chain/state_root)
echo "target_root_a=$ROOT_A_TARGET"; echo "target_root_b=$ROOT_B_TARGET"; echo "target_root_ab_equal=$( [ "$ROOT_A_TARGET" = "$ROOT_B_TARGET" ] && echo true || echo false )"

echo "phase=c_rejoin seconds=$REJOIN_SECS"; start 2 "/ip4/${IP[0]}/tcp/${P2P[0]}/p2p/$PA"; sleep 12
echo "c_sync_status=$(api "${NS[2]}" "${API[2]}" /api/sync/status || true)"; echo "c_refuse_lines=$(grep -iE 'refusing to seal epoch|verified catch-up|sealing enabled' "$WORK/c/run.log" 2>/dev/null | head -20 || true)"
# Sample the snapshot BOTH peers serve, back to back, at two time points. The joiner's
# agreement rule (state_sync.rs agreed_snapshot) needs two peers to match on epoch AND
# state_root AND digest simultaneously; sampling only one peer cannot tell an epoch skew
# apart from a non-deterministic digest, which are different failures. Early point first
# (while C is actively polling), matching point again after the rejoin window below.
echo "snapshot_served_by_a_early=$(api "${NS[0]}" "${API[0]}" /api/sync/snapshot | jq -c '{epoch,state_root,digest,account_count}' || true)"
echo "snapshot_served_by_b_early=$(api "${NS[1]}" "${API[1]}" /api/sync/snapshot | jq -c '{epoch,state_root,digest,account_count}' || true)"
sleep "$REJOIN_SECS"
# The +12s grep above fires while C is still inside HONE_STATE_SYNC_TEST_DELAY_SECS
# (60s), so catch-up has not even been attempted yet and no seal has been refused —
# that early sample is why Pass-B could not assert refuse-to-seal. Re-sample the same
# keys after the rejoin window, when the refusal and the completion have both had time
# to be logged. The early keys are kept as-is so existing parsers keep working.
echo "c_sync_status_final=$(api "${NS[2]}" "${API[2]}" /api/sync/status || true)"
echo "c_refuse_lines_final=$(grep -iE 'refusing to seal epoch|verified catch-up|sealing enabled|refusing import' "$WORK/c/run.log" 2>/dev/null | head -40 || true)"
echo "c_refuse_count_final=$(grep -icE 'refusing to seal epoch' "$WORK/c/run.log" 2>/dev/null || echo 0)"
echo "c_snapshot_epoch_served_by_a=$(api "${NS[0]}" "${API[0]}" /api/sync/snapshot | jq -c '{epoch,state_root,digest,account_count}' || true)"
echo "snapshot_served_by_b_final=$(api "${NS[1]}" "${API[1]}" /api/sync/snapshot | jq -c '{epoch,state_root,digest,account_count}' || true)"
ROOT_A=$(api "${NS[0]}" "${API[0]}" /api/chain/state_root); ROOT_B=$(api "${NS[1]}" "${API[1]}" /api/chain/state_root); ROOT_C=$(api "${NS[2]}" "${API[2]}" /api/chain/state_root)
echo "final_root_a=$ROOT_A"; echo "final_root_b=$ROOT_B"; echo "final_root_c=$ROOT_C"; echo "final_roots_equal=$( [ "$ROOT_A" = "$ROOT_B" ] && [ "$ROOT_B" = "$ROOT_C" ] && echo true || echo false )"
# reward_watermark == highest_contiguous_rewarded epoch (main.rs) — the epoch label the
# snapshot-completeness fix (and the item-5 anchor fix) is scored against, sampled AFTER
# the post-rejoin finalization boundary so it reflects applied rewards, not just seals.
EPOCH_A=$(api "${NS[0]}" "${API[0]}" /api/sync/status | jq -r '.reward_watermark'); EPOCH_B=$(api "${NS[1]}" "${API[1]}" /api/sync/status | jq -r '.reward_watermark'); EPOCH_C=$(api "${NS[2]}" "${API[2]}" /api/sync/status | jq -r '.reward_watermark')
echo "final_epoch_a=$EPOCH_A"; echo "final_epoch_b=$EPOCH_B"; echo "final_epoch_c=$EPOCH_C"; echo "final_epochs_equal=$( [ "$EPOCH_A" = "$EPOCH_B" ] && [ "$EPOCH_B" = "$EPOCH_C" ] && echo true || echo false )"
# Per-epoch credit-vs-recycle lines for C: must show CREDIT (sealer credited N hunits),
# not "earned nothing ... recycle fund", for epochs in the rejoin window.
echo "c_reward_lines_final=$(grep -iE '\[finalize\] epoch .* (sealer .* credited|earned nothing)' "$WORK/c/run.log" 2>/dev/null | tail -40 || true)"
# C's registered-clock count as seen by the reward path (main.rs reward-path log, added
# for this gate's observability) — must read 3/3 once all three nodes are registered.
echo "c_registered_clock_lines_final=$(grep -iE 'reward-path registered clock count' "$WORK/c/run.log" 2>/dev/null | tail -20 || true)"
# Order 2c89388cfea9 item 2: a node that is not sync-ready must not PUBLISH seals.
# The pre-existing c_refuse_count_final counts the seal CONSUMER gate (main.rs:577);
# this counts the publication gate, which is the path live C actually bypassed.
echo "c_publish_refuse_count_final=$(grep -icE 'refusing to PUBLISH seal' "$WORK/c/run.log" 2>/dev/null || echo 0)"
echo "c_publish_refuse_lines_final=$(grep -iE 'refusing to PUBLISH seal' "$WORK/c/run.log" 2>/dev/null | head -5 || true)"
# Order item 1: epochs whose finalize was deferred because this node had no
# epoch_validators snapshot yet. Non-zero here during catch-up is the fix WORKING;
# non-zero at the END means an epoch never got its snapshot (would stall the walk).
echo "c_deferred_finalize_lines=$(grep -iE 'finalized by reward consensus but deferred' "$WORK/c/run.log" 2>/dev/null | tail -5 || true)"
echo "c_deferred_finalize_count=$(grep -icE 'finalized by reward consensus but deferred' "$WORK/c/run.log" 2>/dev/null || echo 0)"
# Order item 1 GREEN criterion: C must emit sealed_by-POPULATED epoch_meta for the
# post-import epochs. Read it straight out of C's store via the epoch API rather than
# inferring it from the credit lines.
# Echo the harvested set explicitly: if C emitted no finalize lines at all the loop
# body never runs, and absent c_epoch_meta_* keys would read as "not checked" rather
# than as the result they actually are.
EPOCH_PROBE_SET=$(grep -oE '\[finalize\] epoch [0-9]+' "$WORK/c/run.log" 2>/dev/null | grep -oE '[0-9]+' | sort -u | tail -6)
echo "c_epoch_meta_probe_set=$(echo $EPOCH_PROBE_SET)"
for e in $EPOCH_PROBE_SET; do
  echo "c_epoch_meta_${e}=$(api "${NS[2]}" "${API[2]}" "/api/epoch/$e" 2>/dev/null | jq -c '{epoch,finalized,quorum,sealed_by}' 2>/dev/null || echo unavailable)"
  echo "a_epoch_meta_${e}=$(api "${NS[0]}" "${API[0]}" "/api/epoch/$e" 2>/dev/null | jq -c '{epoch,finalized,quorum,sealed_by}' 2>/dev/null || echo unavailable)"
done
# ROOT-DIFF PROBE. `final_roots_equal=false` alone does not say WHICH account differs,
# and log archaeology cannot answer it: run 7 showed C applying its post-import epoch
# byte-identically to A (same sealer credits, same layer_a EMA, same emission split)
# while the roots still differed. Dump every node's full balance set and diff it.
# Deliberately a plain `diff -u`, not a join/awk pipeline: the first version of this
# probe emitted nothing at all and an empty result was indistinguishable from "no
# difference" — the same ambiguity that made final_epochs_equal useless. Row counts and
# an explicit marker below mean a failed probe can never read as a clean one.
for i in 0 1 2; do
  n=$(case $i in 0) echo a;; 1) echo b;; 2) echo c;; esac)
  api "${NS[$i]}" "${API[$i]}" /api/sync/snapshot 2>/dev/null \
    | jq -r '.balances[] | "\(.account)\t\(.token)\t\(.amount)"' 2>/dev/null \
    | sort > "$WORK/balances-$n.tsv" || true
  echo "balance_rows_$n=$(wc -l < "$WORK/balances-$n.tsv" 2>/dev/null || echo 0)"
done
if [ ! -s "$WORK/balances-a.tsv" ] || [ ! -s "$WORK/balances-c.tsv" ]; then
  echo "balance_probe=FAILED_NO_ROWS"
else
  echo "balance_probe=ok"
fi
echo "balance_diff_ab=$(diff -q "$WORK/balances-a.tsv" "$WORK/balances-b.tsv" >/dev/null 2>&1 && echo identical || echo DIFFER)"
echo "balance_diff_ac=$(diff -q "$WORK/balances-a.tsv" "$WORK/balances-c.tsv" >/dev/null 2>&1 && echo identical || echo DIFFER)"
echo "balance_dump_a<<EOF"; cat "$WORK/balances-a.tsv" 2>/dev/null; echo "EOF"
echo "balance_dump_c<<EOF"; cat "$WORK/balances-c.tsv" 2>/dev/null; echo "EOF"
# `diff` exits 1 when the files differ, and under `set -euo pipefail` that killed the
# whole script right here — every probe below (balance_series, and now the fund/escrow
# traces) never ran, in runs 8, 9 and 10 alike. The empty tail read as "not collected"
# rather than as the abort it was. Neutralize the exit status; the output is the signal.
echo "balance_diff_ac_detail<<EOF"; { diff -u "$WORK/balances-a.tsv" "$WORK/balances-c.tsv" 2>/dev/null || true; } | head -60; echo "EOF"
# TIME SERIES. The end-state dump names the differing ACCOUNT but not the EPOCH the
# divergence entered at, and the job lifecycle logs nothing to recover it from. The
# sampler below (started before the A/B phase) wrote one dump per node per tick with the
# epoch each node reported; the first tick where a and c differ names that boundary, and
# whether the delta appears at one boundary or accumulates says whether the "one epoch of
# grace benchmark fees" magnitude is causal or coincidental.
echo "balance_series<<EOF"
for f in $(ls "$WORK"/series-*.tsv 2>/dev/null | sort -t- -k2 -n); do
  seq=$(basename "$f" .tsv); echo "$seq $(md5sum < "$f" | cut -c1-12) $(head -1 "$f")"
done
echo "EOF"
# ESCROW / FUND TRACE (order 2c89388cfea9). The run-9 divergence is a single 5,000,000
# hunit __testnet_fund__ → __recycle_fund__ step on the joiner, the exact size of one
# epoch of D8 grace benchmark escrow (5 jobs × 1,000,000 max_fee, main.rs). Both nodes
# log "inference job posted" for every epoch they run, yet A's end-state balances
# reconcile to the emission log with ZERO escrow outflow. Emit the per-epoch fund trace
# from both nodes side by side so the re-gate says whether C takes an EXTRA debit or A's
# debit is overwritten by a concurrent non-atomic credit — `Store::credit`/`debit` are
# read-modify-write and this reward driver holds no `chain.write_lock`.
for n in a c; do
  echo "${n}_fund_trace<<EOF"
  grep -aoE '\[bal\] epoch [0-9]+ [a-z-]+ testnet=[0-9]+ recycle=[0-9]+' "$WORK/$n/run.log" 2>/dev/null | tail -40
  echo "EOF"
  echo "${n}_escrow_trace<<EOF"
  grep -aoE '\[escrow\] post [^ ]+ requester=[^ ]+ fee=[0-9]+ requester [0-9]+->[0-9]+ recycle->[0-9]+' "$WORK/$n/run.log" 2>/dev/null | tail -30
  echo "EOF"
  echo "${n}_escrow_count=$(grep -ac '\[escrow\] post' "$WORK/$n/run.log" 2>/dev/null || echo 0)"
done
# Keep the raw node logs. They live under $WORK, which is disposable; every prior pass
# that needed log archaeology had to re-run the whole gate to get them back.
if [ -n "${HONE_3CLOCK_ARCHIVE_DIR:-}" ]; then
  mkdir -p "$HONE_3CLOCK_ARCHIVE_DIR"
  for n in a b c; do gzip -c "$WORK/$n/run.log" > "$HONE_3CLOCK_ARCHIVE_DIR/$n.log.gz" 2>/dev/null || true; done
  cp "$WORK"/balances-*.tsv "$HONE_3CLOCK_ARCHIVE_DIR/" 2>/dev/null || true
  echo "archive_dir=$HONE_3CLOCK_ARCHIVE_DIR"
fi
echo "qdisc_final_a=$(sudo -n ip netns exec "${NS[0]}" tc -s qdisc show dev "${NV[0]}")"; echo "qdisc_final_b=$(sudo -n ip netns exec "${NS[1]}" tc -s qdisc show dev "${NV[1]}")"; echo "qdisc_final_c=$(sudo -n ip netns exec "${NS[2]}" tc -s qdisc show dev "${NV[2]}")"; echo "raw_output_complete=true"
