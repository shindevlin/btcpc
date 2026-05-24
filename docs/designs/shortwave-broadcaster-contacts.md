# BTCPC Beacon — Shortwave Broadcaster Contact Sheet

Produced by: Artistic Engineer Agent
Date: 2026-05-24
Purpose: Research contacts and rates for leasing shortwave broadcast time to transmit BTCPC sealed epoch hashes as Olivia/MFSK digital mode audio.

Related document: docs/designs/bitcoin-mesh-relay.md
Related proposal: novelproposals.md — "BTCPC Beacon" entries

---

## Summary Recommendation

**Contact WRMI first.** They are the only US commercial shortwave broadcaster with a published per-minute rate, existing digital-data-over-shortwave precedent (Shortwave Radiogram), and a general manager who can be reached directly.

**Contact Kim Andrew Elliott in parallel.** He runs Shortwave Radiogram on WRMI. A sponsored segment within that existing program may be cheaper and faster than booking raw WRMI airtime, and it comes with an existing audience of SDR hobbyists who know how to decode MFSK.

**WBCQ is a credible backup** at $50/hour ($0.83/min) with a more permissive content culture. Contact Allan Weiner directly.

**NEXUS-IBA is a European fallback** for coverage targeting Europe/Africa/Asia rather than the Americas. Requires explicit negotiation on digital-mode-audio content; their IPAR program specifies audio-only MP3 submission, so a data beacon needs a conversation.

**WWCR, TWR Bonaire, and Caribbean Beacon are not viable paths** — WWCR is exclusively religious/talk, TWR Bonaire ended shortwave in 1993, and Caribbean Beacon is a single-broadcaster religious station with no public third-party booking process.

---

## Option 1 — WRMI (Radio Miami International) [PRIMARY TARGET]

**Status: Active, recommended, contact immediately**

### About
The largest privately-owned shortwave broadcaster in the Western Hemisphere. Located in Okeechobee, Florida. Operates 14 transmitters (most at 100 kW) with 23 directional antennas covering 11 worldwide beam directions. Already carries Shortwave Radiogram — the exact technical stack (MFSK digital tones over shortwave, decoded by RTL-SDR + Fldigi) BTCPC intends to use. This is not theoretical; it is production-proven.

Frequencies span 5–21 MHz, with multiple antenna directions covering North America, South America, Europe, Africa, and the Pacific.

### Key Contact
**Jeff White — General Manager**
Jeff White is also Secretary-Treasurer of the National Association of Shortwave Broadcasters (NASB).

- Email: info@wrmi.net
- Phone: +1-305-559-9764
- Mailing: 10400 NW 240th Street, Okeechobee, Florida 34972, USA
- Booking page: https://www.wrmi.net/index.php/broadcast-us/

### Rates
**$1.00 per minute** — published rate card for block airtime purchases (fetched 2026-05-24 via search).
Blocks are available in 15, 30, and 60-minute windows.

Rate card source: search-confirmed from WRMI's own website content. Verify directly before committing.

### Cost Scenarios
| Schedule | Slots/day | Duration | Daily cost | Monthly cost |
|---|---|---|---|---|
| 4x daily (propagation windows) | 4 | 1 min each | $4 | ~$120 |
| Hourly continuous | 24 | 1 min each | $24 | ~$720 |
| 2x daily | 2 | 1 min each | $2 | ~$60 |

A 1-minute slot can carry the full BTCPC beacon payload many times over. A 32-byte SHA-256 hash + epoch height + timestamp encodes to ~120 characters in MFSK32, which transmits in under 3 seconds. One minute of airtime is generous.

### Content Policy
WRMI accepts a wide range of programming. No published restriction on digital-mode audio content. The Shortwave Radiogram precedent strongly implies MFSK tones are acceptable. **Confirm explicitly in the first email whether they accept digital-mode audio files (e.g., an MFSK32-encoded WAV file) as broadcast content.**

### Frequency and Coverage
Ask Jeff White to recommend:
- A frequency targeting the Americas daytime/evening (17–21 MHz day, 9–11 MHz evening)
- A frequency with North Atlantic / Europe coverage (15–17 MHz)
- Night sky-wave coverage for the Eastern US / Caribbean / South America (5–7 MHz)

