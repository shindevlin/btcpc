#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * sha256.h — minimal, portable SHA-256 (FIPS 180-4)
 *
 * Public-domain-style implementation, self-contained (no external deps),
 * matching the project's existing TweetNaCl vendoring convention. Used to
 * compute `data_hash` for every capture bound for a chain SensorReading /
 * SensorDataCommit, over the *exact* payload bytes the Flipper signs with
 * its device ed25519 key (see protocol/btcpc_protocol.c).
 *
 * Builds identically on-device (Flipper ufbt/ARM) and on host (test/), since
 * it only uses stdint.h / stddef.h and plain integer arithmetic — no
 * furi_hal, no platform intrinsics.
 *
 * Shin Devlin — btcpc.network
 */

#define BTCPC_SHA256_DIGEST_LEN 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buf_len;
} BtcpcSha256Ctx;

void btcpc_sha256_init(BtcpcSha256Ctx* ctx);
void btcpc_sha256_update(BtcpcSha256Ctx* ctx, const uint8_t* data, size_t len);
void btcpc_sha256_final(BtcpcSha256Ctx* ctx, uint8_t out[BTCPC_SHA256_DIGEST_LEN]);

/* Convenience one-shot: hash `len` bytes of `data` into `out`. */
void btcpc_sha256(const uint8_t* data, size_t len, uint8_t out[BTCPC_SHA256_DIGEST_LEN]);

/* Hex-encode a digest into `out` (65 bytes: 64 hex chars + NUL). */
void btcpc_sha256_to_hex(const uint8_t digest[BTCPC_SHA256_DIGEST_LEN], char out[65]);
