"""
Structured extraction of surveillance contract data.

Uses Anthropic API when ANTHROPIC_API_KEY is set, falls back to
local Ollama otherwise. Both paths produce the same SQLite output.
"""
import json
import logging
import os
import time
from pathlib import Path
from typing import Optional

from crawl import (
    DOCS_INDEX,
    hash_text,
    list_downloaded_docs,
    log_skip,
)
from schema import (
    SurveillanceContract,
    hash_exists,
    init_db,
    insert_contract,
)

logger = logging.getLogger(__name__)

MAX_CHARS = 32_000
EXTRACTION_SLEEP = float(os.environ.get("BTCPC_EXTRACT_SLEEP", "0.5"))

OLLAMA_URL = os.environ.get("OLLAMA_URL", "http://localhost:11434")
OLLAMA_MODEL = os.environ.get("BTCPC_OLLAMA_MODEL", "dolphin-llama3:latest")
ANTHROPIC_MODEL = os.environ.get("BTCPC_EXTRACT_MODEL", "claude-haiku-4-5")

# BTCPC node inference gateway (OpenAI-compatible)
BTCPC_NODE_URL = os.environ.get("BTCPC_NODE_URL", "http://localhost:4242")
BTCPC_ACCOUNT = os.environ.get("BTCPC_ACCOUNT", "")

SYSTEM_PROMPT = """You are extracting structured data from a government surveillance contract, procurement document, or FOIA response. Extract only what is explicitly stated in the document. If a field is not mentioned, return null.

For vendor_normalized, map to:
- FLOCK if Flock Safety
- MOTOROLA if Motorola Solutions or Vigilant Solutions
- AXON if Axon
- SHOTSPOTTER if ShotSpotter or SoundThinking
- VERKADA if Verkada
- FUSUS if Fusus
- OTHER otherwise

For warrant_required:
- Return true only if the document explicitly requires a warrant or court order for searches
- Return false if searches are explicitly permitted without warrant
- Return null if not stated

For federal_sharing_enabled:
- Return true if ICE, FBI, DHS, Border Patrol, or federal agencies are named as authorized recipients
- Return false if data sharing is explicitly limited to local agencies
- Return null if not stated

Set confidence (0.0-1.0) to your confidence that this is a real surveillance contract. Set to 0.0 if this appears to be a news article, letter, or unrelated document.

Return ONLY a valid JSON object with these fields (no markdown, no explanation):
agency_name, agency_jurisdiction, vendor_name, vendor_normalized, contract_value_usd,
start_date, end_date, camera_count, retention_days, warrant_required,
federal_sharing_enabled, data_sharing_agencies (array of strings), audit_log_required,
public_notice_required, contract_type (ALPR|VIDEO|AUDIO|DRONE|PLATFORM|OTHER|null),
confidence (required, 0.0-1.0), extraction_notes"""


def _truncate_text(text: str) -> str:
    if len(text) <= MAX_CHARS:
        return text
    truncated = text[:MAX_CHARS]
    last_para = truncated.rfind("\n\n")
    if last_para > MAX_CHARS // 2:
        return truncated[:last_para]
    return truncated