Ideal: two frequency/time slots per day — one Americas, one Europe/Africa.

### Next Step
Send an email to info@wrmi.net with:
1. Description of the content (BTCPC epoch hash beacon — digital audio file in MFSK32 format, approximately 30 seconds of audio per transmission)
2. Request for confirmation that digital-mode audio files are acceptable content
3. Request for a rate confirmation and frequency availability
4. Request for the scheduling granularity (can we book a 1-minute slot repeating daily, or is weekly the minimum?)

---

## Option 2 — Shortwave Radiogram / Kim Andrew Elliott [PARALLEL CONTACT]

**Status: Active, worth contacting directly — a sponsored segment is potentially cheaper and comes with an existing audience**

### About
Shortwave Radiogram is a weekly shortwave program produced by Dr. Kim Andrew Elliott (retired VOA). It broadcasts digital text and images encoded as MFSK32/MFSK64/BPSK31 modem tones over licensed shortwave transmitters — exactly the same stack BTCPC intends to use. Transmitted via WRMI (Florida) and WINB (Pennsylvania). Active as of program 424, November 2025 and beyond.

This is the only program in the world that already does exactly what BTCPC wants to do: encode structured digital data as modem tones and broadcast it globally on shortwave. The audience (SDR hobbyists who monitor these broadcasts and decode with Fldigi) is precisely the technically-inclined early-adopter audience BTCPC wants to reach.

### Key Contact
**Dr. Kim Andrew Elliott**
- Email: radiogram@verizon.net
- Website: https://swradiogram.net
- Kim Elliott at USC: https://uscpublicdiplomacy.org/users/kim_elliott

### What to Ask
There is no published submission process for contributing data to the Shortwave Radiogram. This is a one-person production. The approach is:

1. Email Kim Elliott at radiogram@verizon.net
2. Explain the BTCPC beacon concept — sealed epoch hash (32 bytes hex), transmitted as a brief MFSK32 segment, no voice required
3. Ask whether he would consider including a sponsored BTCPC data segment (15–30 seconds) in a weekly Shortwave Radiogram broadcast as an experiment
4. Offer to provide the pre-encoded MFSK32 audio file — he just includes it as an audio segment in the show

If he declines, that is fine — the WRMI direct booking path achieves the same technical outcome. But a Shortwave Radiogram sponsorship would be cheaper (sponsoring a segment within an existing WRMI booking rather than booking raw WRMI airtime separately) and comes with built-in credibility and an existing audience.

### Notes
- Kim Elliott has included experimental digital modes and data experiments in past programs (confirmed by the program history showing experiments with DRM, HamDRM, and various MFSK variants)
- The program is non-commercial in tone; a "BTCPC blockchain hash" framing may not fit his editorial approach; frame it as "a cryptographic epoch hash from a distributed compute network" or similar
- Do not approach this as a paid advertisement; approach it as a technical experiment collaboration

---

## Option 3 — WBCQ (The Planet) [BACKUP]

**Status: Active, more permissive content culture, slightly lower rate**

### About
WBCQ broadcasts from Monticello, Maine. Up to 500,000 watts on a rotatable curtain antenna (installed 2018). Operates on 7.490 MHz, 9.330 MHz, 5.130 MHz, 3.265 MHz, and 6.160 MHz. Known for carrying unconventional, alternative, and eclectic programming — historically accepted content that other broadcasters declined. Coverage: North America, Caribbean, South America, and Europe depending on frequency and time of day.

WBCQ was purchased by Allan Weiner from World Harvest Radio in 2020 — WBCQ now operates both the Maine site (primary) and the former WHRI South Carolina site (leased frequencies).

### Key Contact
**Allan Weiner — Owner and General Manager**
- Phone: 1-207-889-0039
- Email: wbcq@wbcq.com
- Twitter/X: @AllanWBCQ
- Website: https://www.wbcq.com

### Rates
**Approximately $50/hour** (~$0.83/min) as of February 2026, per search-confirmed pricing. Earlier promotional rates as low as $25/hour on the 6160 kHz transmitter were offered (minimum 4 hours weekly).

