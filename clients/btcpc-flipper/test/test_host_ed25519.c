/*
 * test_host_ed25519.c — host build of btcpc_ed25519_* without furi_hal
 *
 * crypto/ed25519.c is Flipper-only (includes furi_hal_random.h for its
 * randombytes() implementation). This is a byte-for-byte reimplementation of
 * the same three public functions (crypto/ed25519.h), but backed by the test
 * PRNG in test_host_crypto_shim.c instead of the STM32WB55 TRNG. It exists
 * ONLY to let test binaries link tweetnacl.c and exercise the exact signing
 * primitives the real firmware uses, without needing the Flipper SDK.
 *
 * Test binaries must link this file instead of crypto/ed25519.c.
 *
 * Shin Devlin — btcpc.network
 */

#include "../crypto/ed25519.h"
#include "../crypto/tweetnacl.h"
#include <string.h>

void btcpc_ed25519_keypair(uint8_t pk_out[BTCPC_ED25519_PK_LEN],
                           uint8_t sk_out[BTCPC_ED25519_SK_LEN]) {
    crypto_sign_keypair(pk_out, sk_out);
}

void btcpc_ed25519_sign(uint8_t        sig_out[BTCPC_ED25519_SIG_LEN],
                        const uint8_t* msg,
                        size_t         msg_len,
                        const uint8_t  sk[BTCPC_ED25519_SK_LEN]) {
#define BTCPC_SIGN_MSG_MAX 512
    if(msg_len > BTCPC_SIGN_MSG_MAX) {
        return;
    }
    uint8_t  sm[BTCPC_SIGN_MSG_MAX + BTCPC_ED25519_SIG_LEN];
    uint64_t smlen = 0;
    crypto_sign(sm, &smlen, msg, (uint64_t)msg_len, sk);
    memcpy(sig_out, sm, BTCPC_ED25519_SIG_LEN);
#undef BTCPC_SIGN_MSG_MAX
}

int btcpc_ed25519_verify(const uint8_t  sig[BTCPC_ED25519_SIG_LEN],
                         const uint8_t* msg,
                         size_t         msg_len,
                         const uint8_t  pk[BTCPC_ED25519_PK_LEN]) {
#define BTCPC_VERIFY_MSG_MAX 512
    if(msg_len > BTCPC_VERIFY_MSG_MAX) {
        return -1;
    }
    uint8_t  sm[BTCPC_VERIFY_MSG_MAX + BTCPC_ED25519_SIG_LEN];
    uint8_t  m[BTCPC_VERIFY_MSG_MAX];
    uint64_t mlen = 0;
    memcpy(sm, sig, BTCPC_ED25519_SIG_LEN);
    memcpy(sm + BTCPC_ED25519_SIG_LEN, msg, msg_len);
    int rc = crypto_sign_open(m, &mlen, sm, (uint64_t)(msg_len + BTCPC_ED25519_SIG_LEN), pk);
    return rc;
#undef BTCPC_VERIFY_MSG_MAX
}
