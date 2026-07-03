/*
 * btcpc_data_hash.c — see btcpc_data_hash.h
 *
 * Shin Devlin — btcpc.network
 */

#include "btcpc_data_hash.h"
#include "crypto/sha256.h"

void btcpc_data_hash_hex(BtcpcMsgType msg_type, const uint8_t* payload,
                         size_t payload_len, char out_hex[65]) {
    BtcpcSha256Ctx ctx;
    uint8_t        digest[BTCPC_SHA256_DIGEST_LEN];
    uint8_t        type_byte = (uint8_t)msg_type;

    btcpc_sha256_init(&ctx);
    btcpc_sha256_update(&ctx, &type_byte, 1);
    if(payload_len > 0 && payload != NULL) {
        btcpc_sha256_update(&ctx, payload, payload_len);
    }
    btcpc_sha256_final(&ctx, digest);
    btcpc_sha256_to_hex(digest, out_hex);
}
