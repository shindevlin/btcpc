#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * btcpc_protocol.h — BLE message framing between Flipper and phone
 *
 * Wire format:
 *   [BtcpcFrameHeader] [payload bytes]
 *
 * The signature in the header covers the payload bytes only.
 * The phone verifies the signature against the Flipper's on-chain public key
 * before accepting any data.
 *
 * All multi-byte integers are little-endian.
 */

#define BTCPC_FRAME_MAGIC  { 'B', 'T', 'P', 'C' }
#define BTCPC_MAGIC_LEN    4
#define BTCPC_SIG_LEN      64
#define BTCPC_MAX_PAYLOAD  472  /* BLE ATT MTU 512 - header size */

/* Message types */
typedef enum {
    /* Flipper → phone */
    BTCPC_MSG_SUBGHZ_OBS    = 0x01,  /* Sub-GHz radio observation (legacy, single-band RSSI) */
    BTCPC_MSG_RFID_SCAN     = 0x02,  /* RFID/125kHz card scan */
    BTCPC_MSG_NFC_SCAN      = 0x03,  /* NFC/ISO14443 scan */
    BTCPC_MSG_IBUTTON       = 0x04,  /* iButton/1-Wire read */
    BTCPC_MSG_HEARTBEAT     = 0x05,  /* battery, uptime, version */
    BTCPC_MSG_IR_CAPTURE    = 0x06,  /* captured IR signal */
    BTCPC_MSG_SIGN_RESP     = 0x07,  /* Flipper returns Ed25519 signature */
    BTCPC_MSG_SUBGHZ_CENSUS = 0x08,  /* multi-band RF census: event counts + RSSI per band */
    BTCPC_MSG_BLE_ENV       = 0x09,  /* BLE environment scan: nearby advertisement census */
    BTCPC_MSG_READER_DETECT = 0x0A,  /* external reader field detected */
    BTCPC_MSG_USB_SAFETY    = 0x0B,  /* USB juice jacking scan result */

    /* Phone → Flipper */
    BTCPC_MSG_ENTRY_HASH = 0x10,  /* chain entry hash to rebroadcast via Sub-GHz */
    BTCPC_MSG_CLOCK_SYNC = 0x11,  /* unix timestamp in ms */
    BTCPC_MSG_GPS        = 0x12,  /* lat/lon from phone GPS */
    BTCPC_MSG_SIGN_REQ   = 0x13,  /* phone requests Flipper sign a 32-byte digest */
    BTCPC_MSG_SENSOR_REQ = 0x14,  /* phone requests specific sensor capture */
} BtcpcMsgType;

/*
 * Frame header — packed to avoid padding bytes in the BLE stream.
 *
 * Note: payload_len sits at offset 5 (odd), so __attribute__((packed))
 * is required to prevent the compiler inserting a pad byte before it.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[BTCPC_MAGIC_LEN]; /* "BTPC" */
    uint8_t  msg_type;               /* BtcpcMsgType */
    uint16_t payload_len;            /* bytes following the header */
    uint8_t  sig[BTCPC_SIG_LEN];    /* ed25519 signature over payload */
} BtcpcFrameHeader;

#define BTCPC_FRAME_HEADER_SIZE  sizeof(BtcpcFrameHeader)  /* = 71 bytes */

/* ─── Payload structs (all packed) ─────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t freq_hz;    /* centre frequency in Hz */
    int8_t   rssi_dbm;  /* RSSI in dBm */
    uint8_t  modulation; /* 0=AM, 1=FM, 2=OOK */
    uint8_t  bandwidth;  /* kHz, approximate */
} BtcpcSubGhzObs;

/*
 * BtcpcBandCensus — RF activity census for one frequency band.
 *
 * event_count: distinct RSSI excursions ≥ 12 dB above local noise floor.
 *              Each event is one or more devices transmitting.
 *              On 433 MHz OOK this maps to remotes / sensors.
 *              On 868 MHz FSK this includes LoRa, wM-Bus, Z-Wave, Sigfox.
 *
 * long_count:  subset of events lasting ≥ 50 ms.
 *              Short OOK bursts (remotes, sensors) are typically 5–30 ms.
 *              LoRa chirps and FSK data frames run 50–500 ms.
 *              This is the best proxy for LoRa / meter traffic without
 *              decoding the modulation.
 *
 * peak_rssi_dbm:   strongest signal seen during the listen window.
 * noise_floor_dbm: average RSSI during the first 100 ms (quiet calibration).
 */
typedef struct __attribute__((packed)) {
    uint32_t freq_hz;
    uint8_t  event_count;     /* distinct RF events above noise threshold */
    uint8_t  long_count;      /* events ≥ 50 ms (LoRa chirps / FSK frames) */
    int8_t   peak_rssi_dbm;
    int8_t   noise_floor_dbm;
} BtcpcBandCensus;  /* 8 bytes */

