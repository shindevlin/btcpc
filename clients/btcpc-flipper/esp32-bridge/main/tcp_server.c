/*
 * tcp_server.c — TCP server for PC ↔ BTCPC-Flipper communication
 *
 * Listens on BTCPC_TCP_PORT (4242). Up to 4 simultaneous PC clients.
 * Wire format: raw BTCPC frames streamed over TCP, same as BLE DATA_CHANNEL.
 * Length framing: none needed — BTCPC frame magic + payload_len is self-delimiting.
 */

#include "tcp_server.h"
#include "config.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <esp_log.h>

#define TAG        "BtcpcTCP"
#define MAX_CLIENTS 4
#define RX_BUF_LEN  4096

static struct {
    tcp_frame_cb_t cb;
    void*          cb_ctx;
    int            clients[MAX_CLIENTS];
    SemaphoreHandle_t lock;
} g;

static void remove_client(int fd) {
    xSemaphoreTake(g.lock, portMAX_DELAY);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(g.clients[i] == fd) {
            close(fd);
            g.clients[i] = -1;
            ESP_LOGI(TAG, "client disconnected (fd=%d)", fd);
            break;
        }
    }
    xSemaphoreGive(g.lock);
}

static bool add_client(int fd) {
    xSemaphoreTake(g.lock, portMAX_DELAY);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(g.clients[i] < 0) {
            g.clients[i] = fd;
            xSemaphoreGive(g.lock);
            return true;
        }
    }
    xSemaphoreGive(g.lock);
    return false;
}

void tcp_server_broadcast(const uint8_t* data, size_t len) {
    xSemaphoreTake(g.lock, portMAX_DELAY);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(g.clients[i] >= 0) {
            int sent = send(g.clients[i], data, len, MSG_DONTWAIT);
            if(sent < 0) {
                ESP_LOGW(TAG, "send failed fd=%d, removing", g.clients[i]);
                close(g.clients[i]);
                g.clients[i] = -1;
            }
        }
    }
    xSemaphoreGive(g.lock);
}

int tcp_server_client_count(void) {
    int count = 0;
    xSemaphoreTake(g.lock, portMAX_DELAY);
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if(g.clients[i] >= 0) count++;
    }
    xSemaphoreGive(g.lock);
    return count;
}

/* Per-client receive task */
typedef struct { int fd; } ClientArg;

static void client_task(void* arg) {
    ClientArg* ca = (ClientArg*)arg;
    int fd = ca->fd;
    free(ca);

    uint8_t buf[RX_BUF_LEN];
    size_t  rx_len = 0;

    while(1) {
        int n = recv(fd, buf + rx_len, sizeof(buf) - rx_len, 0);
        if(n <= 0) break;
        rx_len += n;

        /* Dispatch complete BTCPC frames */
        while(rx_len >= 7) {
            if(buf[0] != 'B' || buf[1] != 'T' ||
               buf[2] != 'P' || buf[3] != 'C') {
                memmove(buf, buf + 1, rx_len - 1);
                rx_len--;
                continue;
            }
            uint16_t plen = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
            size_t   total = 71 + plen;
            if(rx_len < total) break;

            if(g.cb) g.cb(buf, total, g.cb_ctx);
            memmove(buf, buf + total, rx_len - total);
            rx_len -= total;
        }
    }

    remove_client(fd);
    vTaskDelete(NULL);
}

static void server_task(void* arg) {
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(BTCPC_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, MAX_CLIENTS);

    ESP_LOGI(TAG, "listening on port %d", BTCPC_TCP_PORT);

    while(1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(server_fd, (struct sockaddr*)&peer, &peer_len);
        if(fd < 0) continue;

        char ip[16];
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "PC connected: %s (fd=%d)", ip, fd);

        if(!add_client(fd)) {
            ESP_LOGW(TAG, "max clients reached, rejecting %s", ip);
            close(fd);
            continue;
        }

        ClientArg* ca = malloc(sizeof(ClientArg));
        ca->fd = fd;
        xTaskCreate(client_task, "btcpc_tcp_cli", 4096, ca, 9, NULL);
    }
}

void tcp_server_init(tcp_frame_cb_t cb, void* ctx) {
    g.cb     = cb;
    g.cb_ctx = ctx;
    g.lock   = xSemaphoreCreateMutex();
    for(int i = 0; i < MAX_CLIENTS; i++) g.clients[i] = -1;
    xTaskCreate(server_task, "btcpc_tcp_srv", 4096, NULL, 9, NULL);
}
