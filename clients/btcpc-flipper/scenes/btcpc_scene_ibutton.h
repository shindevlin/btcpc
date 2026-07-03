/*
 * btcpc_scene_ibutton.h — iButton (1-Wire/Dallas ROM) presence-scan capture
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <gui/scene_manager.h>
#include <stdint.h>
#include <stdbool.h>

#include "../protocol/btcpc_protocol.h"

void btcpc_scene_ibutton_on_enter(void* context);
bool btcpc_scene_ibutton_on_event(void* context, SceneManagerEvent event);
void btcpc_scene_ibutton_on_exit(void* context);

/*
 * Attempt a single iButton contact read. Returns true and fills `btn_out` if
 * a Dallas/Maxim 1-Wire device made contact within the poll window; returns
 * false otherwise. Reusable by the auto-rotation scene.
 *
 * ON-DEVICE VERIFICATION NEEDED: wraps the Flipper's 1-Wire contact pin via
 * the `ibutton` worker API. No Flipper SDK toolchain was available in this
 * change's build environment to compile-check the exact worker call
 * sequence against a specific firmware SDK revision. The touchpoint is
 * isolated to this one function. See docs/SIGNING_INTEGRATION.md and the
 * PRD "Test on real hardware" item.
 */
bool btcpc_ibutton_read_once(BtcpcIButton* btn_out);
