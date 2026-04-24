# Bitcoin Proof of Compute (BTCPC)

Real work on a blockchain. Earn BTCPC by doing inference, storage, sensing, hosting, and timing. Every token is backed by useful work.

## What you'll need

| Requirement | Notes |
|-------------|-------|
| **BTCPC username** | 3-20 chars, lowercase letters/numbers/hyphens. The installer will prompt you. |
| **Node.js 20+** | Installer handles this automatically. |
| **Ollama** | Required for mining only. Installer asks before installing. |
| **~2 GB disk** | Chain data + smallest inference model (~500MB). |
| **Linux** (recommended) | macOS works. Windows: use WSL or the `.bat` installer. |

Optional (add to `.env` later):

| Key | Purpose |
|-----|---------|
| `HIVE_POSTING_KEY` | Post verified compute claims to the Hive blockchain |
| `TELEGRAM_BOT_TOKEN` + `TELEGRAM_CHAT_ID` | Get reward notifications via Telegram |
| `BTCPC_DATA_DIR` | Custom path for chain data (default: `~/btcpc/data`) |
| `BTCPC_STORAGE_DIR` + `BTCPC_STORAGE_CAPACITY_GB` | Storage-node earnings |
| `BTCPC_GNSS_HOST` | GNSS relay sensor — earns from the IoT rewards pool |

## Install

```bash
curl -fsSL https://btcpc.net/install.sh | bash
```

The installer will ask you for a username, your preferred role (miner/clock/both), and any optional keys. Everything else is automatic — Node.js, Ollama, NVIDIA drivers if available.

**Phone (Termux):**
```bash
curl -fsSL https://btcpc.net/install-termux.sh | bash
```

**Windows:** Download [btcpc-start.bat](https://btcpc.net/install) or run in WSL.

### Know what you want? Skip the prompts

```bash
curl -fsSL https://btcpc.net/install.sh | bash -s -- myusername
```

Pass your username as the first argument and the installer will only prompt for optional settings.

## Install with AI help

Don't want to touch a terminal? Open Claude, ChatGPT, or any AI assistant and paste:

> Install BTCPC on my computer. Run this command and help me through any errors:
> `curl -fsSL https://btcpc.net/install.sh | bash`
> It will ask for a username (pick any 3-20 char name with lowercase letters/numbers).
> If there are GPU/CUDA issues, fix them. If Node.js fails, try nvm.
> After install, run `node bin/btcpc-all` to start all roles.

The AI will handle everything — installation, troubleshooting, configuration, starting your node.

## Use BTCPC in your project

```javascript
const BTCPC = require('@btcpc/sdk');
const ai = new BTCPC({ apiKey: process.env.BTCPC_API_KEY });

const answer = await ai.ask({ prompt: 'Explain quantum computing' });
```

Or drop-in replace OpenAI:

```javascript
const OpenAI = require('openai');
const client = new OpenAI({
  baseURL: 'https://btcpc.net/v1',
  apiKey: process.env.BTCPC_API_KEY
});
```

Or curl:

```bash
curl -X POST https://btcpc.net/v1/chat/completions \
  -H "Authorization: Bearer btcpc_your_key" \
  -H "Content-Type: application/json" \
  -d '{"model": "auto", "messages": [{"role": "user", "content": "Hello"}]}'
```

## Get an API key

1. Create an account via [Telegram bot](https://t.me/btcpcbot) or the install wizard
2. Register your project: `POST /api/projects/register`
3. Get 1 BTCPC free from the faucet: `POST /api/faucet/claim`

## Earn BTCPC

Every device earns by doing useful work:

| Role | What it does | Hardware needed |
|------|-------------|-----------------|
| **Miner** | AI inference via Ollama | Any computer (GPU = more earnings) |
| **Clock** | Keeps epoch timing alive | Anything (phone, Pi, laptop) |
| **Storage** | Hosts files for the network | Disk space |
| **Gateway** | Relays IoT sensor data | LoRa gateway (Nebra, RAK, etc.) |
| **Sensor** | Reports real-world data | Any sensor (temp, GPS, air quality) |

Bigger stake = higher reward weight. More useful work = more earnings.

Each role runs as its own process or service boundary. On mobile, BTCPC keeps the roles logically separated in-app even when the OS constrains how many processes the app can use.

## How it works

- 30-second epochs, 42M total supply
- 6-pool rewards: 55% miners, 10% verifiers, 5% clocks, 12% storage, 8% services, 10% IoT
- Proof of Compute: every token represents real inference, storage, sensor, hosting, or timing work, not wasted energy
- Optional private authorization: high-value spends can require a second wallet or policy chain, including Bitcoin, Lightning, EVM, Solana, TON, or zkVM-backed approval
- All chain state lives on disk (no database required)
- P2P mesh network — every node is a relay

## Links

- **Website:** [btcpc.net](https://btcpc.net)
- **Telegram:** [@btcpcbot](https://t.me/btcpcbot)
- **Explorer:** [scan.btcpc.net](https://scan.btcpc.net)
- **Whitepaper:** [BTCPC_WHITEPAPER.md](docs/BTCPC_WHITEPAPER.md)
- **Roadmap:** [docs/ROADMAP.md](docs/ROADMAP.md) - living plan, updated and versioned with the code
- **SDK:** [sdk/](sdk/)

## License

MIT
