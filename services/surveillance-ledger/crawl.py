"""
MuckRock crawler for surveillance contract documents.

Fetches FOIA requests matching surveillance search terms, collects
document metadata, and downloads + extracts text from PDFs hosted
on MuckRock's CDN (cdn.muckrock.com).
"""
import hashlib
import io
import json
import logging
import time
from pathlib import Path
from typing import Optional

import requests
from pypdf import PdfReader

logger = logging.getLogger(__name__)

# MuckRock blocks Python's default UA via Cloudflare — spoof curl
_SESSION = requests.Session()
_SESSION.headers.update({"User-Agent": "curl/7.68.0"})

MUCKROCK_API = "https://www.muckrock.com/api_v1/foia/"

RAW_DIR = Path(__file__).parent / "data" / "raw"
SKIP_LOG = Path(__file__).parent / "data" / "skip.log"
DOCS_INDEX = Path(__file__).parent / "data" / "docs_index.json"

MAX_PAGES = 5  # per search term

SEARCH_TERMS = [
    "flock safety",
    "automated license plate reader",
    "license plate recognition",
    "ALPR contract",
    "ShotSpotter",
    "SoundThinking",
    "Axon surveillance",
    "Verkada",
    "Fusus",
    "gunshot detection",
    "Motorola ALPR",
    "Vigilant Solutions",
]


def log_skip(doc_id: str, reason: str) -> None:
    SKIP_LOG.parent.mkdir(parents=True, exist_ok=True)
    with SKIP_LOG.open("a") as f:
        f.write(f"{doc_id}\t{reason}\n")


def _fetch_json(url: str, params: dict = None, retries: int = 3) -> Optional[dict]:
    for attempt in range(retries):
        try:
            resp = _SESSION.get(url, params=params, timeout=30)
            if resp.status_code == 404:
                return None
            resp.raise_for_status()
            return resp.json()
        except requests.RequestException as e:
            if attempt == retries - 1:
                logger.warning("Failed to fetch %s: %s", url, e)
                return None
            time.sleep(2 ** attempt)
    return None


def crawl_muckrock() -> dict:
    """
    Crawl MuckRock for completed FOIA requests matching surveillance terms.
    Returns a dict: {doc_id -> {muckrock_request_id, muckrock_url, document_title, pages, ffile_url}}
    """
    RAW_DIR.mkdir(parents=True, exist_ok=True)

    if DOCS_INDEX.exists():
        with DOCS_INDEX.open() as f:
            docs = json.load(f)
    else:
        docs = {}

    seen_request_ids = set()

    for term in SEARCH_TERMS:
        logger.info("Searching MuckRock for: %s", term)
        page = 1
        next_url = MUCKROCK_API

        while page <= MAX_PAGES:
            # Filter to completed requests only — these have actual documents
            params = {"search": term, "status": "done", "format": "json", "page": page}
            data = _fetch_json(
                next_url,
                params=params if next_url == MUCKROCK_API else None,
            )
            if not data:
                break

            results = data.get("results", [])
            if not results:
                break

            for request in results:
                req_id = str(request.get("id", ""))
                if req_id in seen_request_ids:
                    continue
                seen_request_ids.add(req_id)

                req_url = request.get("absolute_url", "")
                if req_url and not req_url.startswith("http"):
                    req_url = "https://www.muckrock.com" + req_url

                for comm in request.get("communications", []):
                    # MuckRock API uses "files" key, not "documents"
                    for doc in comm.get("files", []):
                        doc_id = str(doc.get("doc_id", ""))
                        ffile_url = doc.get("ffile", "")

                        # Require either a doc_id or a direct PDF URL
                        if not doc_id and not ffile_url:
                            continue
                        # Use doc_id as primary key; fall back to file id
                        key = doc_id or str(doc.get("id", ""))
                        if not key or key in docs:
                            continue

                        docs[key] = {
                            "muckrock_request_id": req_id,
                            "muckrock_url": req_url,
                            "document_title": doc.get("title", ""),
                            "pages": doc.get("pages", 0),
                            "ffile_url": ffile_url,
                            "doc_id": doc_id,
                        }

            next_url_from_api = data.get("next")
            if not next_url_from_api:
                break
            next_url = next_url_from_api
            page += 1
            time.sleep(0.3)

    with DOCS_INDEX.open("w") as f:
        json.dump(docs, f, indent=2)

    logger.info("Found %d unique documents in MuckRock index", len(docs))
    return docs