Rate is confirmed via search results citing a February 2026 price. Verify directly with Allan Weiner for current rates and availability.

### Cost Scenarios
At $50/hour (~$0.83/min):
| Schedule | Daily cost | Monthly cost |
|---|---|---|
| 4 x 1-minute slots | ~$3.32 | ~$100 |
| 1 x 15-minute block | ~$12.50 | ~$375 |

### Content Policy
WBCQ's programming history includes pirate radio veterans, unconventional talk formats, and alternative content. Allan Weiner is known for a liberal content acceptance policy compared to religious broadcasters. Digital-mode audio data content should be acceptable — but confirm explicitly.

### Coverage Notes
- 7.490 MHz: Good North America and Caribbean coverage at night
- 9.330 MHz: Better daytime long-range / transatlantic coverage
- 5.130 MHz: Nighttime, Americas

### Next Step
Call or email Allan Weiner at wbcq@wbcq.com. Explain the BTCPC beacon concept and ask for current rates and frequency/time availability for 1-minute daily slots.

---

## Option 4 — NEXUS-IBA / IRRS-Shortwave (Milan, Italy) [EUROPE/AFRICA COVERAGE]

**Status: Active, quote-based pricing, suitable if coverage targeting Europe/Africa/Asia is a priority**

### About
NEXUS-International Broadcasting Association is a UN-recognized NGO operating shortwave transmitters from 10 to 300 kW. Transmits globally via partner facilities. Registered in Milan, Italy (also Dublin, Ireland office). Runs the IPAR (International Public Access Radio) service for non-religious, non-commercial producers. Also runs IRRS-Shortwave and IRRS-MediumWave.

Coverage: Europe, Africa, Middle East, Asia/Pacific, Americas — though their transmitters are generally Europe/Africa optimized compared to WRMI's Americas focus.

### Key Contact
- Email: info@nexus.org
- Phone Italy: +39 02 2666971
- Phone UK: +44 20 3769-0185
- Phone USA: (201) 540-0996
- USA Toll-Free: 888-612-0039
- Quote request: https://www.nexus.org/quote-request/ (use promo code IPARPROMO for IPAR)
- Contact page: https://www.nexus.org/contact-us/

NEXUS offers "a no-obligation, 15-minute free-of-charge consultation with a senior specialist."

### Rates
Quote-based. NEXUS describes their rates as "just 1/6 of the lowest 1-minute rate of a radio or TV spot on a local US station." No published per-minute rate. Request a quote.

### Content Policy (IPAR)
- Non-religious required
- Non-commercial content required for IPAR track (BTCPC is a blockchain/compute network — this framing needs care; present it as a technical research/experimental broadcasting project)
- Organization turnover under $50,000 USD/EUR for IPAR
- IPAR specifies MP3 submission — digital-mode audio (MFSK tones) submitted as a WAV/MP3 file should technically comply, but **confirm explicitly before booking**

### Critical Unknown
NEXUS's IPAR submission form specifies "MP3" upload. A digital-mode audio file (MFSK32 tones) can be encoded as MP3 — the question is whether NEXUS considers a data-mode audio file as acceptable "non-commercial audio programming." This must be clarified before any booking.

### Next Step
Email info@nexus.org and ask: "We want to transmit a periodic shortwave beacon containing a cryptographic data string encoded as MFSK32 audio tones. The audio file would be an MP3 containing modem tones — no voice. Is this acceptable content under your IPAR program or general airtime service? What is the minimum slot length and approximate rate?"

---

## Ruled-Out Options

### WWCR (Nashville, Tennessee)
- Address: 1300 WWCR Avenue, Nashville, TN 37218
- Phone: 615-255-1300
- Website: https://www.wwcr.com — contact form at https://wwcr.com/contact.html
- Airtime page: https://www.wwcr.com/buying-airtime.html
- Owner: F.W. Robbert Broadcasting

**Verdict: Not recommended.** WWCR is entirely religious/talk format. All four transmitters are leased to religious organizations and speakers. Their buying-airtime page targets ministries and preachers. A cryptographic data beacon would be anomalous content and would almost certainly be declined. No published per-minute rate for non-religious content found.

