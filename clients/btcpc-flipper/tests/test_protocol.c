/*
 * test_protocol.c — Host-side unit tests for btcpc_protocol.c
 *
 * Runs on x86 Linux with no Flipper hardware. Tests frame building,
 * magic validation, payload length encoding, and round-trip parsing.
 *
 * Build + run: make -C tests test
 */

#include "furi_stub.h"
#include "../protocol/btcpc_protocol.h"
#include "../crypto/ed25519.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* ── Test helpers ────────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_pass = 0;

#define TEST(name) \
    static void test_##name(void)

#define RUN(name) do { \
    tests_run++; \
    printf("  %-50s", #name); \
    test_##name(); \
    tests_pass++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if((a) != (b)) { \
        printf("FAIL\n    expected %lld, got %lld at line %d\n", \
               (long long)(b), (long long)(a), __LINE__); \
        exit(1); \
    } \
} while(0)

#define ASSERT_MEM(ptr, val, len) do { \
    for(size_t _i = 0; _i < (len); _i++) { \
        if(((uint8_t*)(ptr))[_i] != (val)) { \
            printf("FAIL\n    byte[%zu] = 0x%02x, expected 0x%02x at line %d\n", \
                   _i, ((uint8_t*)(ptr))[_i], (uint8_t)(val), __LINE__); \
            exit(1); \
        } \
    } \
} while(0)

/* ── Keygen helper ───────────────────────────────────────────────────────── */

static void gen_keypair(uint8_t pk[32], uint8_t sk[64]) {
    btcpc_ed25519_keypair(pk, sk);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

TEST(header_size) {
    /* Frame header must be exactly 71 bytes: 4+1+2+64 */
    ASSERT_EQ(sizeof(BtcpcFrameHeader), 71);
    ASSERT_EQ(BTCPC_FRAME_HEADER_SIZE, 71);
}

TEST(magic_bytes) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);

    BtcpcFrame frame;
    BtcpcHeartbeat hb = {0};
    hb.battery_pct = 85;
    hb.uptime_s    = 3600;
    btcpc_build_heartbeat(&frame, &hb, sk);

    ASSERT_EQ(frame.hdr.magic[0], 'B');
    ASSERT_EQ(frame.hdr.magic[1], 'T');
    ASSERT_EQ(frame.hdr.magic[2], 'P');
    ASSERT_EQ(frame.hdr.magic[3], 'C');
}

TEST(heartbeat_msg_type) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {.battery_pct = 90, .uptime_s = 100};
    btcpc_build_heartbeat(&frame, &hb, sk);
    ASSERT_EQ(frame.hdr.msg_type, BTCPC_MSG_HEARTBEAT);
}

TEST(heartbeat_payload_len) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {0};
    btcpc_build_heartbeat(&frame, &hb, sk);
    ASSERT_EQ(frame.hdr.payload_len, sizeof(BtcpcHeartbeat));
}

TEST(heartbeat_payload_roundtrip) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {.battery_pct = 77, .uptime_s = 9999};
    memcpy(hb.fw_version, "0.1.0\0\0\0", 8);
    btcpc_build_heartbeat(&frame, &hb, sk);

    BtcpcHeartbeat* parsed = (BtcpcHeartbeat*)frame.payload;
    ASSERT_EQ(parsed->battery_pct, 77);
    ASSERT_EQ(parsed->uptime_s, 9999);
}

TEST(subghz_obs_roundtrip) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcSubGhzObs obs = {
        .freq_hz   = 433920000,
        .rssi_dbm  = -72,
        .modulation = 2,
        .bandwidth  = 200,
    };
    btcpc_build_subghz(&frame, &obs, sk);

    ASSERT_EQ(frame.hdr.msg_type, BTCPC_MSG_SUBGHZ_OBS);
    ASSERT_EQ(frame.hdr.payload_len, sizeof(BtcpcSubGhzObs));
    BtcpcSubGhzObs* p = (BtcpcSubGhzObs*)frame.payload;
    ASSERT_EQ(p->freq_hz,   433920000U);
    ASSERT_EQ(p->rssi_dbm,  -72);
    ASSERT_EQ(p->modulation, 2);
}

