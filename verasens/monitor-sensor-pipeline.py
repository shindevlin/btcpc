#!/usr/bin/env python3
"""
Verasens sensor pipeline monitor.

Watches for SensorDataCommit -> SensorReward (and GatewayRewardSplit) events
on the BTCPC node at NODE_URL.  Polls /api/explorer/activity every epoch
and checks sealed blocks for raw sensor entries.

Usage:
    python3 monitor-sensor-pipeline.py [--node http://localhost:4242] [--interval 30]

To submit a test SensorDataCommit (requires PyNaCl: pip install PyNaCl):
    python3 monitor-sensor-pipeline.py --submit-test --posting-key <64-char-hex-seed>

How to sign a SensorDataCommit manually (all other tools):
    See sign_sensor_commit() below — canonical message is compact JSON with keys
    sorted ALPHABETICALLY (serde_json BTreeMap default, no preserve_order feature):
      {"batch_hash":"...","chain_id":"btcpc-2","owner":"...",
       "reading_count":N,"sensor_id":"...","sensor_type":"...",
       "signed_by":"...","type":"SENSOR_DATA_COMMIT"}
    Sign with ed25519 (posting key seed), hex-encode the 64-byte signature.

Curl commands (node at http://localhost:4242):

  # Node info
  curl -s http://localhost:4242/api/node/info | python3 -m json.tool

  # Register a sensor (signature required if posting key is set on account):
  curl -s -X POST http://localhost:4242/api/sensor/register \
    -H 'Content-Type: application/json' \
    -d '{"sensor_id":"<account>/mydevice","owner":"<account>",
         "sensor_type":"sampled","location":null,"metadata":null,
         "signature":"<128-char-hex>"}'

  # Submit a SensorDataCommit:
  curl -s -X POST http://localhost:4242/api/sensor/commit \
    -H 'Content-Type: application/json' \
    -d '{"sensor_id":"<account>/mydevice","owner":"<account>",
         "batch_hash":"<64-char-sha256-hex>","reading_count":1,
         "sensor_type":"sampled","value":42.0,
         "signature":"<128-char-hex>"}'

  # Poll activity feed for recent sensor events:
  curl -s http://localhost:4242/api/explorer/activity | \
    python3 -c "import sys,json; [print(e) for e in json.load(sys.stdin).get('entries',[]) \
    if any(x in e.get('type','') for x in ('SENSOR','Sensor','GATEWAY'))]"

  # Check a sealed block for sensor entries:
  curl -s http://localhost:4242/api/block/<epoch> | python3 -m json.tool
"""

import sys
import time
import json
import hashlib
import argparse
import urllib.request
import urllib.error
from datetime import datetime, timezone


# ── Signing (optional: requires PyNaCl) ────────────────────────────────────

def _try_import_nacl():
    try:
        import nacl.signing
        return nacl.signing
    except ImportError:
        return None


def sign_sensor_commit(
    chain_id: str,
    sensor_id: str,
    owner: str,
    batch_hash: str,
    reading_count: int,
    sensor_type: str,
    posting_key_hex: str,
) -> str:
    """
    Sign a SensorDataCommit canonical message with an ed25519 posting key seed.
    Returns 128-char hex signature.

    Canonical message (keys sorted alphabetically, compact JSON no spaces):
      {"batch_hash":"...","chain_id":"...","owner":"...",
       "reading_count":N,"sensor_id":"...","sensor_type":"...",
       "signed_by":"...","type":"SENSOR_DATA_COMMIT"}

    Requires: pip install PyNaCl
    """
    nacl_signing = _try_import_nacl()
    if nacl_signing is None:
        raise ImportError("PyNaCl not installed — run: pip install PyNaCl")

    seed = bytes.fromhex(posting_key_hex)
    sk = nacl_signing.SigningKey(seed)

    # Build canonical message — serde_json::json! uses BTreeMap (alphabetical key order)
    # since btcpc-node has no serde_json preserve_order feature.  sort_keys=True matches.
    msg = json.dumps({
        "chain_id": chain_id,
        "type": "SENSOR_DATA_COMMIT",
        "sensor_id": sensor_id,
        "owner": owner,
        "batch_hash": batch_hash,
        "reading_count": reading_count,
        "sensor_type": sensor_type,
        "signed_by": owner,
    }, separators=(",", ":"), sort_keys=True)

    signed = sk.sign(msg.encode())
    # nacl returns signature + message concatenated; first 64 bytes are the sig
    return signed.signature.hex()


