#!/bin/bash
# Refresh the public Gource vault data.
# Run daily via cron or launch from CI:
#   0 3 * * * /home/ubuntclaw/repos/btcpc/scripts/gource-refresh.sh

set -euo pipefail

cd "$(dirname "$0")/.."
node scripts/obsidian-publish.js
