/*
 * btcpc_scene_ir.h — infrared remote-signal capture scene
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <gui/scene_manager.h>
#include <stdint.h>
#include <stdbool.h>

#include "../protocol/btcpc_protocol.h"

void btcpc_scene_ir_on_enter(void* context);
bool btcpc_scene_ir_on_event(void* context, SceneManagerEvent event);
void btcpc_scene_ir_on_exit(void* context);

/*
 * Attempt a single IR capture. Returns true and fills `ir_out` if a signal
 * was received within the poll window (decoded to a known protocol, or raw
 * if not); returns false if nothing was received. Reusable by the
 * auto-rotation scene.
 *
 * Weak presence-only class per the PRD inventory: the Flipper's IR receiver
 * has no calibrated lux/temperature sensing — this only proves "some IR
 * remote signal was seen here," never a magnitude reading.
 *
 * ON-DEVICE VERIFICATION NEEDED: wraps the Flipper's IR receiver via the
 * `infrared` worker API. No Flipper SDK toolchain was available in this
 * change's build environment to compile-check the exact worker call
 * sequence against a specific firmware SDK revision. The touchpoint is
 * isolated to this one function. See docs/SIGNING_INTEGRATION.md and the
 * PRD "Test on real hardware" item.
 */
bool btcpc_ir_capture_once(BtcpcIrCapture* ir_out);
