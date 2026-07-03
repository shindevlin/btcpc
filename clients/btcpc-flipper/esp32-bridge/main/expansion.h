#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Called when a complete BTCPC frame arrives from the Flipper */
typedef void (*expansion_frame_cb_t)(const uint8_t* data, size_t len, void* ctx);

void expansion_init(expansion_frame_cb_t cb, void* ctx);
bool expansion_send(const uint8_t* data, size_t len);
bool expansion_is_ready(void);
