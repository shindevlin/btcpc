# HF Radio Legal Assessment: JS8Call Epoch Seal Hash Beaconing

**Date:** 2026-05-24 | **Status:** Research assessment — not legal advice

## Compliance Verdict

The use case is architecturally sound. The legal risk collapses almost entirely to one question: is the same node that broadcasts on JS8Call also earning BTCPC mining rewards? If yes — pecuniary interest violation in every jurisdiction. If no (beaconing node is a reward-free relay observer) — most risk dissolves and the transmission is defensible as technical investigation in permissive jurisdictions.

---

## Regulatory Foundation: ITU Radio Regulations Article 25

All six jurisdictions are ITU member states. Every national framework derives from RR Article 25:

> "A radiocommunication service for the purpose of self-training, intercommunication and technical investigations carried out by amateurs... without pecuniary interest."

Two universal constraints follow: (1) no pecuniary interest, (2) purpose must be self-training, intercommunication, or technical investigation.

---

## The Two Tests

### Test 1 — Is a SHA-256 epoch seal hash "encrypted to obscure meaning"?

A SHA-256 digest of publicly defined chain state is not encryption-for-obscurity. It is deterministic, applied to public data, verifiable by anyone with the open-source node software, with no shared secret or key. The legal standard varies:

- **Intent-based** (Ireland SI 192/2009 Reg 7(1)(r)): prohibits encoding "for the purpose of obscuring meaning." SHA-256 of public data **passes**.
- **Enumerated whitelist** (Costa Rica SUTEL): only Morse, Q-code, international phonetic alphabet permitted. SHA-256 hex **fails**.
- **Clear-language requirement** (Guatemala): communications in Spanish or "universally accepted code." SHA-256 is **arguable** — strengthened by a published public spec.

### Test 2 — Pecuniary interest?

- **Same node earns rewards AND broadcasts:** Direct/indirect pecuniary interest. Violation everywhere.
- **Separate relay-only observer node, no stake, no reward key:** No pecuniary interest. Defensible as technical investigation.

The separation is non-negotiable under amateur radio law in all jurisdictions reviewed.

---

## Jurisdiction Analysis

### Ireland / ComReg — PERMISSIVE ✓
**Instruments:** S.I. No. 192/2009; ComReg 09/45 R5 (2023) | **IARU:** Region 1 | **Source:** High (statute text read directly)

- Encoding: **Intent-based** — SHA-256 hash passes Reg 7(1)(r)
- Digital modes: Explicitly authorized, no additional licence needed
- Unattended beacon: Confirm with ComReg before deploying (no explicit automatic beacon clause in SI 192/2009)
- Station ID: JS8Call includes call sign by default; verify 10-minute interval compliance

**Best overall jurisdiction.** English-language regs, CEPT/HAREC alignment enables EU-wide reciprocal operation.

---

### Panama / ASEP + Ministerio de Gobierno — PERMISSIVE (Central America) ✓
**Instruments:** Decreto Ejecutivo No. 205 (2004); ASEP PNAF | **IARU:** Region 2 | **IARP:** Yes (Law 11/2003) | **Source:** High (Decreto 205 HTML read directly)

- Encoding: **No explicit prohibition found** in Decreto 205. Art. 27 prohibits commercial/offensive content only.
- Digital modes: **Explicitly authorized** — Art. 5 lists FSK, PSK, packet, "digital information" generally
- Experimental purpose: **Explicitly authorized** — Art. 4 lists "experimentación"
- Confirm no separate encryption regulation with ASEP before operating

**Best Central American jurisdiction.** Most explicit digital mode + experimental authorization in the set.

---

### Belize / PUC — UNCERTAIN ⚠
**Instruments:** Telecom Act CAP.299 (2020); PUC Amateur Radio Framework (2022) | **IARU:** Region 2 | **Source:** Secondary (PDF inaccessible)

- Encoding: Primary text not accessible — treat as unverified
- **Residency constraint:** License available to resident citizens only — foreign operator needs a local collaborator
- Contact PUC directly: https://www.puc.bz/amateur-radio-framework/

---

### Guatemala / SIT — MODERATE RISK ⚠
**Instruments:** Ley de Radiocomunicaciones (Decreto 115-96) | **IARU:** Region 2 | **Source:** Secondary

