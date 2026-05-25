# BTCPC Node — Installation Guide

Setup guide for running a BTCPC node on Linux, Raspberry Pi, or WSL2 (Windows).

---

## Node roles

| Role | What it does | Requires |
|------|-------------|----------|
| **Clock** | Participates in epoch consensus, earns ClockReward | Any machine |
| **Worker** | Bids on and runs inference jobs, earns job fees | Ollama + a model |
| **Miner** | Wins MineReward at epoch seal | GPU (CUDA) |

Set any combination via environment variables. Clock-only is the simplest starting point.

---

## 1. Get the binary

### Option A — Build from source (recommended)

```bash
# x86_64 (standard Linux, WSL2)
cd rust/btcpc-node
cargo build --release
sudo cp ../../target/release/btcpc-node /usr/local/bin/btcpc-node
sudo chmod +x /usr/local/bin/btcpc-node

# aarch64 (Raspberry Pi) — run on your build machine
CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc \
  cargo build --release --target aarch64-unknown-linux-gnu
# then scp the binary to the Pi
scp target/aarch64-unknown-linux-gnu/release/btcpc-node pi@<pi-ip>:/tmp/btcpc-node-new
ssh pi@<pi-ip> "sudo cp /tmp/btcpc-node-new /usr/local/bin/btcpc-node && sudo chmod +x /usr/local/bin/btcpc-node"
```

### Option B — Check for a pre-built release

Pre-built binaries for x86_64 and aarch64 are published on the GitHub releases page when available. Download, chmod +x, and move to `/usr/local/bin/`.

---

## 2. Get the genesis file

```bash
# Copy from the repo to a stable location on the target machine
cp rust/btcpc-node/genesis.json ~/genesis.json

# On a remote machine (Pi, WSL, etc.)
scp rust/btcpc-node/genesis.json user@host:~/genesis.json
```

---

## 3. Create an account and posting key

On first start, the node generates a wallet automatically and saves it to `~/.btcpc/wallet.key`. To use an existing account, set `BTCPC_ACCOUNT` and `BTCPC_POSTING_KEY` before the first start.

If starting fresh:
```bash
mkdir -p ~/.btcpc
# Start once to generate the wallet, then stop it
BTCPC_ACCOUNT=yourname BTCPC_GENESIS_FILE=~/genesis.json \
  BTCPC_GENESIS_TIMESTAMP=1777672500000 btcpc-node &
sleep 3 && kill %1
# Read your posting key
cat ~/.btcpc/wallet.key | python3 -c "import sys,json; d=json.load(sys.stdin); print('private:', d['btcpc_private_key'])"
```

---

## 4. Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `BTCPC_ACCOUNT` | `genesis` | Your username on the chain |
| `BTCPC_POSTING_KEY` | *(auto)* | Ed25519 private key hex (32 bytes) |
| `BTCPC_GENESIS_FILE` | *(required)* | Path to genesis.json |
| `BTCPC_GENESIS_TIMESTAMP` | *(required)* | `1777672500000` |
| `BTCPC_CLOCK` | `false` | Enable clock consensus participation |
| `BTCPC_WORKER` | `false` | Enable inference worker mode |
| `OLLAMA_URL` | `http://localhost:11434` | Ollama endpoint for worker mode |
| `BTCPC_MODEL` | `qwen2.5:0.5b` | Model name to bid on jobs with |
| `BTCPC_API_PORT` | `4242` | HTTP API port |
| `BTCPC_P2P_PORT` | `6942` | libp2p gossipsub P2P port |
| `RUST_LOG` | `warn` | Log level (`info` recommended) |

---

## 5. Systemd service

### User service (regular Linux, WSL2)

```bash
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/btcpc-node.service << 'EOF'
[Unit]
Description=BTCPC Node
After=network.target

[Service]
Type=simple
Restart=always
RestartSec=5
ExecStart=/usr/local/bin/btcpc-node
Environment="BTCPC_ACCOUNT=yourname"
Environment="BTCPC_POSTING_KEY=<your-ed25519-private-key-hex>"
Environment="BTCPC_GENESIS_TIMESTAMP=1777672500000"
Environment="BTCPC_GENESIS_FILE=/home/yourname/genesis.json"
Environment="BTCPC_CLOCK=true"
Environment="BTCPC_WORKER=true"
Environment="OLLAMA_URL=http://localhost:11434"
Environment="BTCPC_MODEL=qwen2.5:0.5b"
Environment="BTCPC_API_PORT=4242"
Environment="BTCPC_P2P_PORT=6942"
Environment="RUST_LOG=info"

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable btcpc-node
systemctl --user start btcpc-node
```

### System service (Raspberry Pi, dedicated server)

Same `[Service]` block, saved to `/etc/systemd/system/btcpc-node.service`. Replace `yourname` with the machine user (e.g. `pi`). Update `BTCPC_GENESIS_FILE` path accordingly.

```bash
sudo systemctl daemon-reload
sudo systemctl enable btcpc-node
sudo systemctl start btcpc-node
```

---

## 6. Verify

```bash
# API health check
curl http://localhost:4242/api/node/info

# Live logs
journalctl --user -u btcpc-node -f          # user service
journalctl -u btcpc-node -f                 # system service
```

A healthy node shows `"peer_count": N` (N ≥ 1) and an `"epoch"` that matches the network.

---

## WSL2 notes

**Enable systemd in WSL2** — add to `/etc/wsl.conf`:
```ini
[boot]
systemd=true
```
Restart WSL (`wsl --shutdown` from PowerShell, then reopen).

**Ollama on Windows** — WSL2 cannot reach `localhost:11434` when Ollama runs on Windows. Use the WSL gateway IP instead:
```bash
# Find the gateway IP
ip route | grep default | awk '{print $3}'
# e.g. 172.26.16.1

# Set in the service
Environment="OLLAMA_URL=http://172.26.16.1:11434"
```

**Port already in use** — if `btcpc-node` fails with "Address already in use", a stale process may be holding the port:
```bash
sudo ss -tlnp | grep 4242
# Find the pid, then:
sudo kill <pid>
systemctl --user restart btcpc-node
```

**WSL gateway IP can change** — if `OLLAMA_URL` stops working after a Windows reboot, re-run the `ip route` check and update the service file.

---

## Common mistakes

**Wrong account after changing BTCPC_ACCOUNT** — the hardware fingerprint is stamped on first run. If you change the account, you must wipe the data directory and start fresh:
```bash
systemctl --user stop btcpc-node
rm -rf ~/.btcpc
systemctl --user start btcpc-node
```

**Old binary** — if `/api/node/info` returns 404, the installed binary is outdated. Rebuild and reinstall from source.

**No peers** — the node refuses to accept entries when `peer_count == 0`. This is intentional: a disconnected node cannot submit to the network. Wait for at least one peer before submitting transactions. Bootstrap peers are resolved via DNS (`bootstrap1.btcpc.net`, `bootstrap2.btcpc.net`).

**Genesis mismatch** — if you started the node without `BTCPC_GENESIS_FILE`, it may have created a local genesis that doesn't match the network. Wipe `~/.btcpc` and restart with the correct genesis file.
