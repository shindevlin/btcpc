# Surveillance Ledger

A pipeline that collects public government surveillance contracts — from USASpending.gov (federal) and MuckRock FOIA documents (state/local) — extracts structured metadata, stores results in SQLite, and produces SHA-256 anchor records for submission to the BTCPC chain.

## What it does

1. **Federal** (`usaspending.py`) — queries USASpending.gov for contracts with known surveillance vendors (Flock Safety, Vigilant Solutions, ShotSpotter, Axon, Verkada, Fusus). Returns structured data directly — no LLM needed.
2. **Crawl** (`crawl.py`) — queries MuckRock for completed FOIA requests matching surveillance terms, downloads PDF response documents, and extracts text via `pypdf` with Tesseract OCR fallback for scanned docs.
3. **Extract** (`extract.py`) — sends each downloaded document to an LLM and stores structured output in SQLite. Idempotent — skips already-processed docs.
4. **Anchor** (`anchor.py`) — writes `output/anchors_{date}.jsonl`, one JSON record per high-confidence contract. Ready for chain submission once `SurveillanceLedgerAnchor` is implemented.

## Setup

```bash
cd services/surveillance-ledger
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

No API key required to run `federal` or `crawl`. LLM extraction (`extract`) uses whichever backend is available:

| Priority | Condition | Backend |
|----------|-----------|---------|
| 1 | `ANTHROPIC_API_KEY` set | Anthropic claude-haiku |
| 2 | `BTCPC_ACCOUNT` set | BTCPC node `/v1/chat/completions` (pays dreams) |
| 3 | neither | Ollama direct at `OLLAMA_URL` |

## Usage

```bash
python run.py federal        # collect federal contracts from USASpending.gov (no LLM)
python run.py crawl          # crawl MuckRock + download PDFs to data/raw/
python run.py extract        # run LLM extraction on unprocessed MuckRock docs
python run.py anchor         # produce anchor records in output/
python run.py all            # federal + crawl + extract + anchor
python run.py stats          # print summary stats from DB
```

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ANTHROPIC_API_KEY` | — | Anthropic API key (extraction backend #1) |
| `BTCPC_ACCOUNT` | — | BTCPC account name (extraction backend #2, uses dreams) |
| `BTCPC_NODE_URL` | `http://localhost:4242` | BTCPC node URL for inference gateway |
| `BTCPC_EXTRACT_MODEL` | `claude-haiku-4-5` | Anthropic model for extraction |
| `BTCPC_OLLAMA_MODEL` | `dolphin-llama3:latest` | Ollama model for BTCPC/native extraction |
| `OLLAMA_URL` | `http://localhost:11434` | Ollama endpoint for direct extraction |
| `BTCPC_EXTRACT_SLEEP` | `0.5` | Seconds between extraction calls |

## Output files

- `data/raw/{doc_id}.txt` — extracted text for each MuckRock document (gitignored)
- `data/docs_index.json` — MuckRock metadata index (gitignored)
- `data/skip.log` — documents skipped with reason (gitignored)
- `data/surveillance.db` — SQLite database of all contracts (gitignored)
- `output/anchors_{date}.jsonl` — chain anchor records, one per line (gitignored)

## Privacy design

This pipeline extracts contract metadata only: vendor names, agency names, dollar amounts, camera counts, retention periods, and data-sharing clauses. It does not collect, store, or transmit any personal data, license plate numbers, vehicle information, or surveillance footage. The goal is to make the terms of surveillance contracts legible to the public and anchored to a verifiable public ledger — not to replicate or enable the surveillance itself.
