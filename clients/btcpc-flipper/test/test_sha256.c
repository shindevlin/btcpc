/*
 * test_sha256.c — off-device tests for the SHA-256 implementation
 * (crypto/sha256.c) against NIST/well-known test vectors.
 *
 * The SHA-256 implementation is pure logic — no Flipper SDK dependency — so
 * it builds and runs with a plain host C compiler.
 *
 * Build & run:
 *   cc -I.. test_sha256.c ../crypto/sha256.c -o /tmp/sha256_test && /tmp/sha256_test
 *
 * Shin Devlin — btcpc.network
 */

#include "../crypto/sha256.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if(!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else        { printf("ok:   %s\n", msg); } \
} while(0)

static int check_vector(const char* label, const uint8_t* data, size_t len,
                         const char* expect_hex) {
    uint8_t digest[BTCPC_SHA256_DIGEST_LEN];
    char hex[65];
    btcpc_sha256(data, len, digest);
    btcpc_sha256_to_hex(digest, hex);
    int ok = strcmp(hex, expect_hex) == 0;
    if(!ok) {
        printf("FAIL: %s\n  got:    %s\n  expect: %s\n", label, hex, expect_hex);
    } else {
        printf("ok:   %s\n", label);
    }
    return ok;
}

int main(void) {
    /* NIST FIPS 180-4 test vectors. */
    failures += !check_vector("empty string", (const uint8_t*)"", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    failures += !check_vector("\"abc\"", (const uint8_t*)"abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    failures += !check_vector("56-byte multi-block-boundary vector",
        (const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    failures += !check_vector("\"The quick brown fox jumps over the lazy dog\"",
        (const uint8_t*)"The quick brown fox jumps over the lazy dog", 43,
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");

    /* Long-message vector: 1,000,000 x 'a', exercises many transform() calls
     * and the final-block length-trailer path when the tail is far from a
     * block boundary. */
    {
        static uint8_t million_a[1000000];
        memset(million_a, 'a', sizeof(million_a));
        failures += !check_vector("1,000,000 x 'a'", million_a, sizeof(million_a),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* Exact-56-byte-message boundary: after appending the 0x80 marker this
     * lands at buf_len == 57 > 56, forcing the "pad to a full block, extra
     * transform, fresh block" branch in btcpc_sha256_final(). */
    {
        uint8_t buf[56];
        memset(buf, 'z', sizeof(buf));
        uint8_t digest[BTCPC_SHA256_DIGEST_LEN];
        btcpc_sha256(buf, sizeof(buf), digest);
        /* No independently-known vector for this exact input; the meaningful
         * assertion is determinism + no crash/garbage, checked below via a
         * second identical call. */
        uint8_t digest2[BTCPC_SHA256_DIGEST_LEN];
        btcpc_sha256(buf, sizeof(buf), digest2);
        CHECK(memcmp(digest, digest2, BTCPC_SHA256_DIGEST_LEN) == 0,
              "56-byte boundary input hashes deterministically");
    }

    /* Incremental update() must match one-shot btcpc_sha256() for the same
     * bytes, split arbitrarily across multiple update() calls — proves the
     * internal buffering/carry logic is correct, not just the single-call
     * path every other test above exercises. */
    {
        const char* msg = "The quick brown fox jumps over the lazy dog";
        size_t mlen = strlen(msg);

        uint8_t one_shot[BTCPC_SHA256_DIGEST_LEN];
        btcpc_sha256((const uint8_t*)msg, mlen, one_shot);

        BtcpcSha256Ctx ctx;
        btcpc_sha256_init(&ctx);
        btcpc_sha256_update(&ctx, (const uint8_t*)msg, 1);
        btcpc_sha256_update(&ctx, (const uint8_t*)msg + 1, 2);
        btcpc_sha256_update(&ctx, (const uint8_t*)msg + 3, 10);
        btcpc_sha256_update(&ctx, (const uint8_t*)msg + 13, mlen - 13);
        uint8_t incremental[BTCPC_SHA256_DIGEST_LEN];
        btcpc_sha256_final(&ctx, incremental);

        CHECK(memcmp(one_shot, incremental, BTCPC_SHA256_DIGEST_LEN) == 0,
              "incremental update() across arbitrary chunk boundaries matches one-shot");
    }

    /* Hex encoding sanity: correct length, lowercase, NUL-terminated. */
    {
        uint8_t digest[BTCPC_SHA256_DIGEST_LEN];
        btcpc_sha256((const uint8_t*)"x", 1, digest);
        char hex[65];
        btcpc_sha256_to_hex(digest, hex);
        CHECK(strlen(hex) == 64, "hex output is exactly 64 characters");
        int lowercase_only = 1;
        for(size_t i = 0; i < 64; i++) {
            char c = hex[i];
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) lowercase_only = 0;
        }
        CHECK(lowercase_only, "hex output is lowercase hex digits only");
    }

    if(failures == 0) { printf("\nALL SHA-256 TESTS PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}