def _extract_with_anthropic(text: str, doc_id: str) -> Optional[dict]:
    import anthropic

    RECORD_CONTRACT_TOOL = {
        "name": "record_contract",
        "description": "Record extracted surveillance contract data.",
        "input_schema": {
            "type": "object",
            "properties": {
                "agency_name": {"type": ["string", "null"]},
                "agency_jurisdiction": {"type": ["string", "null"]},
                "vendor_name": {"type": ["string", "null"]},
                "vendor_normalized": {
                    "type": ["string", "null"],
                    "enum": ["FLOCK", "MOTOROLA", "AXON", "SHOTSPOTTER", "VERKADA", "FUSUS", "OTHER", None],
                },
                "contract_value_usd": {"type": ["number", "null"]},
                "start_date": {"type": ["string", "null"]},
                "end_date": {"type": ["string", "null"]},
                "camera_count": {"type": ["integer", "null"]},
                "retention_days": {"type": ["integer", "null"]},
                "warrant_required": {"type": ["boolean", "null"]},
                "federal_sharing_enabled": {"type": ["boolean", "null"]},
                "data_sharing_agencies": {"type": "array", "items": {"type": "string"}},
                "audit_log_required": {"type": ["boolean", "null"]},
                "public_notice_required": {"type": ["boolean", "null"]},
                "contract_type": {
                    "type": ["string", "null"],
                    "enum": ["ALPR", "VIDEO", "AUDIO", "DRONE", "PLATFORM", "OTHER", None],
                },
                "confidence": {"type": "number", "minimum": 0.0, "maximum": 1.0},
                "extraction_notes": {"type": ["string", "null"]},
            },
            "required": ["confidence"],
            "additionalProperties": False,
        },
    }

    truncated = _truncate_text(text)
    client = anthropic.Anthropic()
    try:
        response = client.messages.create(
            model=ANTHROPIC_MODEL,
            max_tokens=2048,
            system=[{"type": "text", "text": SYSTEM_PROMPT, "cache_control": {"type": "ephemeral"}}],
            tools=[RECORD_CONTRACT_TOOL],
            tool_choice={"type": "tool", "name": "record_contract"},
            messages=[{"role": "user", "content": f"Document text:\n\n{truncated}"}],
        )
    except anthropic.APIError as e:
        logger.warning("Anthropic API error for doc %s: %s", doc_id, e)
        return None

    for block in response.content:
        if block.type == "tool_use" and block.name == "record_contract":
            return block.input
    return None


def _extract_with_ollama(text: str, doc_id: str) -> Optional[dict]:
    import requests

    truncated = _truncate_text(text)
    prompt = f"{SYSTEM_PROMPT}\n\nDocument text:\n\n{truncated}"

    try:
        resp = requests.post(
            f"{OLLAMA_URL}/api/generate",
            json={
                "model": OLLAMA_MODEL,
                "prompt": prompt,
                "format": "json",
                "stream": False,
                "options": {"temperature": 0.1, "num_predict": 2048},
            },
            timeout=120,
        )
        resp.raise_for_status()
        raw = resp.json().get("response", "")
        return json.loads(raw)
    except Exception as e:
        logger.warning("Ollama extraction failed for doc %s: %s", doc_id, e)
        return None


def _extract_with_btcpc(text: str, doc_id: str) -> Optional[dict]:
    """
    Extract via the BTCPC node's /v1/chat/completions gateway.
    Requires BTCPC_ACCOUNT to be set. Deducts dreams from that account.
    Uses OpenAI message format — same prompt, JSON output requested.
    """
    import requests as _req

    truncated = _truncate_text(text)
    user_msg = (
        f"{SYSTEM_PROMPT}\n\n"
        "Return ONLY a valid JSON object, no markdown fences.\n\n"
        f"Document text:\n\n{truncated}"
    )

    try:
        resp = _req.post(
            f"{BTCPC_NODE_URL}/v1/chat/completions",
            headers={"Authorization": f"Bearer {BTCPC_ACCOUNT}"},
            json={
                "model": OLLAMA_MODEL,
                "messages": [{"role": "user", "content": user_msg}],
                "temperature": 0.1,
                "max_tokens": 2048,
            },
            timeout=300,  # allow for cold model load on first request
        )
        if resp.status_code == 402:
            logger.warning("BTCPC account %s has insufficient balance for extraction", BTCPC_ACCOUNT)
            return None
        if resp.status_code == 401:
            logger.warning("BTCPC account %s is not valid — check BTCPC_ACCOUNT env var", BTCPC_ACCOUNT)
            return None
        resp.raise_for_status()
        content = resp.json()["choices"][0]["message"]["content"]
        # Strip markdown fences if the model added them
        content = content.strip()
        if content.startswith("```"):
            content = content.split("```")[1]
            if content.startswith("json"):
                content = content[4:]
        return json.loads(content.strip())
    except Exception as e:
        logger.warning("BTCPC extraction failed for doc %s: %s", doc_id, e)
        return None


