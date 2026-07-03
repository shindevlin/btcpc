/*
 * btcpc_scene_rfid.h — 125 kHz RFID presence-scan capture scene
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <gui/scene_manager.h>
#include <stdint.h>
#include <stdbool.h>

#include "../protocol/btcpc_protocol.h"

void btcpc_scene_rfid_on_enter(void* context);
bool btcpc_scene_rfid_on_event(void* context, SceneManagerEvent event);
void btcpc_scene_rfid_on_exit(void* context);

/*
 * Attempt a single 125 kHz card read. Returns true and fills `scan_out` if a
 * card was read within the poll window; returns false if no card is present.
 * Reusable by the auto-rotation scene.
 *
 * ON-DEVICE VERIFICATION NEEDED: wraps the Flipper's LF-RFID analog front-end
 * (coil + comparator, decoded via the `lfrfid` worker API — EM4100/HID
 * Prox/Indala). No Flipper SDK toolchain was available in this change's
 * build environment to compile-check the exact worker call sequence against
 * a specific firmware SDK revision. The touchpoint is isolated to this one
 * function. See docs/SIGNING_INTEGRATION.md and the PRD "Test on real
 * hardware" item.
 */
bool btcpc_rfid_read_once(BtcpcRfidScan* scan_out);