**If you contact them anyway:** Call 615-255-1300. Their website's contact form is at wwcr.com/contact.html. A ZoomInfo record references Chris Buchanan with a wwcr.com email but this is not confirmed primary-source.

---

### TWR Bonaire (PJB)
**Verdict: Dead end.** Trans World Radio ended shortwave broadcasts from Bonaire in 1993. The current Bonaire operation is "Shine 800 AM" — medium wave 800 kHz serving Latin America with religious programming at 440–450 kW. No shortwave infrastructure. No third-party airtime. TWR Bonaire is not an option for any BTCPC shortwave beacon use case.

---

### Caribbean Beacon (Anguilla, University Network)
- Frequencies: 11.775 MHz daytime / 6.090 MHz nighttime, 100 kW
- Status: Active, religious-only programming
**Verdict: Not viable.** Caribbean Beacon is a single-purpose religious broadcaster (University Network — Doctor Gene Scott legacy content). No public booking process for third parties. No evidence of any third-party airtime availability.

---

### Radio Verdad (Guatemala, TGAV)
- Frequency: 4055 kHz, 700 watts
- Contact: radioverdad5@yahoo.com
- Website: radioverdad.org
**Verdict: Not viable.** 700 watts is insufficient for meaningful skywave coverage. Religious programming only. The station's website shows minimal recent activity. Not a candidate.

---

### NEXUS IPAR — Organization Size Constraint Note
NEXUS IPAR requires organizations with annual turnover under $50,000 USD/EUR. If BTCPC's organizational entity (if formalized) exceeds this, the IPAR track is unavailable. Use the general NEXUS airtime booking track instead (no turnover limit, higher rates).

---

## Technical Spec for All Broadcaster Contacts

When contacting any broadcaster, the content description to provide:

> We would like to broadcast a periodic shortwave beacon containing a cryptographic digest string. The content is a short text string (approximately 120 characters) encoded as MFSK32 digital audio tones — the same format used by Shortwave Radiogram. We would deliver a pre-encoded audio file (WAV or MP3, approximately 30–60 seconds) for scheduled playback. No voice. No music. The content is a sequence number and hash string from a distributed compute network, transmitted for chain verification purposes. The content is legal, non-commercial in tone, and contains no hate speech, political advocacy, or regulated financial advice.

This framing is accurate and positions the beacon as a technical/experimental broadcast compatible with most broadcaster content policies.

---

## ITU / HFCC Note (for any future owned-transmitter scenario)

If BTCPC ever pursues operating its own shortwave transmitter:
- Frequency coordination goes through the national telecommunications authority of the transmitter's host country
- That authority submits to the ITU Radiocommunication Bureau
- HFCC (High Frequency Coordination Conference) manages the seasonal frequency database: https://new.hfcc.org
- Seasonal schedules (A and B) are coordinated twice yearly
- This process takes 6–12 months minimum and requires technical expertise in HF propagation
- Not relevant for the airtime-lease path; WRMI and WBCQ handle their own ITU coordination

---

## Recommended Action Sequence

1. Email info@wrmi.net (Jeff White) with the content description above. Ask for rate confirmation, digital-mode audio acceptance confirmation, and scheduling options for daily 1-minute slots.

2. Email radiogram@verizon.net (Kim Elliott) to ask whether he would include a BTCPC data segment as a one-time or recurring experiment in Shortwave Radiogram.

3. If WRMI confirms digital-mode audio is acceptable: book a single 1-minute test broadcast. Prepare the MFSK32 audio file. Total cost: $1.00 for the test.

4. If a European/African coverage target is needed: email info@nexus.org and explicitly ask about digital-mode audio content acceptance before requesting a quote.

5. Allan Weiner at WBCQ (wbcq@wbcq.com / 1-207-889-0039) is the backup if WRMI pricing or scheduling does not work.

---

*This document was produced by the Artistic Engineer Agent via primary-source web research on 2026-05-24. Rates and contacts should be verified before any booking commitment.*
