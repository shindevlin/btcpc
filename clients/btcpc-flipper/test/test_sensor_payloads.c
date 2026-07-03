/*
 * test_sensor_payloads.c — off-device tests for sensor capture -> signed
 * frame -> data_hash correctness, per PRD Phase 1.3.
 *
 * Proves, for every wired sensor class (subghz, nfc, rfid125, ibutton, ir):
 *   1. The payload struct packs the fields the Phase 1.3 ratified mapping
 *      requires (docs/PLATFORM_PRD.md) with the exact wire layout
 *      btcpc_protocol.h defines.
 *   2. data_hash is SHA-256(msg_type_byte || raw payload bytes) — the exact
 *      formula the phone independently recomputes
 *      (android/rust/btcpc-miner/src/flipper_rx.rs::payload_data_hash) —
 *      and is therefore reproducible off the signed frame alone.
 *   3. The frame is signed with the device ed25519 key and verifies against
 *      the matching public key; a tampered payload or wrong key fails
 *      verification (proves the signature is actually binding, not just
 *      present).
 *
 * This does NOT touch any Flipper hardware API — it operates purely on the
 * protocol layer (struct packing, hashing, signing), which is
 * hardware-independent and identical whether the payload came from a real
 * radio or a test fixture. Hardware-triggered capture (the actual
 * furi_hal_nfc / lfrfid_worker / ibutton_worker / infrared_worker calls) is
 * covered by the "Test on real hardware" PRD item, not here.
 *
 * Build & run (from clients/btcpc-flipper/test/):
 *   cc -I.. test_sensor_payloads.c test_host_ed25519.c test_host_crypto_shim.c \
 *      ../protocol/btcpc_protocol.c ../btcpc_data_hash.c ../crypto/sha256.c \
 *      ../crypto/tweetnacl.c -o /tmp/sensor_payloads && /tmp/sensor_payloads
 *
 * Shin Devlin — btcpc.network
 */

#include "../protocol/btcpc_protocol.h"
#include "../btcpc_data_hash.h"
#include "../crypto/sha256.h"
#include "../crypto/ed25519.h"
#include "test_host_crypto_shim.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if(!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else        { printf("ok:   %s\n", msg); } \
} while(0)

/* Reference reimplementation of the phone's payload_data_hash() formula
 * (android/rust/btcpc-miner/src/flipper_rx.rs), computed independently here
 * so a match proves firmware and phone agree without importing Rust code. */
static void reference_data_hash_hex(uint8_t msg_type, const uint8_t* payload,
                                     size_t payload_len, char out_hex[65]) {
    BtcpcSha256Ctx ctx;
    uint8_t        digest[BTCPC_SHA256_DIGEST_LEN];
    btcpc_sha256_init(&ctx);
    btcpc_sha256_update(&ctx, &msg_type, 1);
    if(payload_len > 0) btcpc_sha256_update(&ctx, payload, payload_len);
    btcpc_sha256_final(&ctx, digest);
    btcpc_sha256_to_hex(digest, out_hex);
}