def make_batch_hash(sensor_id: str, epoch: int, readings: list) -> str:
    """SHA-256 of the batch JSON (same as what the device stores off-chain)."""
    batch = json.dumps({
        "sensor_id": sensor_id,
        "epoch": epoch,
        "readings": readings,
    }, separators=(",", ":"), sort_keys=True)
    return hashlib.sha256(batch.encode()).hexdigest()


# ── HTTP helpers ────────────────────────────────────────────────────────────

def ts() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def fetch(url: str, timeout: int = 10):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        if e.code != 404:
            print(f"[{ts()}] HTTP {e.code} fetching {url}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"[{ts()}] Error fetching {url}: {e}", file=sys.stderr)
        return None


def post(url: str, payload: dict, timeout: int = 10):
    body = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=body,
                                  headers={"Content-Type": "application/json"},
                                  method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        body = e.read()
        print(f"[{ts()}] HTTP {e.code} posting to {url}: {body.decode()[:200]}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"[{ts()}] Error posting to {url}: {e}", file=sys.stderr)
        return None


# ── Live API helpers ────────────────────────────────────────────────────────

def get_node_info(node: str) -> dict:
    return fetch(f"{node}/api/node/info") or {}


def get_balance(node: str, account: str) -> dict:
    return fetch(f"{node}/api/balance/{account}") or {}


def get_mempool_status(node: str) -> dict:
    return fetch(f"{node}/api/mempool/status") or {}


def scan_activity_for_sensor(node: str) -> list:
    """
    Poll /api/explorer/activity and return sensor-related entries.
    This endpoint returns the ~20 most recent system entries (rewards, seals).
    """
    data = fetch(f"{node}/api/explorer/activity")
    if not data:
        return []
    entries = data.get("entries", data) if isinstance(data, dict) else data
    if not isinstance(entries, list):
        return []
    return [
        e for e in entries
        if isinstance(e, dict) and any(
            x in e.get("type", "")
            for x in ("SENSOR", "Sensor", "GATEWAY_REWARD", "GatewayReward")
        )
    ]


def scan_block_for_sensor(node: str, epoch: int) -> list:
    """
    Fetch /api/block/:epoch and return sensor-related entries from the payload.
    Falls back to empty list on 404 (epoch not yet written to block store).
    """
    data = fetch(f"{node}/api/block/{epoch}")
    if not data:
        return []
    payload = data.get("payload", {})
    entries = payload.get("entries", []) if isinstance(payload, dict) else []
    return [
        e for e in entries
        if isinstance(e, dict) and any(
            x in e.get("type", "")
            for x in ("SENSOR", "Sensor", "GATEWAY_REWARD", "GatewayReward")
        )
    ]


# ── Sensor type rotation (Phase 1.2 — universal ingest test) ────────────────

# One type per epoch, cycled in order. Each type exercises a different
# metadata schema so the generic ingest path is validated for every class.
SENSOR_ROTATE_TYPES = [
    ("gnss",      {"lat": 37.7749, "lon": -122.4194, "alt": 10.0, "accuracy": 5.0}),
    ("subghz",    {"freq_hz": 433920000, "rssi": -72.5, "modulation": "AM270"}),
    ("heartbeat", {"uptime_s": 3600, "battery_pct": 85, "fw": "1.2.0"}),
    ("sampled",   {"value": 42.0, "unit": "raw"}),
    ("rfid",      {"protocol": "EM4100", "obs_id": "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4"}),
    ("nfc",       {"protocol": "ISO14443A", "obs_id": "b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5"}),
    ("ibutton",   {"protocol": "DS1990A", "obs_id": "c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6"}),
    ("coverage",  {"mcc": 310, "mnc": 260, "rsrp": -95, "lat": 37.7749, "lon": -122.4194}),
]


def next_rotate_type(epoch: int) -> tuple:
    """Return (sensor_type, metadata) for this epoch, cycling in order."""
    return SENSOR_ROTATE_TYPES[epoch % len(SENSOR_ROTATE_TYPES)]


def submit_rotate_commit(node: str, account: str, posting_key_hex: str) -> dict | None:
    """
    Submit one SensorDataCommit for the current epoch, using the sensor type
    that corresponds to (epoch % len(SENSOR_ROTATE_TYPES)).

    Run once per epoch to cycle through all 8 sensor types in ~4 minutes.
    """
    info = get_node_info(node)
    epoch = info.get("epoch", 0)
    chain_id = info.get("chain_id", "btcpc-2")
    sensor_type, metadata = next_rotate_type(epoch)
    sensor_id = f"{account}/{sensor_type}-test"

    # Register this sensor type (idempotent)
    nacl_signing = _try_import_nacl()
    if nacl_signing and posting_key_hex:
        seed = bytes.fromhex(posting_key_hex)
        sk = nacl_signing.SigningKey(seed)
        reg_msg = json.dumps({
            "chain_id": chain_id, "type": "SENSOR_REGISTER",
            "sensor_id": sensor_id, "owner": account,
            "sensor_type": sensor_type, "location": None, "signed_by": account,
        }, separators=(",", ":"), sort_keys=True)
        reg_sig = sk.sign(reg_msg.encode()).signature.hex()
    else:
        reg_sig = ""

    post(f"{node}/api/sensor/register", {
        "sensor_id": sensor_id, "owner": account,
        "sensor_type": sensor_type, "location": None,
        "metadata": metadata, "signature": reg_sig,
    })

    readings = [{"epoch": epoch, "ts": int(time.time() * 1000), **metadata}]
    batch_hash = make_batch_hash(sensor_id, epoch, readings)
    sig = sign_sensor_commit(
        chain_id=chain_id, sensor_id=sensor_id, owner=account,
        batch_hash=batch_hash, reading_count=1,
        sensor_type=sensor_type, posting_key_hex=posting_key_hex,
    )

    print(f"[{ts()}] rotate epoch={epoch} type={sensor_type} ({epoch % len(SENSOR_ROTATE_TYPES) + 1}/{len(SENSOR_ROTATE_TYPES)})")
    resp = post(f"{node}/api/sensor/commit", {
        "sensor_id": sensor_id, "owner": account,
        "batch_hash": batch_hash, "reading_count": 1,
        "sensor_type": sensor_type, "value": None, "signature": sig,
    })
    print(f"[{ts()}] response: {json.dumps(resp)}")
    return resp


# ── Submit test entry ───────────────────────────────────────────────────────

def submit_test_commit(node: str, account: str, posting_key_hex: str) -> dict | None:
    """
    Register a test sensor (if not already registered) then submit one
    SensorDataCommit for the current epoch.  Returns the API response.

    Sensor ID: "<account>/test"
    Sensor type: "sampled"
    batch_hash: SHA-256 of a minimal batch JSON
    reading_count: 1
    value: 42.0  (representative numeric value for cross-validation)
    """
    info = get_node_info(node)
    epoch = info.get("epoch", 0)
    chain_id = info.get("chain_id", "btcpc-2")
    sensor_id = f"{account}/test"

    # Step 1: Register sensor (idempotent — node stores in meta, not re-applied)
    reg_sig = sign_sensor_commit(
        chain_id=chain_id,
        sensor_id=sensor_id,
        owner=account,
        batch_hash="0" * 64,  # unused for register, but need a valid call
        reading_count=0,
        sensor_type="sampled",
        posting_key_hex=posting_key_hex,
    ) if posting_key_hex else ""

    # SensorRegister canonical message differs — build separately
    nacl_signing = _try_import_nacl()
    if nacl_signing and posting_key_hex:
        seed = bytes.fromhex(posting_key_hex)
        sk = nacl_signing.SigningKey(seed)
        # canonical_signing_message() for SensorRegister includes:
        # chain_id, type, sensor_id, owner, sensor_type, location, signed_by
        # Note: metadata is NOT part of the signed message (excluded in tx.rs).
        reg_msg = json.dumps({
            "chain_id": chain_id,
            "type": "SENSOR_REGISTER",
            "sensor_id": sensor_id,
            "owner": account,
            "sensor_type": "sampled",
            "location": None,
            "signed_by": account,
        }, separators=(",", ":"), sort_keys=True)
        reg_sig = sk.sign(reg_msg.encode()).signature.hex()
    else:
        reg_sig = ""

    reg_resp = post(f"{node}/api/sensor/register", {
        "sensor_id": sensor_id,
        "owner": account,
        "sensor_type": "sampled",
        "location": None,
        "metadata": None,
        "signature": reg_sig,
    })
    print(f"[{ts()}] SensorRegister response: {json.dumps(reg_resp)}")

    # Step 2: Build batch hash from a minimal reading set
    readings = [{"epoch": epoch, "value": 42.0, "ts": int(time.time() * 1000)}]
    batch_hash = make_batch_hash(sensor_id, epoch, readings)

    # Step 3: Sign the commit
    sig = sign_sensor_commit(
        chain_id=chain_id,
        sensor_id=sensor_id,
        owner=account,
        batch_hash=batch_hash,
        reading_count=1,
        sensor_type="sampled",
        posting_key_hex=posting_key_hex,
    )

    print(f"[{ts()}] Submitting SensorDataCommit: sensor={sensor_id} epoch={epoch} "
          f"batch_hash={batch_hash[:12]}... sig={sig[:12]}...")

    resp = post(f"{node}/api/sensor/commit", {
        "sensor_id": sensor_id,
        "owner": account,
        "batch_hash": batch_hash,
        "reading_count": 1,
        "sensor_type": "sampled",
        "value": 42.0,
        "signature": sig,
    })
    print(f"[{ts()}] SensorDataCommit response: {json.dumps(resp)}")
    return resp


# ── Main monitor loop ───────────────────────────────────────────────────────

def monitor(node: str, interval: int):
    print(f"[{ts()}] Verasens sensor pipeline monitor")
    print(f"[{ts()}] Node: {node}  Poll interval: {interval}s")

    info = get_node_info(node)
    account = info.get("account", "natoshisakamoto")
    print(f"[{ts()}] chain={info.get('chain_id')} epoch={info.get('epoch')} "
          f"peers={info.get('peer_count')} account={account}")
    print()

    last_epoch_seen = info.get("epoch", 0)
    seen_event_keys: set = set()

    while True:
        info = get_node_info(node)
        current_epoch = info.get("epoch", last_epoch_seen)
        account = info.get("account", account)

        # ── Activity feed: fast path for recent entries ──────────────────
        for entry in scan_activity_for_sensor(node):
            key = (entry.get("type"), entry.get("epoch") or entry.get("_epoch"),
                   entry.get("sensor_id") or entry.get("node_id") or entry.get("sensor_account"))
            if key in seen_event_keys:
                continue
            seen_event_keys.add(key)
            t = entry.get("type", "")
            ep = entry.get("epoch") or entry.get("_epoch", "?")

            if "COMMIT" in t.upper() or "Commit" in t:
                print(f"[{ts()}] SENSOR_DATA_COMMIT  epoch={ep}  "
                      f"sensor={entry.get('sensor_id','?')}  "
                      f"owner={entry.get('owner','?')}  "
                      f"readings={entry.get('reading_count','?')}  "
                      f"type={entry.get('sensor_type','?')}")

            elif "SENSOR_REWARD" in t.upper() or t in ("SensorReward", "SENSOR_REWARD"):
                dreams = entry.get("amount", 0)
                btcpc  = dreams / 10_000_000_000
                print(f"[{ts()}] SENSOR_REWARD        epoch={ep}  "
                      f"node={entry.get('node_id','?')}  "
                      f"amount={btcpc:.6f} BTCPC  ({dreams} dreams)")

            elif "GATEWAY" in t.upper():
                print(f"[{ts()}] GATEWAY_REWARD_SPLIT epoch={ep}  "
                      f"sensor={entry.get('sensor_account','?')}  "
                      f"gateway={entry.get('gateway_account','?')}  "
                      f"sensor_amt={entry.get('sensor_amount',0)} dreams  "
                      f"gateway_amt={entry.get('gateway_amount',0)} dreams")

        # ── Block scan for each newly sealed epoch ───────────────────────
        if current_epoch > last_epoch_seen:
            for ep in range(last_epoch_seen + 1, current_epoch + 1):
                block_entries = scan_block_for_sensor(node, ep)
                commits = [e for e in block_entries
                           if "COMMIT" in e.get("type","").upper() or "Commit" in e.get("type","")]
                rewards = [e for e in block_entries
                           if "REWARD" in e.get("type","").upper() or "Reward" in e.get("type","")]
                if commits or rewards:
                    print(f"[{ts()}] === Epoch {ep}: "
                          f"{len(commits)} commit(s), {len(rewards)} reward(s) ===")
                    for e in commits + rewards:
                        print(f"  {json.dumps(e)}")
                else:
                    print(f"[{ts()}] Epoch {ep} sealed — no sensor activity")
            last_epoch_seen = current_epoch

        # ── Periodic status line ─────────────────────────────────────────
        mpool = get_mempool_status(node)
        bal   = get_balance(node, account)
        print(f"[{ts()}] epoch={current_epoch}  peers={info.get('peer_count',0)}  "
              f"mempool={mpool.get('pending_count',0)}  "
              f"balance={bal.get('balance','?')} BTCPC")

        time.sleep(interval)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verasens sensor pipeline monitor")
    parser.add_argument("--node",        default="http://localhost:4242")
    parser.add_argument("--interval",    type=int, default=30,
                        help="Poll interval in seconds")
    parser.add_argument("--submit-test", action="store_true",
                        help="Submit one test SensorDataCommit (sampled type) then exit")
    parser.add_argument("--rotate-test", action="store_true",
                        help="Submit one SensorDataCommit for this epoch's type in the "
                             "rotation cycle (gnss→subghz→heartbeat→sampled→rfid→nfc→ibutton→coverage), "
                             "then exit.  Run once per epoch to walk all 8 types.")
    parser.add_argument("--account", default="",
                        help="Override account name (default: read from node /api/node/info)")
    parser.add_argument("--posting-key", default="",
                        help="64-char hex ed25519 seed (BTCPC_POSTING_KEY) for signing")
    args = parser.parse_args()

    if args.rotate_test:
        if not args.posting_key:
            print("ERROR: --rotate-test requires --posting-key <64-char-hex-seed>",
                  file=sys.stderr)
            sys.exit(1)
        info = get_node_info(args.node)
        account = args.account or info.get("account", "natoshisakamoto")
        submit_rotate_commit(args.node, account, args.posting_key)
        sys.exit(0)

    if args.submit_test:
        if not args.posting_key:
            print("ERROR: --submit-test requires --posting-key <64-char-hex-seed>",
                  file=sys.stderr)
            sys.exit(1)
        info = get_node_info(args.node)
        account = args.account or info.get("account", "natoshisakamoto")
        submit_test_commit(args.node, account, args.posting_key)
        sys.exit(0)

    try:
        monitor(args.node, args.interval)
    except KeyboardInterrupt:
        print(f"\n[{ts()}] Stopped.")
