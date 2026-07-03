/*
 * btcpc_data_hash.h — SensorReading/SensorDataCommit data_hash computation
 *
 * data_hash must be reproducible on both ends of the pipeline: the Flipper
 * signs a payload, and later the phone independently recomputes the exact
 * same hash from the bytes it received (see `payload_data_hash` in
 * android/rust/btcpc-miner/src/flipper_rx.rs) before committing it on-chain
 * as `batch_hash`. This module is the Flipper-side half of that contract —
 * kept as a tiny standalone unit so it is trivially testable off-device and
 * so every capture scene computes data_hash identically instead of each
 * scene hand-rolling its own hashing call.
 *
 * Convention (must match the phone exactly): SHA-256 over
 * `msg_type byte || raw payload bytes` — i.e. the same one-byte message-type
 * discriminator the wire frame carries, prefixed onto the exact payload
 * bytes that get ed25519-signed. This disambiguates payloads that could
 * otherwise collide byte-for-byte across different sensor classes.
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "protocol/btcpc_protocol.h"

/*
 * Compute data_hash = SHA-256(msg_type || payload) and hex-encode it.
 * `out_hex` must be at least 65 bytes (64 hex chars + NUL).
 */
void btcpc_data_hash_hex(BtcpcMsgType msg_type, const uint8_t* payload,
                         size_t payload_len, char out_hex[65]);
