/*
 * test_host_crypto_shim.c — see test_host_crypto_shim.h
 *
 * Shin Devlin — btcpc.network
 */

#include "test_host_crypto_shim.h"

/* xorshift128 state, fixed non-zero default so tests are reproducible even
 * if a test forgets to call test_prng_seed(). */
static uint32_t s_state[4] = {0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0x85a308d3u};

void test_prng_seed(uint32_t seed) {
    s_state[0] = seed ^ 0x9e3779b9u;
    s_state[1] = seed ^ 0x243f6a88u;
    s_state[2] = seed ^ 0xb7e15162u;
    s_state[3] = seed ^ 0x85a308d3u;
    if(s_state[0] == 0) s_state[0] = 1;
}

static uint32_t xorshift128(void) {
    uint32_t t = s_state[3];
    uint32_t s = s_state[0];
    s_state[3] = s_state[2];
    s_state[2] = s_state[1];
    s_state[1] = s;
    t ^= t << 11;
    t ^= t >> 8;
    s_state[0] = t ^ s ^ (s >> 19);
    return s_state[0];
}

/* Required by tweetnacl.c (crypto_sign_keypair uses this to seed the key). */
void randombytes(uint8_t* x, uint64_t xlen) {
    uint64_t remaining = xlen;
    while(remaining >= 4) {
        uint32_t word = xorshift128();
        x[0] = (uint8_t)(word);
        x[1] = (uint8_t)(word >> 8);
        x[2] = (uint8_t)(word >> 16);
        x[3] = (uint8_t)(word >> 24);
        x += 4;
        remaining -= 4;
    }
    if(remaining > 0) {
        uint32_t word = xorshift128();
        for(uint64_t i = 0; i < remaining; i++) {
            x[i] = (uint8_t)(word >> (i * 8));
        }
    }
}
