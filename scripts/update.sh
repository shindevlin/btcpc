#!/usr/bin/env bash
# Download the latest honemesh-node release binary and restart the service.
# Safe to run manually or via honemesh-update.timer.
#
# Usage:  sudo bash update.sh

set -euo pipefail

GITHUB_REPO="shindevlin/btcpc"
ASSET="honemesh-node-x86_64-linux"
BINARY="/usr/local/bin/honemesh-node"
SERVICE="honemesh-node"

# ── Fetch latest release tag ──────────────────────────────────────────────────
LATEST=$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases" \
    | grep -m1 '"tag_name"' \
    | grep 'node-v' \
    | sed 's/.*"tag_name": "\(.*\)".*/\1/')

if [ -z "$LATEST" ]; then
    echo "honemesh-update: could not determine latest release tag — aborting"
    exit 1
fi

# ── Compare with running binary ───────────────────────────────────────────────
CURRENT_TAG=$(cat /var/lib/honemesh/.release-tag 2>/dev/null || echo "none")
if [ "$CURRENT_TAG" = "$LATEST" ]; then
    echo "honemesh-update: already on ${LATEST}"
    exit 0
fi

echo "honemesh-update: ${CURRENT_TAG} -> ${LATEST}"

# ── Download ──────────────────────────────────────────────────────────────────
DOWNLOAD_URL="https://github.com/${GITHUB_REPO}/releases/download/${LATEST}/${ASSET}"
curl -fsSL "$DOWNLOAD_URL" -o "${BINARY}.tmp"
chmod +x "${BINARY}.tmp"
mv "${BINARY}.tmp" "$BINARY"
echo "$LATEST" > /var/lib/honemesh/.release-tag
echo "honemesh-update: binary updated to ${LATEST}"

# ── Restart ───────────────────────────────────────────────────────────────────
if systemctl is-active --quiet "$SERVICE"; then
    systemctl restart "$SERVICE"
    echo "honemesh-update: ${SERVICE} restarted"
fi
