#pragma once
#include <stdbool.h>

typedef void (*wifi_connected_cb_t)(const char* ip, void* ctx);
typedef void (*wifi_disconnected_cb_t)(void* ctx);

void wifi_manager_init(wifi_connected_cb_t on_connected,
                       wifi_disconnected_cb_t on_disconnected,
                       void* ctx);
bool wifi_manager_is_connected(void);
void wifi_manager_get_ip(char* buf, size_t len);
/* Save new credentials to NVS and reboot */
void wifi_manager_save_credentials(const char* ssid, const char* pass);
