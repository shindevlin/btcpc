/*
 * main.c — BTCPC WiFi Bridge for Flipper Zero ESP32-S2 dev board
 *
 * Architecture:
 *   Flipper ←UART/expansion→ ESP32-S2 ←TCP→ PC daemon
 *                                      ←TCP→ Android app (future)
 *
 * On boot:
 *   1. Load WiFi credentials from NVS. If none, start AP "BTCPC-Setup"
 *      and serve config page at http://192.168.4.1/
 *   2. Connect to WiFi, announce via mDNS as btcpc-flipper.local:4242
 *   3. Start TCP server on port 4242
 *   4. Start expansion UART handshake with Flipper at 9600→230400 baud
 *   5. Bridge: Flipper frames → TCP broadcast; PC frames → Flipper UART
 */

#include "config.h"
#include "expansion.h"
#include "tcp_server.h"
#include "wifi_manager.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <mdns.h>

#define TAG "BtcpcMain"

/* ── Frame routing ────────────────────────────────────────────────────────── */

/* Flipper → PC: broadcast to all TCP clients */
static void on_flipper_frame(const uint8_t* data, size_t len, void* ctx) {
    (void)ctx;
    ESP_LOGD(TAG, "Flipper→PC: %zu bytes type=0x%02x", len, len > 4 ? data[4] : 0);
    tcp_server_broadcast(data, len);
}

/* PC → Flipper: forward via expansion UART */
static void on_pc_frame(const uint8_t* data, size_t len, void* ctx) {
    (void)ctx;
    ESP_LOGD(TAG, "PC→Flipper: %zu bytes type=0x%02x", len, len > 4 ? data[4] : 0);
    expansion_send(data, len);
}

/* ── WiFi event callbacks ─────────────────────────────────────────────────── */

static void on_wifi_connected(const char* ip, void* ctx) {
    (void)ctx;
    ESP_LOGI(TAG, "WiFi connected: %s — starting services", ip);

    /* mDNS: PC discovers at btcpc-flipper.local:4242 */
    mdns_init();
    mdns_hostname_set(BTCPC_MDNS_HOSTNAME);
    mdns_instance_name_set("BTCPC Flipper Bridge");
    mdns_service_add(NULL, "_btcpc", "_tcp", BTCPC_TCP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local port %d", BTCPC_MDNS_HOSTNAME, BTCPC_TCP_PORT);

    tcp_server_init(on_pc_frame, NULL);
}

static void on_wifi_disconnected(void* ctx) {
    (void)ctx;
    ESP_LOGW(TAG, "WiFi disconnected");
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "BTCPC WiFi Bridge starting");

    /* Expansion UART starts immediately — doesn't need WiFi */
    expansion_init(on_flipper_frame, NULL);

    /* WiFi init: connects or starts AP setup mode */
    wifi_manager_init(on_wifi_connected, on_wifi_disconnected, NULL);

    /* Done — everything runs in tasks */
    ESP_LOGI(TAG, "all tasks started");
}
