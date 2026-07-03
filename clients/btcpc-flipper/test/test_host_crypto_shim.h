/*
 * test_host_crypto_shim.h — host-side randombytes() for TweetNaCl in tests
 *
 * tweetnacl.c requires the caller to provide `void randombytes(uint8_t*,
 * uint64_t)`. On the Flipper this comes from crypto/ed25519.c, which pulls in
 * furi_hal_random.h — unavailable on a host build. Test binaries that link
 * tweetnacl.c directly (bypassing ed25519.c, which is Flipper-only) must
 * supply their own randombytes(). This one is a small deterministic-seedable
 * PRNG (xorshift128) — NOT cryptographically secure, and must never be used
 * outside test binaries. Production signing always goes through
 * crypto/ed25519.c on-device, which is unmodified by this shim.
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Seed the test PRNG. Call once per test binary (or per test, for
 * isolation) before any TweetNaCl keypair/sign call that needs randomness. */
void test_prng_seed(uint32_t seed);
