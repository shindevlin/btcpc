"""
USASpending.gov collector for federal surveillance contracts.

Searches the official federal contracts database for surveillance
technology vendors. Returns structured data without any LLM extraction —
USASpending data is already structured.
"""
import hashlib
import json
import logging
import time
from datetime import datetime, timezone
from typing import Iterator

import requests

from schema import SurveillanceContract, hash_exists, init_db, insert_contract

logger = logging.getLogger(__name__)

USA_SPENDING_URL = "https://api.usaspending.gov/api/v2/search/spending_by_award/"

# Vendor search terms → normalized name
VENDOR_MAP = {
    "Flock Safety": "FLOCK",
    "Vigilant Solutions": "MOTOROLA",
    "Digital Recognition Network": "MOTOROLA",
    # "Motorola Solutions" excluded — too broad, mostly radios/comms not ALPR
    "ShotSpotter": "SHOTSPOTTER",
    "SoundThinking": "SHOTSPOTTER",
    "Axon Enterprise": "AXON",
    "Axon Public Safety": "AXON",
    "Verkada": "VERKADA",
    "Fusus": "FUSUS",
}

FIELDS = [
    "Award ID",
    "Recipient Name",
    "Award Amount",
    "Start Date",
    "End Date",
    "Awarding Agency",
    "Awarding Sub Agency",
    "Description",
    "Place of Performance State Code",
    "Place of Performance City Name",
    "generated_internal_id",
]


MAX_PAGES_PER_VENDOR = 5  # cap at 500 results per vendor keyword


def _search_vendor(keyword: str, limit: int = 100) -> list[dict]:
    """Fetch contract awards matching a keyword from USASpending."""
    results = []
    page = 1

    while page <= MAX_PAGES_PER_VENDOR:
        payload = {
            "filters": {
                "keywords": [keyword],
                "award_type_codes": ["A", "B", "C", "D"],  # contracts only
            },
            "fields": FIELDS,
            "limit": limit,
            "page": page,
            "sort": "Award Amount",
            "order": "desc",
        }

        try:
            resp = requests.post(USA_SPENDING_URL, json=payload, timeout=30)
            resp.raise_for_status()
            data = resp.json()
        except requests.RequestException as e:
            logger.warning("USASpending request failed (keyword=%s, page=%d): %s", keyword, page, e)
            break

        batch = data.get("results", [])
        results.extend(batch)

        meta = data.get("page_metadata", {})
        has_next = meta.get("hasNext", False)
        if not has_next or len(batch) == 0:
            break

        page += 1
        time.sleep(0.5)

    return results


def _award_to_contract(award: dict, vendor_normalized: str) -> SurveillanceContract:
    """Convert a USASpending award dict to a SurveillanceContract."""
    desc = award.get("Description", "") or ""
    agency = award.get("Awarding Sub Agency") or award.get("Awarding Agency") or ""
    city = award.get("Place of Performance City Name", "") or ""
    state = award.get("Place of Performance State Code", "") or ""
    jurisdiction = f"{city}, {state}".strip(", ") if city or state else None

    # Build a deterministic hash from the award ID
    award_id = award.get("generated_internal_id") or award.get("Award ID") or ""
    raw = f"usaspending:{award_id}"
    doc_hash = "sha256:" + hashlib.sha256(raw.encode()).hexdigest()

    return SurveillanceContract(
        document_hash=doc_hash,
        muckrock_request_id=None,
        muckrock_url=f"https://www.usaspending.gov/award/{award_id}/",
        documentcloud_id=None,
        document_title=f"{award.get('Recipient Name', '')} — {award_id}",
        agency_name=agency,
        agency_jurisdiction=jurisdiction,
        vendor_name=award.get("Recipient Name"),
        vendor_normalized=vendor_normalized,
        contract_value_usd=award.get("Award Amount"),
        start_date=award.get("Start Date"),
        end_date=award.get("End Date"),
        camera_count=None,
        retention_days=None,
        warrant_required=None,
        federal_sharing_enabled=True,  # federal contracts = federal agency = True by definition
        data_sharing_agencies=[agency] if agency else [],
        audit_log_required=None,
        public_notice_required=None,
        contract_type=_infer_contract_type(desc, vendor_normalized),
        confidence=0.95,  # structured data, high confidence
        extraction_notes=desc[:200] if desc else None,
    )


def _infer_contract_type(desc: str, vendor: str) -> str:
    desc_lower = desc.lower()
    if vendor in ("FLOCK", "MOTOROLA") or "license plate" in desc_lower or "alpr" in desc_lower or "lpr" in desc_lower:
        return "ALPR"
    if vendor == "SHOTSPOTTER" or "shotspotter" in desc_lower or "gunshot" in desc_lower:
        return "AUDIO"
    if vendor == "AXON" or "body worn" in desc_lower or "body camera" in desc_lower or "taser" in desc_lower:
        return "VIDEO"
    if vendor == "VERKADA" or "camera" in desc_lower or "cctv" in desc_lower:
        return "VIDEO"
    if vendor == "FUSUS" or "platform" in desc_lower:
        return "PLATFORM"
    return "OTHER"


def collect_federal_contracts() -> tuple[int, int]:
    """
    Collect all federal surveillance contracts from USASpending.
    Returns (inserted, skipped_existing).
    """
    init_db()
    now_iso = datetime.now(timezone.utc).isoformat()

    inserted = 0
    skipped = 0
    seen_hashes: set[str] = set()

    for keyword, vendor_normalized in VENDOR_MAP.items():
        logger.info("Searching USASpending for: %s", keyword)
        awards = _search_vendor(keyword)
        logger.info("  Found %d awards", len(awards))

        for award in awards:
            contract = _award_to_contract(award, vendor_normalized)

            if contract.document_hash in seen_hashes:
                skipped += 1
                continue
            seen_hashes.add(contract.document_hash)

            if hash_exists(contract.document_hash):
                skipped += 1
                continue

            if insert_contract(contract, now_iso):
                inserted += 1
                logger.info(
                    "  Inserted: %s | %s | $%s",
                    contract.vendor_normalized,
                    contract.agency_name,
                    f"{contract.contract_value_usd:,.0f}" if contract.contract_value_usd else "?",
                )

        time.sleep(1)

    return inserted, skipped
