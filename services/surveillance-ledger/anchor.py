"""
Produce anchor records for BTCPC chain submission.

Each extracted contract gets a JSON anchor record written to
output/anchors_{date}.jsonl (newline-delimited JSON).
These records are ready to submit to the BTCPC chain when the
SurveillanceLedgerAnchor entry type is implemented.
"""
import json
import logging
from datetime import datetime, timezone
from pathlib import Path

from schema import SurveillanceContract, get_all_contracts

logger = logging.getLogger(__name__)

OUTPUT_DIR = Path(__file__).parent / "output"
MIN_CONFIDENCE = 0.5  # Only anchor high-confidence extractions


def make_anchor(contract: SurveillanceContract, extracted_at: str) -> dict:
    """Build a chain anchor record from a contract."""
    return {
        "document_hash": contract.document_hash,
        "vendor_normalized": contract.vendor_normalized,
        "agency_jurisdiction": contract.agency_jurisdiction,
        "extracted_at": extracted_at,
        "source_url": contract.muckrock_url,
        "btcpc_entry_type": "SurveillanceLedgerAnchor",
    }


def produce_anchors(min_confidence: float = MIN_CONFIDENCE) -> tuple[int, int]:
    """
    Load all contracts from DB and write anchor records for those
    meeting the confidence threshold.

    Appends to today's anchors file, deduplicating by document_hash.

    Returns (written, skipped_low_confidence).
    """
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    contracts = get_all_contracts()

    if not contracts:
        logger.info("No contracts in DB. Run extract first.")
        return 0, 0

    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    anchor_file = OUTPUT_DIR / f"anchors_{today}.jsonl"
    extracted_at = datetime.now(timezone.utc).isoformat()

    # Load existing hashes in today's file to dedup
    existing_hashes = set()
    if anchor_file.exists():
        with anchor_file.open() as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        existing_hashes.add(json.loads(line)["document_hash"])
                    except (json.JSONDecodeError, KeyError):
                        pass

    written = 0
    skipped_low = 0

    with anchor_file.open("a") as f:
        for contract in contracts:
            if contract.confidence < min_confidence:
                skipped_low += 1
                continue
            if contract.document_hash in existing_hashes:
                continue

            anchor = make_anchor(contract, extracted_at)
            f.write(json.dumps(anchor) + "\n")
            existing_hashes.add(contract.document_hash)
            written += 1

    return written, skipped_low
