/*
 * expansion.c — Flipper expansion port protocol implementation for ESP32-S2
 *
 * Implements the Flipper expansion protocol state machine:
 *   1. Starts at 9600 baud, exchanges heartbeats
 *   2. Negotiates up to 230400 baud
 *   3. Exchanges BTCPC frames as ExpansionFrameTypeData payloads
 *
 * BTCPC frames larger than 64 bytes are chunked across multiple data frames.
 * The receiver reassembles by reading until a full BTCPC frame (magic + len) is complete.
 */

#include "expansion.h"
#include "config.h"

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/uart.h>
#include <esp_log.h>

#define TAG "BtcpcExp"

/* ── Expansion protocol frame types (matches expansion_protocol.h) ────────── */
#define EXP_FRAME_HEARTBEAT  0x01
#define EXP_FRAME_STATUS     0x02
#define EXP_FRAME_BAUD_RATE  0x03
#define EXP_FRAME_CONTROL    0x04
#define EXP_FRAME_DATA       0x05

#define EXP_MAX_DATA         64
#define EXP_ERROR_NONE       0x00

/* Frame header: type(1) + length(1) for data frames */
typedef struct __attribute__((packed)) {
    uint8_t type;
} ExpFrameHeader;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t error;
} ExpStatusFrame;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t baud;
} ExpBaudFrame;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
    uint8_t data[EXP_MAX_DATA];
} ExpDataFrame;

/* ── State ────────────────────────────────────────────────────────────────── */

typedef enum {
    EXP_STATE_IDLE,
    EXP_STATE_HANDSHAKE,
    EXP_STATE_BAUD_NEGOTIATE,
    EXP_STATE_READY,
} ExpState;

static struct {
    ExpState           state;
    expansion_frame_cb_t cb;
    void*              cb_ctx;
    /* reassembly buffer for incoming BTCPC frames */
    uint8_t            rx_buf[4096];
    size_t             rx_len;
    TickType_t         last_heartbeat;
} g;

/* ── UART helpers ─────────────────────────────────────────────────────────── */

static void uart_set_baud(uint32_t baud) {
    uart_set_baudrate(EXPANSION_UART_NUM, baud);
    vTaskDelay(pdMS_TO_TICKS(30)); /* dead time per spec */
}

static void uart_write_raw(const uint8_t* buf, size_t len) {
    uart_write_bytes(EXPANSION_UART_NUM, (const char*)buf, len);
}

static int uart_read_raw(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    return uart_read_bytes(EXPANSION_UART_NUM, buf, len,
                           pdMS_TO_TICKS(timeout_ms));
}

/* ── Frame send helpers ───────────────────────────────────────────────────── */

static void send_heartbeat(void) {
    uint8_t frame = EXP_FRAME_HEARTBEAT;
    uart_write_raw(&frame, 1);
}

static void send_status_ok(void) {
    ExpStatusFrame f = { .type = EXP_FRAME_STATUS, .error = EXP_ERROR_NONE };
    uart_write_raw((uint8_t*)&f, sizeof(f));
}

static void send_baud_request(uint32_t baud) {
    ExpBaudFrame f = { .type = EXP_FRAME_BAUD_RATE, .baud = baud };
    uart_write_raw((uint8_t*)&f, sizeof(f));
}

/* ── Public: send BTCPC frame to Flipper (chunked) ────────────────────────── */

bool expansion_send(const uint8_t* data, size_t len) {
    if(g.state != EXP_STATE_READY) return false;

    size_t offset = 0;
    while(offset < len) {
        size_t chunk = len - offset;
        if(chunk > EXP_MAX_DATA) chunk = EXP_MAX_DATA;

        ExpDataFrame f;
        f.type   = EXP_FRAME_DATA;
        f.length = (uint8_t)chunk;
        memcpy(f.data, data + offset, chunk);

        uart_write_raw((uint8_t*)&f, 2 + chunk); /* type + length + data */
        offset += chunk;
        vTaskDelay(1); /* yield between chunks */
    }
    return true;
}

bool expansion_is_ready(void) {
    return g.state == EXP_STATE_READY;
}

/* ── Frame receiver: reassemble BTCPC frames from data chunks ─────────────── */