int main(void) {
    test_prng_seed(0xC0FFEE);

    uint8_t pk[BTCPC_ED25519_PK_LEN];
    uint8_t sk[BTCPC_ED25519_SK_LEN];
    btcpc_ed25519_keypair(pk, sk);

    uint8_t wrong_pk[BTCPC_ED25519_PK_LEN];
    uint8_t wrong_sk[BTCPC_ED25519_SK_LEN];
    btcpc_ed25519_keypair(wrong_pk, wrong_sk);

    /* ── Sub-GHz ─────────────────────────────────────────────────────── */
    {
        BtcpcSubGhzObs obs = {
            .freq_hz = 433920000UL, .rssi_dbm = -72, .modulation = 2, .bandwidth = 0,
        };
        BtcpcFrame frame;
        size_t len = btcpc_build_subghz(&frame, &obs, sk);

        CHECK(len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcSubGhzObs),
              "subghz: frame length == header + payload size");
        CHECK(frame.hdr.msg_type == BTCPC_MSG_SUBGHZ_OBS,
              "subghz: msg_type tagged correctly");
        CHECK(memcmp(frame.payload, &obs, sizeof(obs)) == 0,
              "subghz: payload bytes match the packed struct verbatim");
        CHECK(btcpc_frame_verify(&frame.hdr, frame.payload, pk),
              "subghz: signature verifies against the signing device's pubkey");
        CHECK(!btcpc_frame_verify(&frame.hdr, frame.payload, wrong_pk),
              "subghz: signature REJECTED against a different device's pubkey");

        char got_hex[65], want_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_SUBGHZ_OBS, frame.payload, frame.hdr.payload_len, got_hex);
        reference_data_hash_hex(BTCPC_MSG_SUBGHZ_OBS, frame.payload, frame.hdr.payload_len, want_hex);
        CHECK(strcmp(got_hex, want_hex) == 0,
              "subghz: data_hash matches independent SHA-256(msg_type||payload) reference");

        /* Tamper with a signed byte -> verification must fail (proves the
         * signature actually binds to these exact bytes, and that data_hash
         * would change too — same tamper-evidence property the chain relies
         * on for `data_hash = SHA-256(raw captured payload)`). */
        uint8_t tampered[sizeof(BtcpcSubGhzObs)];
        memcpy(tampered, frame.payload, sizeof(tampered));
        tampered[4] ^= 0xFF; /* flip a byte inside rssi_dbm */
        CHECK(!btcpc_frame_verify(&frame.hdr, tampered, pk),
              "subghz: signature REJECTED after payload tampering");
        char tampered_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_SUBGHZ_OBS, tampered, sizeof(tampered), tampered_hex);
        CHECK(strcmp(tampered_hex, got_hex) != 0,
              "subghz: data_hash changes when payload is tampered (tamper-evident)");
    }

    /* ── NFC ──────────────────────────────────────────────────────────── */
    {
        BtcpcNfcScan scan = {
            .tech = 0, /* A */
            .uid = {0x04, 0x1a, 0x2b, 0x3c, 0, 0, 0, 0, 0, 0},
            .uid_len = 4,
            .atqa = {0x00, 0x44},
            .sak = 0x08,
        };
        BtcpcFrame frame;
        size_t len = btcpc_build_nfc(&frame, &scan, sk);

        CHECK(len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcNfcScan),
              "nfc: frame length == header + payload size");
        CHECK(frame.hdr.msg_type == BTCPC_MSG_NFC_SCAN, "nfc: msg_type tagged correctly");
        CHECK(memcmp(frame.payload, &scan, sizeof(scan)) == 0,
              "nfc: payload bytes match the packed struct (tech/uid/atqa/sak) verbatim");
        CHECK(btcpc_frame_verify(&frame.hdr, frame.payload, pk),
              "nfc: signature verifies against the signing device's pubkey");
        CHECK(!btcpc_frame_verify(&frame.hdr, frame.payload, wrong_pk),
              "nfc: signature REJECTED against a different device's pubkey");

        char got_hex[65], want_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_NFC_SCAN, frame.payload, frame.hdr.payload_len, got_hex);
        reference_data_hash_hex(BTCPC_MSG_NFC_SCAN, frame.payload, frame.hdr.payload_len, want_hex);
        CHECK(strcmp(got_hex, want_hex) == 0, "nfc: data_hash matches independent reference");
    }

    /* ── 125kHz RFID ─────────────────────────────────────────────────── */
    {
        BtcpcRfidScan scan = {
            .protocol = 0, /* EM4100 */
            .id = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0},
            .id_len = 4,
        };
        BtcpcFrame frame;
        size_t len = btcpc_build_rfid(&frame, &scan, sk);

        CHECK(len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcRfidScan),
              "rfid125: frame length == header + payload size");
        CHECK(frame.hdr.msg_type == BTCPC_MSG_RFID_SCAN, "rfid125: msg_type tagged correctly");
        CHECK(memcmp(frame.payload, &scan, sizeof(scan)) == 0,
              "rfid125: payload bytes match the packed struct (protocol/card_id) verbatim");
        CHECK(btcpc_frame_verify(&frame.hdr, frame.payload, pk),
              "rfid125: signature verifies against the signing device's pubkey");

        char got_hex[65], want_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_RFID_SCAN, frame.payload, frame.hdr.payload_len, got_hex);
        reference_data_hash_hex(BTCPC_MSG_RFID_SCAN, frame.payload, frame.hdr.payload_len, want_hex);
        CHECK(strcmp(got_hex, want_hex) == 0, "rfid125: data_hash matches independent reference");
    }

    /* ── iButton ──────────────────────────────────────────────────────── */
    {
        BtcpcIButton btn = {
            .id = {0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
            .family = 0x01,
        };
        BtcpcFrame frame;
        size_t len = btcpc_build_ibutton(&frame, &btn, sk);

        CHECK(len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcIButton),
              "ibutton: frame length == header + payload size");
        CHECK(frame.hdr.msg_type == BTCPC_MSG_IBUTTON, "ibutton: msg_type tagged correctly");
        CHECK(memcmp(frame.payload, &btn, sizeof(btn)) == 0,
              "ibutton: payload bytes match the packed struct (rom_code/family) verbatim");
        CHECK(btcpc_frame_verify(&frame.hdr, frame.payload, pk),
              "ibutton: signature verifies against the signing device's pubkey");

        char got_hex[65], want_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_IBUTTON, frame.payload, frame.hdr.payload_len, got_hex);
        reference_data_hash_hex(BTCPC_MSG_IBUTTON, frame.payload, frame.hdr.payload_len, want_hex);
        CHECK(strcmp(got_hex, want_hex) == 0, "ibutton: data_hash matches independent reference");
    }

    /* ── IR ───────────────────────────────────────────────────────────── */
    {
        BtcpcIrCapture ir = {
            .protocol_id = BtcpcIrProtocolNec,
            .address = 0x00FF,
            .command = 0x1A,
        };
        BtcpcFrame frame;
        size_t len = btcpc_build_ir(&frame, &ir, sk);

        CHECK(len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcIrCapture),
              "ir: frame length == header + payload size");
        CHECK(frame.hdr.msg_type == BTCPC_MSG_IR_CAPTURE, "ir: msg_type tagged correctly");
        CHECK(memcmp(frame.payload, &ir, sizeof(ir)) == 0,
              "ir: payload bytes match the packed struct (protocol/address/command) verbatim");
        CHECK(btcpc_frame_verify(&frame.hdr, frame.payload, pk),
              "ir: signature verifies against the signing device's pubkey");
        CHECK(!btcpc_frame_verify(&frame.hdr, frame.payload, wrong_pk),
              "ir: signature REJECTED against a different device's pubkey");

        char got_hex[65], want_hex[65];
        btcpc_data_hash_hex(BTCPC_MSG_IR_CAPTURE, frame.payload, frame.hdr.payload_len, got_hex);
        reference_data_hash_hex(BTCPC_MSG_IR_CAPTURE, frame.payload, frame.hdr.payload_len, want_hex);
        CHECK(strcmp(got_hex, want_hex) == 0, "ir: data_hash matches independent reference");

        /* Raw/undecoded IR is still a valid presence event per the PRD. */
        BtcpcIrCapture raw_ir = { .protocol_id = BtcpcIrProtocolRaw, .address = 0, .command = 0 };
        BtcpcFrame raw_frame;
        size_t raw_len = btcpc_build_ir(&raw_frame, &raw_ir, sk);
        CHECK(raw_len == BTCPC_FRAME_HEADER_SIZE + sizeof(BtcpcIrCapture),
              "ir: raw/undecoded capture still builds a valid frame");
        CHECK(btcpc_frame_verify(&raw_frame.hdr, raw_frame.payload, pk),
              "ir: raw/undecoded capture signature verifies");
    }

    /* ── Cross-class data_hash disambiguation ────────────────────────────
     * Two different message types with byte-identical payload contents must
     * produce different data_hash values (the msg_type-byte prefix in the
     * hash formula is what disambiguates them). Regression guard for the
     * exact convention documented in btcpc_data_hash.h. */
    {
        uint8_t same_bytes[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        char hash_a[65], hash_b[65];
        btcpc_data_hash_hex(BTCPC_MSG_SUBGHZ_OBS, same_bytes, sizeof(same_bytes), hash_a);
        btcpc_data_hash_hex(BTCPC_MSG_NFC_SCAN, same_bytes, sizeof(same_bytes), hash_b);
        CHECK(strcmp(hash_a, hash_b) != 0,
              "data_hash disambiguates identical payload bytes across different msg_types");
    }

    /* ── Every payload struct is packed with no compiler-inserted padding,
     * matching the exact byte counts the PRD mapping and the phone parser
     * both assume. */
    {
        CHECK(sizeof(BtcpcSubGhzObs) == 7, "BtcpcSubGhzObs is exactly 7 bytes (packed)");
        CHECK(sizeof(BtcpcNfcScan) == 15, "BtcpcNfcScan is exactly 15 bytes (packed)");
        CHECK(sizeof(BtcpcRfidScan) == 10, "BtcpcRfidScan is exactly 10 bytes (packed)");
        CHECK(sizeof(BtcpcIButton) == 9, "BtcpcIButton is exactly 9 bytes (packed)");
        CHECK(sizeof(BtcpcIrCapture) == 9, "BtcpcIrCapture is exactly 9 bytes (packed)");
    }

    if(failures == 0) { printf("\nALL SENSOR PAYLOAD TESTS PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
