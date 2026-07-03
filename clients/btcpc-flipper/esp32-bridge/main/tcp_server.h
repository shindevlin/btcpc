#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Called when a BTCPC frame arrives from the PC */
typedef void (*tcp_frame_cb_t)(const uint8_t* data, size_t len, void* ctx);

void tcp_server_init(tcp_frame_cb_t cb, void* ctx);
/* Send frame to all connected PC clients */
void tcp_server_broadcast(const uint8_t* data, size_t len);
int  tcp_server_client_count(void);