#define BTCPC_CENSUS_BANDS 13

/*
 * BtcpcSubGhzCensus — complete multi-band census pushed every scan cycle.
 *
 * Bands (in order): see btcpc_scene_ble.c btcpc_census_subghz() for the
 * full table of frequencies, modulation hints, and listen windows.
 *
 * listen_window_ms: total time spent scanning across all bands.
 *
 * Privacy: no device addresses, no rolling codes, no meter readings.
 * Event counts + signal strength only — describes RF environment density,
 * not individual device identity.
 */
typedef struct __attribute__((packed)) {
    BtcpcBandCensus band[BTCPC_CENSUS_BANDS];
    uint16_t        listen_window_ms;
} BtcpcSubGhzCensus;  /* 13×8 + 2 = 106 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  ad_count;          /* unique advertisement sources in scan window */
    uint8_t  scan_window_s;     /* scan duration in seconds */
    int8_t   rssi_min_dbm;      /* weakest signal seen */
    int8_t   rssi_max_dbm;      /* strongest signal seen */
    int8_t   rssi_avg_dbm;      /* average RSSI (integer) */
    uint8_t  connectable_count; /* advertisements with connectable flag set */
} BtcpcBleEnv;  /* 6 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  reader_type;  /* 0x01=RFID 125kHz, 0x02=NFC 13.56MHz, 0x03=iButton 1-Wire master */
    int8_t   field_rssi;   /* signal strength or 0 if not measurable */
} BtcpcReaderDetect;  /* 2 bytes */

/*
 * BtcpcUsbSafetyResult — BTCPC_MSG_USB_SAFETY (0x0B)
 *
 * verdict:
 *   0 = SAFE        — no USB enumeration in monitoring window
 *   1 = WARNING     — USB enumerated into a data-capable class
 *   2 = ATTACK      — unknown/unexpected USB class detected
 *
 * usb_class: 0x00=none, 0x02=CDC/serial, 0x0B=CCID smart card, 0xFF=unknown
 * enumeration_count: how many times a non-HID config was observed
 * monitor_ms: total monitoring window in milliseconds
 */
typedef struct __attribute__((packed)) {
    uint8_t  verdict;
    uint8_t  usb_class;
    uint8_t  enumeration_count;
    uint16_t monitor_ms;
} BtcpcUsbSafetyResult;  /* BTCPC_MSG_USB_SAFETY (0x0B), 5 bytes */

/*
 * Privacy design — "what, not which":
 *
 * Card IDs, UIDs, ROM codes, and IR address/command codes are NOT transmitted.
 * They enable cloning, replay, and tracking if published on a public blockchain.
 *
 * What IS transmitted:
 *   - protocol/technology TYPE (public standard name, e.g. EM4100, MIFARE)
 *   - obs_id: sign(flipper_sk, "obs:" || sensor_type || protocol || epoch_minute)[0..16]
 *     Time-bounded (changes every minute), keyed to device identity, not
 *     reversible to any credential. Serves as cryptographic proof of presence.
 */

typedef struct __attribute__((packed)) {
    uint8_t  protocol;    /* 0=EM4100, 1=HID, 2=Indala, 3=Hitag, 0xFF=unknown */
    uint8_t  obs_id[16];  /* sign(sk,"obs:"+type+proto+epoch_min)[0..16] */
} BtcpcRfidScan;           /* BTCPC_MSG_RFID_SCAN (0x02), 17 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  tech;        /* 0=TypeA, 1=TypeB, 2=TypeF, 3=TypeV */
    uint8_t  tag_family;  /* 0=unknown, 1=MIFARE, 2=NTAG, 3=DESFire, 4=FeliCa, 5=ISO15693 */
    uint8_t  obs_id[16];  /* ephemeral proof */
} BtcpcNfcScan;            /* BTCPC_MSG_NFC_SCAN (0x03), 18 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  family;      /* Dallas 1-Wire family code (public, e.g. 0x01=DS1990A, 0x28=DS18B20) */
    uint8_t  obs_id[16];  /* ephemeral proof */
} BtcpcIButton;            /* BTCPC_MSG_IBUTTON (0x04), 17 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  protocol;    /* 0=NEC, 1=Samsung32, 2=RC6, 3=RC5, 4=SIRC, 5=Kaseikyo, 0xFF=raw/unknown */
    uint8_t  obs_id[16];  /* ephemeral proof */
} BtcpcIrCapture;          /* BTCPC_MSG_IR_CAPTURE (0x06), 17 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  battery_pct;    /* 0–100 */
    uint32_t uptime_s;       /* seconds since boot */
    uint8_t  fw_version[8];  /* null-terminated version string */
} BtcpcHeartbeat;

typedef struct __attribute__((packed)) {
    uint8_t  hash[32];   /* SHA-256 of chain entry to rebroadcast */
} BtcpcEntryHash;

