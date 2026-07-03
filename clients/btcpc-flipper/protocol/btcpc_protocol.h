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
 *
 * Wire changes in this revision (backward-compatible, additive only):
 *   - Added `BtcpcIrCapture` payload struct + `btcpc_build_ir()` builder for
 *     the previously-reserved-but-unimplemented BTCPC_MSG_IR_CAPTURE (0x06)
 *     message type. No existing message type, struct layout, field, or byte
 *     offset changed. A phone built against the prior revision continues to
 *     parse/verify every existing frame type identically; it simply does not
 *     yet know how to interpret msg_type 0x06's payload shape (it already
 *     handles the *frame envelope* for 0x06 generically for data_hash
 *     purposes — see android/rust/btcpc-miner/src/flipper_rx.rs).
 *
 * Portability note: the real target (ufbt/ARM, GCC-family) uses
 * `__attribute__((packed))`. MSVC (used only for host-side test builds, see
 * test/) doesn't understand that attribute, so BTCPC_PACKED_STRUCT_BEGIN/END
 * switch to `#pragma pack(push,1)` / `pop` under MSVC. Byte layout is
 * identical either way — this only affects which compiler directive achieves
 * it, never the on-wire bytes.
 */

#if defined(_MSC_VER)
#define BTCPC_PACKED_STRUCT_BEGIN __pragma(pack(push, 1)) typedef struct
#define BTCPC_PACKED_STRUCT_END   __pragma(pack(pop))
#else
#define BTCPC_PACKED_STRUCT_BEGIN typedef struct __attribute__((packed))
#define BTCPC_PACKED_STRUCT_END
#endif

#define BTCPC_FRAME_MAGIC  { 'B', 'T', 'P', 'C' }
#define BTCPC_MAGIC_LEN    4
#define BTCPC_SIG_LEN      64
#define BTCPC_MAX_PAYLOAD  472  /* BLE ATT MTU 512 - header size */

/* Message types */
typedef enum {
    /* Flipper → phone */
    BTCPC_MSG_SUBGHZ_OBS = 0x01,  /* Sub-GHz radio observation */
    BTCPC_MSG_RFID_SCAN  = 0x02,  /* RFID/125kHz card scan */
    BTCPC_MSG_NFC_SCAN   = 0x03,  /* NFC/ISO14443 scan */
    BTCPC_MSG_IBUTTON    = 0x04,  /* iButton/1-Wire read */
    BTCPC_MSG_HEARTBEAT  = 0x05,  /* battery, uptime, version */
    BTCPC_MSG_IR_CAPTURE = 0x06,  /* captured IR signal */

    /* Phone → Flipper */
    BTCPC_MSG_ENTRY_HASH = 0x10,  /* chain entry hash to rebroadcast via Sub-GHz */
    BTCPC_MSG_CLOCK_SYNC = 0x11,  /* unix timestamp in ms */
    BTCPC_MSG_GPS        = 0x12,  /* lat/lon from phone GPS */
} BtcpcMsgType;

/*
 * Frame header — packed to avoid padding bytes in the BLE stream.
 *
 * Note: payload_len sits at offset 5 (odd), so __attribute__((packed))
 * is required to prevent the compiler inserting a pad byte before it.
 */
BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  magic[BTCPC_MAGIC_LEN]; /* "BTPC" */
    uint8_t  msg_type;               /* BtcpcMsgType */
    uint16_t payload_len;            /* bytes following the header */
    uint8_t  sig[BTCPC_SIG_LEN];    /* ed25519 signature over payload */
} BtcpcFrameHeader;
BTCPC_PACKED_STRUCT_END

#define BTCPC_FRAME_HEADER_SIZE  sizeof(BtcpcFrameHeader)  /* = 71 bytes */

/* ─── Payload structs (all packed) ─────────────────────────────────────── */

BTCPC_PACKED_STRUCT_BEGIN {
    uint32_t freq_hz;    /* centre frequency in Hz */
    int8_t   rssi_dbm;  /* RSSI in dBm */
    uint8_t  modulation; /* 0=AM, 1=FM, 2=OOK */
    uint8_t  bandwidth;  /* kHz, approximate */
} BtcpcSubGhzObs;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  protocol;   /* 0=EM4100, 1=HID, 2=Indala, 0xFF=raw */
    uint8_t  id[8];      /* card ID, up to 8 bytes */
    uint8_t  id_len;     /* valid bytes in id[] */
} BtcpcRfidScan;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  tech;       /* 0=A, 1=B, 2=F, 3=V */
    uint8_t  uid[10];    /* UID, up to 10 bytes */
    uint8_t  uid_len;    /* valid bytes in uid[] */
    uint8_t  atqa[2];   /* ATQA for type A, else 0 */
    uint8_t  sak;        /* SAK for type A, else 0 */
} BtcpcNfcScan;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  id[8];      /* iButton/Dallas 64-bit ROM code */
    uint8_t  family;     /* family code (id[0]) */
} BtcpcIButton;
BTCPC_PACKED_STRUCT_END

