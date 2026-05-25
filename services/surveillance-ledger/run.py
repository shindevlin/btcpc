#!/usr/bin/env python3
"""
Surveillance Ledger entrypoint.

Usage:
  python run.py crawl          # crawl MuckRock, save raw text to data/
  python run.py federal        # collect federal contracts from USASpending.gov
  python run.py extract        # run LLM extraction on unprocessed MuckRock docs
  python run.py anchor         # produce anchor records
  python run.py all            # federal + crawl + extract + anchor
  python run.py stats          # print DB summary stats
"""
import logging
import sys

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger(__name__)


def cmd_crawl() -> None:
    from crawl import crawl_and_save
    logger.info("Starting crawl...")
    n = crawl_and_save()
    logger.info("Crawled %d new documents (text saved to data/raw/)", n)


def cmd_federal() -> None:
    from usaspending import collect_federal_contracts
    logger.info("Collecting federal surveillance contracts from USASpending.gov...")
    inserted, skipped = collect_federal_contracts()
    logger.info("Inserted %d new contracts (%d already in DB)", inserted, skipped)


def cmd_extract() -> None:
    from extract import run_extraction
    logger.info("Starting extraction...")
    extracted, skipped_low = run_extraction(min_confidence=0.5)
    logger.info(
        "Extracted %d contracts (%d flagged low-confidence, excluded from anchors)",
        extracted,
        skipped_low,
    )


def cmd_anchor() -> None:
    from anchor import produce_anchors
    logger.info("Producing anchor records...")
    written, skipped_low = produce_anchors()
    logger.info(
        "Wrote %d anchor records (%d skipped, low confidence) → output/anchors_*.jsonl",
        written,
        skipped_low,
    )


def cmd_stats() -> None:
    from schema import get_stats, init_db
    init_db()
    stats = get_stats()
    print(f"\n=== Surveillance Ledger Stats ===")
    print(f"Total contracts:         {stats['total']}")
    print(f"Total contract value:    ${stats['total_value_usd']:,.0f}")
    print(f"Federal sharing enabled: {stats['federal_sharing_count']}")
    print(f"\nBy vendor:")
    for vendor, count in stats["by_vendor"]:
        print(f"  {vendor or 'UNKNOWN':<15} {count}")
    print()


COMMANDS = {
    "crawl": cmd_crawl,
    "federal": cmd_federal,
    "extract": cmd_extract,
    "anchor": cmd_anchor,
    "stats": cmd_stats,
    "all": None,
}


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "all":
        cmd_federal()
        cmd_crawl()
        cmd_extract()
        cmd_anchor()
    else:
        COMMANDS[cmd]()


if __name__ == "__main__":
    main()