static void process_data_chunk(const uint8_t* chunk, uint8_t len) {
    if(g.rx_len + len > sizeof(g.rx_buf)) {
        ESP_LOGW(TAG, "rx buf overflow, resetting");
        g.rx_len = 0;
    }
    memcpy(g.rx_buf + g.rx_len, chunk, len);
    g.rx_len += len;

    /* BTCPC frame: magic "BTPC"(4) + type(1) + payload_len_le(2) + sig(64) + payload */
    while(g.rx_len >= 7) {
        if(g.rx_buf[0] != 'B' || g.rx_buf[1] != 'T' ||
           g.rx_buf[2] != 'P' || g.rx_buf[3] != 'C') {
            /* shift one byte and retry */
            memmove(g.rx_buf, g.rx_buf + 1, g.rx_len - 1);
            g.rx_len--;
            continue;
        }
        uint16_t plen = (uint16_t)g.rx_buf[5] | ((uint16_t)g.rx_buf[6] << 8);
        size_t total  = 71 + plen; /* FRAME_HEADER_SIZE=71 */
        if(g.rx_len < total) break; /* wait for more data */

        if(g.cb) g.cb(g.rx_buf, total, g.cb_ctx);
        memmove(g.rx_buf, g.rx_buf + total, g.rx_len - total);
        g.rx_len -= total;
    }
}

/* ── Main expansion task ──────────────────────────────────────────────────── */

static void expansion_task(void* arg) {
    (void)arg;
    uint8_t byte;

    ESP_LOGI(TAG, "expansion task started at %d baud", EXPANSION_BAUD_DEFAULT);
    g.state = EXP_STATE_HANDSHAKE;

    while(1) {
        int n = uart_read_raw(&byte, 1, EXPANSION_HEARTBEAT_MS);

        if(n <= 0) {
            /* timeout — send heartbeat to keep link alive */
            if(g.state >= EXP_STATE_HANDSHAKE) send_heartbeat();
            continue;
        }

        switch(byte) {
        case EXP_FRAME_HEARTBEAT:
            if(g.state == EXP_STATE_HANDSHAKE) {
                ESP_LOGI(TAG, "got heartbeat, initiating baud negotiation");
                send_status_ok();
                vTaskDelay(pdMS_TO_TICKS(10));
                send_baud_request(EXPANSION_BAUD_FAST);
                g.state = EXP_STATE_BAUD_NEGOTIATE;
            } else if(g.state == EXP_STATE_READY) {
                /* echo heartbeat back */
                send_heartbeat();
                g.last_heartbeat = xTaskGetTickCount();
            }
            break;

        case EXP_FRAME_STATUS: {
            uint8_t err;
            uart_read_raw(&err, 1, EXPANSION_TIMEOUT_MS);
            if(g.state == EXP_STATE_BAUD_NEGOTIATE && err == EXP_ERROR_NONE) {
                /* Flipper accepted baud — switch */
                vTaskDelay(pdMS_TO_TICKS(30));
                uart_set_baud(EXPANSION_BAUD_FAST);
                send_status_ok();
                g.state = EXP_STATE_READY;
                ESP_LOGI(TAG, "link ready at %d baud", EXPANSION_BAUD_FAST);
            }
            break;
        }

        case EXP_FRAME_BAUD_RATE: {
            /* Flipper is requesting a baud rate — accept and switch */
            uint32_t baud = 0;
            uart_read_raw((uint8_t*)&baud, 4, EXPANSION_TIMEOUT_MS);
            ESP_LOGI(TAG, "Flipper requested baud %lu", (unsigned long)baud);
            send_status_ok();
            vTaskDelay(pdMS_TO_TICKS(30));
            uart_set_baud(baud);
            if(g.state == EXP_STATE_BAUD_NEGOTIATE) {
                g.state = EXP_STATE_READY;
                ESP_LOGI(TAG, "link ready at %lu baud", (unsigned long)baud);
            }
            break;
        }

        case EXP_FRAME_DATA: {
            uint8_t len;
            if(uart_read_raw(&len, 1, EXPANSION_TIMEOUT_MS) < 1) break;
            if(len == 0 || len > EXP_MAX_DATA) break;
            uint8_t buf[EXP_MAX_DATA];
            int got = uart_read_raw(buf, len, EXPANSION_TIMEOUT_MS);
            if(got != len) break;
            send_status_ok(); /* ack the data frame */
            if(g.state == EXP_STATE_READY) {
                process_data_chunk(buf, len);
            }
            break;
        }

        default:
            ESP_LOGD(TAG, "unknown frame type 0x%02x", byte);
            break;
        }
    }
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

void expansion_init(expansion_frame_cb_t cb, void* ctx) {
    memset(&g, 0, sizeof(g));
    g.cb     = cb;
    g.cb_ctx = ctx;
    g.state  = EXP_STATE_IDLE;

    uart_config_t cfg = {
        .baud_rate  = EXPANSION_BAUD_DEFAULT,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(EXPANSION_UART_NUM, &cfg);
    uart_set_pin(EXPANSION_UART_NUM,
                 EXPANSION_TX_PIN, EXPANSION_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(EXPANSION_UART_NUM, EXPANSION_BUF_SIZE, 0, 0, NULL, 0);

    xTaskCreate(expansion_task, "btcpc_exp", 4096, NULL, 10, NULL);
}
