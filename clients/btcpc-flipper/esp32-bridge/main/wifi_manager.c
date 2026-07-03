/*
 * wifi_manager.c — WiFi connection management with NVS credential storage
 *
 * First boot with no credentials: starts AP "BTCPC-Setup" and hosts a
 * minimal HTTP config page at http://192.168.4.1/ to capture SSID/password.
 * After saving, reboots and connects to the configured network.
 */

#include "wifi_manager.h"
#include "config.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>
#include <mdns.h>
#include <lwip/sockets.h>

#define TAG          "BtcpcWiFi"
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY    5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry = 0;
static bool s_connected = false;
static char s_ip[16] = {0};

static wifi_connected_cb_t    s_on_connected;
static wifi_disconnected_cb_t s_on_disconnected;
static void*                  s_cb_ctx;

/* ── NVS credential storage ───────────────────────────────────────────────── */

static bool nvs_load_credentials(char* ssid, char* pass, size_t len) {
    nvs_handle_t h;
    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = len, pl = len;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &sl) == ESP_OK &&
               nvs_get_str(h, NVS_KEY_PASS, pass, &pl) == ESP_OK);
    nvs_close(h);
    return ok;
}

void wifi_manager_save_credentials(const char* ssid, const char* pass) {
    nvs_handle_t h;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    esp_restart();
}

/* ── Minimal HTTP setup server (AP mode) ──────────────────────────────────── */

static const char* SETUP_PAGE =
    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width'>"
    "<title>BTCPC WiFi Setup</title></head><body>"
    "<h2>BTCPC WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
    "SSID: <input name='s' required><br><br>"
    "Password: <input name='p' type='password'><br><br>"
    "<input type='submit' value='Save &amp; Connect'>"
    "</form></body></html>";

static const char* SAVE_OK =
    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
    "<html><body><h2>Saved! Rebooting...</h2></body></html>";

static void parse_and_save(const char* body) {
    char ssid[64] = {0}, pass[64] = {0};
    /* body: s=SSID&p=PASS (URL-encoded, simplified parser) */
    const char* sp = strstr(body, "s=");
    const char* pp = strstr(body, "&p=");
    if(!sp) return;
    sp += 2;
    if(pp) {
        size_t slen = pp - sp;
        if(slen >= sizeof(ssid)) slen = sizeof(ssid) - 1;
        memcpy(ssid, sp, slen);
        strncpy(pass, pp + 3, sizeof(pass) - 1);
    } else {
        strncpy(ssid, sp, sizeof(ssid) - 1);
    }
    ESP_LOGI(TAG, "saving credentials for SSID: %s", ssid);
    wifi_manager_save_credentials(ssid, pass);
}

static void setup_server_task(void* arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {
        .sin_family = AF_INET, .sin_port = htons(BTCPC_CONFIG_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    bind(srv, (struct sockaddr*)&a, sizeof(a));
    listen(srv, 2);
    ESP_LOGI(TAG, "setup server on port %d", BTCPC_CONFIG_PORT);
    while(1) {
        int fd = accept(srv, NULL, NULL);
        if(fd < 0) continue;
        char buf[512] = {0};
        recv(fd, buf, sizeof(buf) - 1, 0);
        if(strstr(buf, "POST /save")) {
            char* body = strstr(buf, "\r\n\r\n");
            send(fd, SAVE_OK, strlen(SAVE_OK), 0);
            close(fd);
            if(body) parse_and_save(body + 4);
        } else {
            send(fd, SETUP_PAGE, strlen(SETUP_PAGE), 0);
            close(fd);
        }
    }
}

/* ── WiFi event handler ───────────────────────────────────────────────────── */

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data) {
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if(s_on_disconnected) s_on_disconnected(s_cb_ctx);
        if(s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGI(TAG, "retrying WiFi (%d/%d)", s_retry, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "connected, IP: %s", s_ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if(s_on_connected) s_on_connected(s_ip, s_cb_ctx);
    }
}

/* ── AP mode (first-run setup) ────────────────────────────────────────────── */

static void start_ap_mode(void) {
    ESP_LOGI(TAG, "no credentials — starting AP mode: %s", BTCPC_AP_SSID);
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = BTCPC_AP_SSID,
            .password        = BTCPC_AP_PASS,
            .max_connection  = 2,
            .authmode        = WIFI_AUTH_WPA2_PSK,
        }
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    xTaskCreate(setup_server_task, "btcpc_setup", 4096, NULL, 5, NULL);
}

/* ── STA mode (normal operation) ──────────────────────────────────────────── */

static void start_sta_mode(const char* ssid, const char* pass) {
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if(!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "failed to connect, starting AP mode");
        start_ap_mode();
    }
}

void wifi_manager_init(wifi_connected_cb_t on_connected,
                       wifi_disconnected_cb_t on_disconnected,
                       void* ctx) {
    s_on_connected    = on_connected;
    s_on_disconnected = on_disconnected;
    s_cb_ctx          = ctx;
    s_wifi_event_group = xEventGroupCreate();

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    char ssid[64], pass[64];
    if(nvs_load_credentials(ssid, pass, sizeof(ssid))) {
        start_sta_mode(ssid, pass);
    } else {
        start_ap_mode();
    }
}

bool wifi_manager_is_connected(void) { return s_connected; }

void wifi_manager_get_ip(char* buf, size_t len) {
    strncpy(buf, s_ip, len - 1);
}