typedef struct __attribute__((packed)) {
    uint64_t unix_ms;    /* unix time in milliseconds */
} BtcpcClockSync;

typedef struct __attribute__((packed)) {
    int32_t  lat_1e7;    /* latitude  * 1e7 (degrees) */
    int32_t  lon_1e7;    /* longitude * 1e7 (degrees) */
    int32_t  alt_mm;     /* altitude in millimetres */
    uint16_t accuracy_m; /* horizontal accuracy in metres */
} BtcpcGps;

/* Signing delegation — phone asks Flipper to sign with its identity key.
 * request_id correlates async BLE responses to their requests.
 * purpose: 0x01 = Transfer entry, 0x02 = generic digest (future). */
typedef struct __attribute__((packed)) {
    uint32_t request_id;  /* caller-assigned correlation ID */
    uint8_t  purpose;     /* signing purpose code */
    uint8_t  digest[32];  /* 32-byte payload to sign */
} BtcpcSignReq;           /* BTCPC_MSG_SIGN_REQ (0x13), 37 bytes */

/* status: 0x00 = ok, 0x01 = busy, 0x02 = rejected by user, 0xFF = error */
typedef struct __attribute__((packed)) {
    uint32_t request_id;  /* echoes the request_id from BtcpcSignReq */
    uint8_t  status;      /* result code */
    uint8_t  sig[64];     /* Ed25519 signature (valid only when status==0) */
} BtcpcSignResp;          /* BTCPC_MSG_SIGN_RESP (0x07), 69 bytes */

/* Phone requests a specific sensor to observe and push a result frame.
 * sensor_type: one of BTCPC_MSG_SUBGHZ_OBS, RFID_SCAN, NFC_SCAN, HEARTBEAT.
 * duration_ms: observation window; 0 uses a sensible default per sensor type. */
typedef struct __attribute__((packed)) {
    uint8_t  sensor_type;   /* BtcpcMsgType of the desired observation */
    uint16_t duration_ms;   /* capture window in ms (0 = default) */
} BtcpcSensorReq;           /* BTCPC_MSG_SENSOR_REQ (0x14), 3 bytes */

/* ─── Buffer type for a complete framed message ─────────────────────────── */

typedef struct {
    BtcpcFrameHeader hdr;
    uint8_t          payload[BTCPC_MAX_PAYLOAD];
} BtcpcFrame;

/*
 * Frame builder and parser declarations.
 *
 * The sk/pk array parameters use a raw size (64/32) rather than
 * BTCPC_ED25519_SK_LEN / BTCPC_ED25519_PK_LEN to avoid pulling
 * crypto/ed25519.h into this header with a path that varies by
 * include site.  The sizes are identical — see crypto/ed25519.h.
 * Callers must include crypto/ed25519.h themselves for the constants.
 */

/* ─── Frame builders (Flipper → phone) ──────────────────────────────────── */
/* All return total bytes (header + payload) ready for BLE TX. */

size_t btcpc_build_subghz(BtcpcFrame* frame, const BtcpcSubGhzObs* obs, const uint8_t sk[64]);
size_t btcpc_build_subghz_census(BtcpcFrame* frame, const BtcpcSubGhzCensus* census, const uint8_t sk[64]);
size_t btcpc_build_rfid(BtcpcFrame* frame, const BtcpcRfidScan* scan, const uint8_t sk[64]);
size_t btcpc_build_nfc(BtcpcFrame* frame, const BtcpcNfcScan* scan, const uint8_t sk[64]);
size_t btcpc_build_ibutton(BtcpcFrame* frame, const BtcpcIButton* btn, const uint8_t sk[64]);
size_t btcpc_build_heartbeat(BtcpcFrame* frame, const BtcpcHeartbeat* hb, const uint8_t sk[64]);
size_t btcpc_build_ir(BtcpcFrame* frame, const BtcpcIrCapture* ir, const uint8_t sk[64]);
size_t btcpc_build_ble_env(BtcpcFrame* frame, const BtcpcBleEnv* env, const uint8_t sk[64]);
size_t btcpc_build_reader_detect(BtcpcFrame* frame, const BtcpcReaderDetect* rd, const uint8_t sk[64]);
size_t btcpc_build_usb_safety(BtcpcFrame* frame, const BtcpcUsbSafetyResult* res, const uint8_t sk[64]);

/* ─── Frame parsers ──────────────────────────────────────────────────────── */

bool   btcpc_frame_verify(const BtcpcFrameHeader* hdr, const uint8_t* payload, const uint8_t pk[32]);
bool   btcpc_parse_frame(const uint8_t* buf, size_t buf_len, BtcpcFrameHeader* hdr_out, const uint8_t** payload_out);
bool   btcpc_parse_clock_sync(const uint8_t* payload, uint16_t payload_len, uint64_t* unix_ms_out);
bool   btcpc_parse_gps(const uint8_t* payload, uint16_t payload_len, BtcpcGps* gps_out);