def _extract(text: str, doc_id: str) -> Optional[dict]:
    """
    Extraction backend priority:
      1. Anthropic API  — if ANTHROPIC_API_KEY is set
      2. BTCPC node     — if BTCPC_ACCOUNT is set (localhost:4242/v1/chat/completions)
      3. Ollama native  — fallback via OLLAMA_URL
    """
    if os.environ.get("ANTHROPIC_API_KEY"):
        return _extract_with_anthropic(text, doc_id)
    if BTCPC_ACCOUNT:
        logger.debug("Using BTCPC node inference (%s, account=%s)", BTCPC_NODE_URL, BTCPC_ACCOUNT)
        return _extract_with_btcpc(text, doc_id)
    logger.debug("No ANTHROPIC_API_KEY or BTCPC_ACCOUNT — using Ollama directly (%s)", OLLAMA_MODEL)
    return _extract_with_ollama(text, doc_id)


def load_docs_index() -> dict:
    if not DOCS_INDEX.exists():
        return {}
    with DOCS_INDEX.open() as f:
        return json.load(f)


def run_extraction(min_confidence: float = 0.5) -> tuple[int, int]:
    """
    Run extraction on all downloaded docs not yet in the DB.
    Returns (extracted_count, skipped_low_confidence).
    """
    init_db()
    docs_index = load_docs_index()
    downloaded = list_downloaded_docs()

    if not downloaded:
        logger.info("No downloaded documents found. Run 'crawl' first.")
        return 0, 0

    if os.environ.get("ANTHROPIC_API_KEY"):
        backend = f"Anthropic ({ANTHROPIC_MODEL})"
    elif BTCPC_ACCOUNT:
        backend = f"BTCPC node ({BTCPC_NODE_URL}, account={BTCPC_ACCOUNT})"
    else:
        backend = f"Ollama direct ({OLLAMA_MODEL})"
    logger.info("Extracting %d docs using %s", len(downloaded), backend)

    extracted = 0
    skipped_low = 0

    from datetime import datetime, timezone
    now_iso = datetime.now(timezone.utc).isoformat()

    for doc_id, text_path in downloaded:
        text = text_path.read_text(encoding="utf-8")
        doc_hash = "sha256:" + hash_text(text)

        if hash_exists(doc_hash):
            logger.debug("Already extracted: %s", doc_id)
            continue

        logger.info("Extracting: %s", doc_id)
        time.sleep(EXTRACTION_SLEEP)

        raw = _extract(text, doc_id)
        if raw is None:
            log_skip(doc_id, "extraction failed")
            continue

        confidence = float(raw.get("confidence", 0.0))
        meta = docs_index.get(doc_id, {})

        contract = SurveillanceContract(
            document_hash=doc_hash,
            muckrock_request_id=meta.get("muckrock_request_id"),
            muckrock_url=meta.get("muckrock_url"),
            documentcloud_id=doc_id,
            document_title=meta.get("document_title"),
            agency_name=raw.get("agency_name"),
            agency_jurisdiction=raw.get("agency_jurisdiction"),
            vendor_name=raw.get("vendor_name"),
            vendor_normalized=raw.get("vendor_normalized"),
            contract_value_usd=raw.get("contract_value_usd"),
            start_date=raw.get("start_date"),
            end_date=raw.get("end_date"),
            camera_count=raw.get("camera_count"),
            retention_days=raw.get("retention_days"),
            warrant_required=raw.get("warrant_required"),
            federal_sharing_enabled=raw.get("federal_sharing_enabled"),
            data_sharing_agencies=raw.get("data_sharing_agencies") or [],
            audit_log_required=raw.get("audit_log_required"),
            public_notice_required=raw.get("public_notice_required"),
            contract_type=raw.get("contract_type"),
            confidence=confidence,
            extraction_notes=raw.get("extraction_notes"),
        )

        inserted = insert_contract(contract, now_iso)
        if inserted:
            extracted += 1
            if confidence < min_confidence:
                skipped_low += 1
                logger.debug("Low confidence (%.2f): %s", confidence, doc_id)
            else:
                logger.info("Extracted: %s (vendor=%s, confidence=%.2f)", doc_id, contract.vendor_normalized, confidence)

    return extracted, skipped_low
