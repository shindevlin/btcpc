/*
 * btcpc_scene_subghz.h — Sub-GHz spectrum observation capture scene
 *
 * Shin Devlin — btcpc.network
 */
#pragma once

#include <gui/scene_manager.h>

void btcpc_scene_subghz_on_enter(void* context);
bool btcpc_scene_subghz_on_event(void* context, SceneManagerEvent event);
void btcpc_scene_subghz_on_exit(void* context);
