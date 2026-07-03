/*
 * btcpc_scene_nfc.h — NFC (ISO14443 A/B/F/V) presence-scan capture scene
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <gui/scene_manager.h>
#include <stdint.h>
#include <stdbool.h>

#include "../protocol/btcpc_protocol.h"

void btcpc_scene_nfc_on_enter(void* context);
bool btcpc_scene_nfc_on_event(void* context, SceneManagerEvent event);
void btcpc_scene_nfc_on_exit(void* context);

/*
 * Attempt a single NFC card poll. Returns true and fills `scan_out` if a card
 * was detected in the field within the (short, non-blocking-friendly) poll
 * window; returns false if no card is present. Reusable by the auto-rotation
 * scene, mirroring the btcpc_subghz_sample_* pattern.
 *
 * ON-DEVICE VERIFICATION NEEDED: this wraps the Flipper NFC front-end
 * (ST25R3916 via the `nfc` HAL / poller API). No Flipper SDK toolchain was
 * available in this change's build environment to compile-check the exact
 * poller call sequence against a specific firmware SDK revision. The
 * touchpoint is isolated to this one function — every other file in this
 * capture path (payload struct, data_hash, signing, BLE framing) is
 * hardware-independent and already verified via host tests. See
 * docs/SIGNING_INTEGRATION.md and the PRD "Test on real hardware" item.
 */
bool btcpc_nfc_poll_once(BtcpcNfcScan* scan_out);