TEST(usb_safety_roundtrip) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcUsbSafetyResult res = {
        .verdict          = 2,  /* ATTACK */
        .usb_class        = 0x02,
        .enumeration_count = 3,
        .monitor_ms       = 10000,
    };
    btcpc_build_usb_safety(&frame, &res, sk);

    ASSERT_EQ(frame.hdr.msg_type, BTCPC_MSG_USB_SAFETY);
    ASSERT_EQ(frame.hdr.payload_len, sizeof(BtcpcUsbSafetyResult));
    BtcpcUsbSafetyResult* p = (BtcpcUsbSafetyResult*)frame.payload;
    ASSERT_EQ(p->verdict, 2);
    ASSERT_EQ(p->enumeration_count, 3);
}

TEST(signature_not_all_zeros) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {.battery_pct = 50};
    btcpc_build_heartbeat(&frame, &hb, sk);

    /* Signature must not be all zeros after signing */
    int all_zero = 1;
    for(int i = 0; i < 64; i++) {
        if(frame.hdr.sig[i] != 0) { all_zero = 0; break; }
    }
    ASSERT_EQ(all_zero, 0);
}

TEST(signature_verifies) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {.battery_pct = 50, .uptime_s = 1234};
    size_t frame_len = btcpc_build_heartbeat(&frame, &hb, sk);
    (void)frame_len;

    /* Verify: ed25519_sign(payload) should verify against pk */
    /* btcpc_ed25519_verify returns 0 on success (TweetNaCl convention) */
    int rc = btcpc_ed25519_verify(
        frame.hdr.sig,
        frame.payload,
        frame.hdr.payload_len,
        pk);
    ASSERT_EQ(rc, 0);
}

TEST(signature_rejects_tampered_payload) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {.battery_pct = 50};
    btcpc_build_heartbeat(&frame, &hb, sk);

    frame.payload[0] ^= 0xFF;

    /* Tampered payload must fail verification (returns -1) */
    int rc = btcpc_ed25519_verify(
        frame.hdr.sig,
        frame.payload,
        frame.hdr.payload_len,
        pk);
    ASSERT_EQ(rc, -1);
}

TEST(payload_len_little_endian) {
    uint8_t pk[32], sk[64];
    gen_keypair(pk, sk);
    BtcpcFrame frame;
    BtcpcHeartbeat hb = {0};
    btcpc_build_heartbeat(&frame, &hb, sk);

    /* payload_len must encode little-endian in raw bytes */
    uint8_t* raw = (uint8_t*)&frame;
    uint16_t len_from_bytes = (uint16_t)raw[5] | ((uint16_t)raw[6] << 8);
    ASSERT_EQ(len_from_bytes, sizeof(BtcpcHeartbeat));
}

TEST(census_payload_size) {
    /* Census: 13 bands × 8 bytes + 2 bytes listen_window = 106 bytes */
    ASSERT_EQ(sizeof(BtcpcBandCensus), 8);
    ASSERT_EQ(13 * sizeof(BtcpcBandCensus) + 2, 106);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\nbtcpc_protocol host-side tests\n");
    printf("══════════════════════════════════════════════════════\n");

    RUN(header_size);
    RUN(magic_bytes);
    RUN(heartbeat_msg_type);
    RUN(heartbeat_payload_len);
    RUN(heartbeat_payload_roundtrip);
    RUN(subghz_obs_roundtrip);
    RUN(usb_safety_roundtrip);
    RUN(signature_not_all_zeros);
    RUN(signature_verifies);
    RUN(signature_rejects_tampered_payload);
    RUN(payload_len_little_endian);
    RUN(census_payload_size);

    printf("══════════════════════════════════════════════════════\n");
    printf("%d/%d tests passed\n\n", tests_pass, tests_run);
    return tests_pass == tests_run ? 0 : 1;
}
