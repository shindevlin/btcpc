# BTCPC Hardware Shopping List

Last updated: 2026-05-24

## Buy Now

### WiFi HaLow Adapter — ALFA AHPI7292S
- **Purpose:** Sub-1GHz WiFi (802.11ah), 1–3km range, standard Linux networking — libp2p QUIC works over it with zero code changes
- **Target node:** Nebra Pi (192.168.68.75)
- **Price:** ~$60
- **Buy from:** https://store.rokland.com (search AHPI7292S) or SparkFun
- **Status:** GO — morse_driver v1.17.9 compiled clean against Nebra kernel 6.12.47+rpt-rpi-v8 (verified 2026-05-24)
- **Notes:** USB adapter (not a HAT). Requires morse_driver DKMS package + separate Morse Micro firmware blobs. Confirm bus type is USB (CONFIG_MORSE_USB=y) before writing final dkms.conf.

### RTL-SDR Blog V3 Dongle
- **Purpose:** Receive BTCPC shortwave beacon (MFSK32 tones), decode with Fldigi. Required to verify the WRMI test broadcast works.
- **Price:** ~$30
- **Buy from:** https://www.rtl-sdr.com/buy-rtl-sdr-dvb-t-dongles/
- **Status:** Buy alongside any WRMI airtime booking
- **Notes:** Comes with antenna. Works on Linux/Mac/Windows. Pair with Gqrx + Fldigi (free software).

### Wire Antenna for Shortwave Receive
- **Purpose:** Receive HF shortwave (7–21 MHz) for beacon decode testing
- **Price:** ~$0–15 (32 metres of hookup wire, run out a window)
- **Status:** Can DIY — a random wire 10m+ long works for receive
- **Notes:** Not needed if RTL-SDR kit includes a telescoping antenna, but a longer wire improves HF reception significantly

---

## Buy When Ready (Phase 3 — LoRa/Meshtastic serial bridge)

### Meshtastic Node — TTGO T-Beam v1.2 or Heltec LoRa32 v3
- **Purpose:** Phase 3 transport cascade — LoRa/Meshtastic serial bridge for relaying BTCPC entry hashes over LoRa mesh without a full gateway
- **Price:** $25–45
- **Buy from:** AliExpress, Amazon, or https://www.lilygo.cc/
- **Status:** Waiting — implement Phase 3 `lora.rs` bridge module first, then test with hardware
- **Notes:** TTGO T-Beam preferred (has GPS + 18650 battery holder). 915MHz for Americas, 868MHz for EU/Ireland.

---

## Consider (Research Phase)

### Amateur Radio License Exam
- **Purpose:** Legal cover for JS8Call HF epoch-seal beaconing in Ireland or Panama
- **Price:** ~$35 exam fee (Ireland: ComReg HAREC exam)
- **Status:** Research — read hf-radio-legal-assessment.md first. Required if running JS8Call transmitter. Not needed for shortwave receive only.
- **Notes:** Ireland is the most permissive jurisdiction. Exam is multiple choice, self-study via IRTS.ie.

### HF Transceiver (JS8Call transmit, future)
- **Purpose:** Transmit BTCPC epoch seal hashes over shortwave — continental / intercontinental range via skywave
- **Price:** $400–1,200 (Xiegu G90, IC-7300, or QRP kit)
- **Status:** Future — only relevant after: (a) ham licence obtained, (b) legal assessment conditions confirmed met, (c) WRMI partnership established as the primary broadcast path
- **Notes:** Do not buy until WRMI path is confirmed insufficient. Buying WRMI airtime at $1/min is far cheaper than owning a transmitter for occasional use.

---

## Ruled Out

| Item | Reason |
|------|--------|
| Belize broadcast licence | Two regulators, discretionary fees, no SW infrastructure — see docs/designs/hf-radio-legal-assessment.md |
| Othernet / Dreamcatcher receiver | Service shut down November 2025 |
| goTenna Mesh | Discontinued |