- Encoding: **Clear-language requirement** — Art. 58 requires Spanish or "universally accepted code"
- SHA-256 hex is arguable; a published public payload specification substantially strengthens the position
- Contact SIT before operating: https://sit.gob.gt/gerencia-de-frecuencias/

---

### Honduras / CONATEL — UNCERTAIN ⚠
**Instruments:** CONATEL Reglamento Radioaficionados (2021) | **IARU:** Region 2 | **Source:** Secondary (PDF 404)

- Encoding: **Unknown** — primary regulation text inaccessible
- Contact CONATEL directly: https://www.conatel.gob.hn/

---

### Costa Rica / SUTEL — NON-PERMISSIBLE ✗
**Instruments:** Ley 8642; Decreto 40639-MICITT (2017); SUTEL Manual v3.0 (2025) | **IARU:** Region 2 | **Source:** High

- Encoding: **Enumerated whitelist** — only Morse, Q-code, international phonetic alphabet. SHA-256 hex fails explicitly.
- Do not operate without formal SUTEL experimental authorization or explicit ruling.

---

## Summary Table

| Jurisdiction | Regulator | Encoding Standard | Digital Modes | Rating |
|---|---|---|---|---|
| Ireland | ComReg | Intent-based ✓ | Explicit ✓ | **PERMISSIVE** |
| Panama | ASEP / Min. Gobierno | No explicit prohibition ✓ | Explicit ✓ | **PERMISSIVE** |
| Belize | PUC | Unknown | Presumed yes | **UNCERTAIN** |
| Guatemala | SIT | Clear-language ⚠ | Presumed yes | **MODERATE RISK** |
| Honduras | CONATEL | Unknown | Presumed yes | **UNCERTAIN** |
| Costa Rica | SUTEL | Whitelist ✗ | Yes | **NON-PERMISSIBLE** |

---

## Recommended Conditions for a Prototype

1. **Beaconing node must be reward-free.** No registered stake, no reward address, not a signing clock. Physically separate from any earning node. Non-negotiable.

2. **Publish a public payload spec before transmitting.** Document the 40-byte format (epoch number + SHA-256 digest + node prefix) in the BTCPC repo with a version number. Reference the spec URL in the JS8Call message field. Makes the "not obscuring meaning" argument evidential.

3. **Station ID must be active and interval-compliant.** JS8Call includes call sign in protocol frames by default — verify this is on and meets the local interval requirement (typically 10 minutes).

4. **Log the operation as a declared technical experiment.** Date, time, frequency, power, epoch number transmitted, purpose statement: *"Technical investigation into distributed ledger chain-tip propagation over HF digital modes."* Submit to regulator if prior notification is required.

5. **Hash-only payloads — no transaction or financial data.** Epoch number + SHA-256 digest + node prefix only. No LedgerEntry contents, transfer amounts, or wallet addresses. The `lorawan.rs` `queue_hash([u8;32])` pattern is the correct model.

6. **Comply with IARU band plan.** JS8Call convention: 14.078 MHz (20m), 7.078 MHz (40m), 3.578 MHz (80m). Verify against operator's license class and local authorization.

7. **Confirm unattended beacon authorization.** Contact ComReg (Ireland) or ASEP (Panama) before deploying automatic epoch-cycle beaconing. A 30-second unattended cycle may require specific authorization under attended-station conditions.

---

## Regulatory Contacts

| Jurisdiction | Regulator | Contact |
|---|---|---|
| Ireland | ComReg | https://www.comreg.ie |
| Panama | ASEP | https://asep.gob.pa |
| Belize | PUC | https://www.puc.bz/telecommunications/ |
| Guatemala | SIT | https://sit.gob.gt/gerencia-de-frecuencias/ |
| Honduras | CONATEL | https://www.conatel.gob.hn |
| Costa Rica | SUTEL | https://sutel.go.cr/pagina/radio-aficionados-y-banda-ciudadana |

---

*This document is a technical research assessment, not legal advice. Primary regulatory texts for Belize (PUC 2022 PDF) and Honduras (CONATEL 2021 PDF) were inaccessible during this review — those ratings carry lower confidence. Consult a licensed telecommunications attorney in the operating jurisdiction before commencing transmission.*