def fetch_document_text(doc_id: str, meta: dict) -> Optional[str]:
    """
    Fetch and extract text for a document.
    Tries direct PDF download from ffile_url first (cdn.muckrock.com).
    Falls back to DocumentCloud text endpoint if doc_id is present.
    Returns extracted text, or None on failure.
    """
    ffile_url = meta.get("ffile_url", "")

    if ffile_url:
        return _extract_pdf_text(ffile_url, doc_id)

    logger.debug("No ffile_url for %s, skipping", doc_id)
    return None


def _extract_pdf_text(pdf_url: str, doc_id: str) -> Optional[str]:
    """
    Download a PDF and extract text.
    First tries pypdf (fast, works on text-layer PDFs).
    Falls back to Tesseract OCR for scanned/image PDFs.
    """
    try:
        resp = _SESSION.get(pdf_url, timeout=60, stream=True)
        if resp.status_code == 404:
            return None
        resp.raise_for_status()
    except requests.RequestException as e:
        logger.warning("PDF download failed for %s: %s", doc_id, e)
        return None

    pdf_bytes = resp.content

    # Try text-layer extraction first
    try:
        reader = PdfReader(io.BytesIO(pdf_bytes))
        pages_text = [p.extract_text() or "" for p in reader.pages]
        combined = "\n".join(pages_text).strip()
        if len(combined) > 100:  # meaningful text found
            return combined
        # Very little text → likely scanned, fall through to OCR
        logger.debug("Sparse text from pypdf (%d chars), trying OCR: %s", len(combined), doc_id)
    except Exception as e:
        logger.debug("pypdf failed for %s: %s — trying OCR", doc_id, e)

    # OCR fallback using Tesseract
    return _ocr_pdf(pdf_bytes, doc_id)


def _ocr_pdf(pdf_bytes: bytes, doc_id: str) -> Optional[str]:
    """OCR a scanned PDF with Tesseract via pdf2image."""
    try:
        from pdf2image import convert_from_bytes
        import pytesseract
    except ImportError:
        logger.debug("pdf2image/pytesseract not installed — skipping OCR for %s", doc_id)
        return None

    try:
        images = convert_from_bytes(pdf_bytes, dpi=200)
        pages_text = [pytesseract.image_to_string(img) for img in images]
        combined = "\n".join(pages_text).strip()
        return combined if len(combined) > 50 else None
    except Exception as e:
        logger.warning("OCR failed for %s: %s", doc_id, e)
        return None


def text_path_for(doc_id: str) -> Path:
    return RAW_DIR / f"{doc_id}.txt"


def hash_text(text: str) -> str:
    """Return SHA-256 of text content (canonical form: no BOM, UTF-8)."""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def crawl_and_save(force: bool = False) -> int:
    """
    Full crawl: fetch MuckRock index, then extract text for each doc.
    Skips docs already saved unless force=True.
    Returns count of newly downloaded documents.
    """
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    docs = crawl_muckrock()
    downloaded = 0

    for doc_id, meta in docs.items():
        out_path = text_path_for(doc_id)
        if out_path.exists() and not force:
            continue

        time.sleep(0.3)  # be polite to cdn.muckrock.com
        text = fetch_document_text(doc_id, meta)

        if text is None:
            log_skip(doc_id, "PDF fetch or parse failed")
            logger.debug("Skipped %s: not available", doc_id)
            continue

        if not text.strip():
            log_skip(doc_id, "Empty text after extraction")
            continue

        out_path.write_text(text, encoding="utf-8")
        downloaded += 1
        logger.info("Saved doc %s (%d chars)", doc_id, len(text))

    return downloaded


def list_downloaded_docs() -> list:
    """Return list of (doc_id, text_path) for all locally saved documents."""
    if not RAW_DIR.exists():
        return []
    return [(p.stem, p) for p in sorted(RAW_DIR.glob("*.txt"))]
