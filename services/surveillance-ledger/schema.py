"""
Pydantic models and SQLite setup for surveillance contract extraction.
"""
import json
import sqlite3
from pathlib import Path
from typing import Optional, List
from pydantic import BaseModel


class SurveillanceContract(BaseModel):
    # Source
    muckrock_request_id: Optional[str] = None
    muckrock_url: Optional[str] = None
    documentcloud_id: Optional[str] = None
    document_title: Optional[str] = None
    document_hash: str  # SHA-256 of raw text content

    # Contract parties
    agency_name: Optional[str] = None        # e.g. "Austin Police Department"
    agency_jurisdiction: Optional[str] = None # e.g. "Austin, TX"
    vendor_name: Optional[str] = None        # e.g. "Flock Safety"
    vendor_normalized: Optional[str] = None  # FLOCK|MOTOROLA|AXON|SHOTSPOTTER|VERKADA|FUSUS|OTHER

    # Contract terms
    contract_value_usd: Optional[float] = None
    start_date: Optional[str] = None         # ISO date string
    end_date: Optional[str] = None
    camera_count: Optional[int] = None
    retention_days: Optional[int] = None

    # Data governance
    warrant_required: Optional[bool] = None
    federal_sharing_enabled: Optional[bool] = None
    data_sharing_agencies: List[str] = []    # named agencies in sharing clauses
    audit_log_required: Optional[bool] = None
    public_notice_required: Optional[bool] = None

    # Classification
    contract_type: Optional[str] = None      # ALPR|VIDEO|AUDIO|DRONE|PLATFORM|OTHER
    confidence: float = 0.0                   # 0.0-1.0, LLM self-reported confidence
    extraction_notes: Optional[str] = None   # LLM notes on ambiguity


DB_PATH = Path(__file__).parent / "data" / "surveillance.db"


def get_db() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(DB_PATH))
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    """Create tables if they don't exist."""
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS contracts (
            document_hash TEXT PRIMARY KEY,
            muckrock_request_id TEXT,
            muckrock_url TEXT,
            documentcloud_id TEXT,
            document_title TEXT,
            agency_name TEXT,
            agency_jurisdiction TEXT,
            vendor_name TEXT,
            vendor_normalized TEXT,
            contract_value_usd REAL,
            start_date TEXT,
            end_date TEXT,
            camera_count INTEGER,
            retention_days INTEGER,
            warrant_required INTEGER,
            federal_sharing_enabled INTEGER,
            data_sharing_agencies TEXT,
            audit_log_required INTEGER,
            public_notice_required INTEGER,
            contract_type TEXT,
            confidence REAL,
            extraction_notes TEXT,
            extracted_at TEXT
        )
    """)
    conn.commit()
    conn.close()


def insert_contract(contract: SurveillanceContract, extracted_at: str) -> bool:
    """
    Insert a contract record. Returns True if inserted, False if already exists.
    """
    conn = get_db()
    try:
        conn.execute("""
            INSERT INTO contracts VALUES (
                ?, ?, ?, ?, ?,
                ?, ?, ?, ?,
                ?, ?, ?, ?, ?,
                ?, ?,
                ?, ?, ?,
                ?, ?, ?,
                ?
            )
        """, (
            contract.document_hash,
            contract.muckrock_request_id,
            contract.muckrock_url,
            contract.documentcloud_id,
            contract.document_title,
            contract.agency_name,
            contract.agency_jurisdiction,
            contract.vendor_name,
            contract.vendor_normalized,
            contract.contract_value_usd,
            contract.start_date,
            contract.end_date,
            contract.camera_count,
            contract.retention_days,
            int(contract.warrant_required) if contract.warrant_required is not None else None,
            int(contract.federal_sharing_enabled) if contract.federal_sharing_enabled is not None else None,
            json.dumps(contract.data_sharing_agencies),
            int(contract.audit_log_required) if contract.audit_log_required is not None else None,
            int(contract.public_notice_required) if contract.public_notice_required is not None else None,
            contract.contract_type,
            contract.confidence,
            contract.extraction_notes,
            extracted_at,
        ))
        conn.commit()
        return True
    except sqlite3.IntegrityError:
        # document_hash already in DB
        return False
    finally:
        conn.close()


def hash_exists(document_hash: str) -> bool:
    """Check if a document hash is already in the DB."""
    conn = get_db()
    row = conn.execute(
        "SELECT 1 FROM contracts WHERE document_hash = ?", (document_hash,)
    ).fetchone()
    conn.close()
    return row is not None


def get_all_contracts() -> List[SurveillanceContract]:
    """Load all contracts from the DB."""
    conn = get_db()
    rows = conn.execute("SELECT * FROM contracts").fetchall()
    conn.close()
    contracts = []
    for row in rows:
        d = dict(row)
        d["data_sharing_agencies"] = json.loads(d["data_sharing_agencies"] or "[]")
        d["warrant_required"] = bool(d["warrant_required"]) if d["warrant_required"] is not None else None
        d["federal_sharing_enabled"] = bool(d["federal_sharing_enabled"]) if d["federal_sharing_enabled"] is not None else None
        d["audit_log_required"] = bool(d["audit_log_required"]) if d["audit_log_required"] is not None else None
        d["public_notice_required"] = bool(d["public_notice_required"]) if d["public_notice_required"] is not None else None
        # Remove extracted_at before parsing into model
        d.pop("extracted_at", None)
        contracts.append(SurveillanceContract(**d))
    return contracts


def get_stats() -> dict:
    """Return summary statistics from the DB."""
    conn = get_db()
    total = conn.execute("SELECT COUNT(*) FROM contracts").fetchone()[0]
    by_vendor = conn.execute(
        "SELECT vendor_normalized, COUNT(*) as cnt FROM contracts GROUP BY vendor_normalized ORDER BY cnt DESC"
    ).fetchall()
    total_value = conn.execute(
        "SELECT SUM(contract_value_usd) FROM contracts WHERE contract_value_usd IS NOT NULL"
    ).fetchone()[0]
    federal_count = conn.execute(
        "SELECT COUNT(*) FROM contracts WHERE federal_sharing_enabled = 1"
    ).fetchone()[0]
    conn.close()
    return {
        "total": total,
        "by_vendor": [(r[0] or "UNKNOWN", r[1]) for r in by_vendor],
        "total_value_usd": total_value or 0.0,
        "federal_sharing_count": federal_count,
    }