/*
 * BtcpcIrCapture — captured infrared remote signal.
 *
 * Covers the common decoded-protocol case (NEC, Samsung32, RC5, RC6, etc.)
 * via protocol/address/command, matching the Phase 1.2 metadata convention's
 * `ir` payload keys exactly (`protocol, address, command`). When the Flipper
 * cannot decode a known protocol it still captures raw timing; `protocol_id
 * == BTCPC_IR_PROTOCOL_RAW` signals that case and `address`/`command` are 0
 * (the presence event is still meaningful — "some IR signal was seen here" —
 * even without a decode).
 *
 * New struct; does not modify any existing frame payload, so all existing
 * message types (SUBGHZ_OBS, RFID_SCAN, NFC_SCAN, IBUTTON, HEARTBEAT) and
 * their wire layout are unchanged and fully backward-compatible.
 */
BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  protocol_id; /* BtcpcIrProtocol */
    uint32_t address;     /* decoded address, protocol-specific width, 0 if n/a */
    uint32_t command;     /* decoded command, protocol-specific width, 0 if n/a */
} BtcpcIrCapture;
BTCPC_PACKED_STRUCT_END

/* Decoded IR protocol identifiers, matching the Flipper Zero infrared app's
 * supported remote protocol families. RAW (0xFF) means "signal detected but
 * not decoded to a known protocol" — still a valid presence event. */
typedef enum {
    BtcpcIrProtocolNec       = 0,
    BtcpcIrProtocolNecExt    = 1,
    BtcpcIrProtocolSamsung32 = 2,
    BtcpcIrProtocolRc5       = 3,
    BtcpcIrProtocolRc6       = 4,
    BtcpcIrProtocolSirc      = 5,  /* Sony SIRC */
    BtcpcIrProtocolRaw       = 0xFF,
} BtcpcIrProtocol;

BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  battery_pct;    /* 0–100 */
    uint32_t uptime_s;       /* seconds since boot */
    uint8_t  fw_version[8];  /* null-terminated version string */
} BtcpcHeartbeat;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    uint8_t  hash[32];   /* SHA-256 of chain entry to rebroadcast */
} BtcpcEntryHash;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    uint64_t unix_ms;    /* unix time in milliseconds */
} BtcpcClockSync;
BTCPC_PACKED_STRUCT_END

BTCPC_PACKED_STRUCT_BEGIN {
    int32_t  lat_1e7;    /* latitude  * 1e7 (degrees) */
    int32_t  lon_1e7;    /* longitude * 1e7 (degrees) */
    int32_t  alt_mm;     /* altitude in millimetres */
    uint16_t accuracy_m; /* horizontal accuracy in metres */
} BtcpcGps;
BTCPC_PACKED_STRUCT_END

/* ─── Buffer type for a complete framed message ─────────────────────────── */

typedef struct {
    BtcpcFrameHeader hdr;
    uint8_t          payload[BTCPC_MAX_PAYLOAD];
} BtcpcFrame;

/* ─── Public API (implemented in btcpc_protocol.c) ──────────────────────── */

#include "../crypto/ed25519.h"  /* BTCPC_ED25519_SK_LEN / _PK_LEN */

/*
 * btcpc_build_*() — serialise a sensor observation into `frame`, sign the
 * payload with the device secret key `sk`, and return the total framed byte
 * count (header + payload) ready for BLE TX.
 */
size_t btcpc_build_subghz(BtcpcFrame* frame, const BtcpcSubGhzObs* obs,
                          const uint8_t sk[BTCPC_ED25519_SK_LEN]);
size_t btcpc_build_rfid(BtcpcFrame* frame, const BtcpcRfidScan* scan,
                        const uint8_t sk[BTCPC_ED25519_SK_LEN]);
size_t btcpc_build_nfc(BtcpcFrame* frame, const BtcpcNfcScan* scan,
                       const uint8_t sk[BTCPC_ED25519_SK_LEN]);
size_t btcpc_build_ibutton(BtcpcFrame* frame, const BtcpcIButton* btn,
                           const uint8_t sk[BTCPC_ED25519_SK_LEN]);
size_t btcpc_build_heartbeat(BtcpcFrame* frame, const BtcpcHeartbeat* hb,
                             const uint8_t sk[BTCPC_ED25519_SK_LEN]);
size_t btcpc_build_ir(BtcpcFrame* frame, const BtcpcIrCapture* ir,
                      const uint8_t sk[BTCPC_ED25519_SK_LEN]);

/* Verify a received frame's ed25519 signature against public key `pk`. */
bool btcpc_frame_verify(const BtcpcFrameHeader* hdr, const uint8_t* payload,
                        const uint8_t pk[BTCPC_ED25519_PK_LEN]);

/* Parse a raw BLE buffer into a frame header + payload pointer. */
bool btcpc_parse_frame(const uint8_t* buf, size_t buf_len,
                       BtcpcFrameHeader* hdr_out, const uint8_t** payload_out);

/* Phone → Flipper payload parsers. */
bool btcpc_parse_clock_sync(const uint8_t* payload, uint16_t payload_len,
                            uint64_t* unix_ms_out);
bool btcpc_parse_gps(const uint8_t* payload, uint16_t payload_len,
                     BtcpcGps* gps_out);
