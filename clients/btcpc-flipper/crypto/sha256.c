/*
 * sha256.c — minimal, portable SHA-256 (FIPS 180-4) implementation
 *
 * See sha256.h for scope/rationale. Straightforward reference implementation;
 * no attempt at constant-time or hardware acceleration — data_hash is a public
 * integrity value, not a secret, so timing side channels don't apply here.
 *
 * Shin Devlin — btcpc.network
 */

#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(BtcpcSha256Ctx* ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for(size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | ((uint32_t)block[i * 4 + 3]);
    }
    for(size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for(size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void btcpc_sha256_init(BtcpcSha256Ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen   = 0;
    ctx->buf_len  = 0;
}

void btcpc_sha256_update(BtcpcSha256Ctx* ctx, const uint8_t* data, size_t len) {
    ctx->bitlen += (uint64_t)len * 8;

    while(len > 0) {
        size_t take = 64 - ctx->buf_len;
        if(take > len) take = len;

        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += take;
        data += take;
        len -= take;

        if(ctx->buf_len == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void btcpc_sha256_final(BtcpcSha256Ctx* ctx, uint8_t out[BTCPC_SHA256_DIGEST_LEN]) {
    /* Capture the true message bit length BEFORE any padding is mixed in —
     * padding bytes must not count toward the length trailer. */
    uint64_t bitlen = ctx->bitlen;

    /* Append the mandatory 0x80 marker byte directly into the buffer. */
    ctx->buf[ctx->buf_len++] = 0x80;

    if(ctx->buf_len > 56) {
        /* Not enough room left in this block for the 8-byte length trailer —
         * zero-pad to a full block, transform, and start a fresh block. */
        memset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
        sha256_transform(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Zero-pad up to byte offset 56, leaving room for the 8-byte trailer. */
    memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

    /* Big-endian 64-bit message length in bits. */
    for(int i = 0; i < 8; i++) {
        ctx->buf[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));
    }
    sha256_transform(ctx, ctx->buf);
    ctx->buf_len = 0;

    for(size_t i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void btcpc_sha256(const uint8_t* data, size_t len, uint8_t out[BTCPC_SHA256_DIGEST_LEN]) {
    BtcpcSha256Ctx ctx;
    btcpc_sha256_init(&ctx);
    btcpc_sha256_update(&ctx, data, len);
    btcpc_sha256_final(&ctx, out);
}

void btcpc_sha256_to_hex(const uint8_t digest[BTCPC_SHA256_DIGEST_LEN], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for(size_t i = 0; i < BTCPC_SHA256_DIGEST_LEN; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[BTCPC_SHA256_DIGEST_LEN * 2] = '\0';
}
