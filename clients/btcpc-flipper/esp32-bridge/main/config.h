#pragma once

/* TCP server port — PC daemon connects here */
#define BTCPC_TCP_PORT          4242

/* mDNS hostname — PC discovers at btcpc-flipper.local */
#define BTCPC_MDNS_HOSTNAME     "btcpc-flipper"

/* Expansion UART — ESP32-S2 GPIO pins for Flipper dev board */
#define EXPANSION_UART_NUM      UART_NUM_1
#define EXPANSION_TX_PIN        17
#define EXPANSION_RX_PIN        18
#define EXPANSION_BUF_SIZE      1024

/* Baud rate negotiation: start at 9600, negotiate up to 230400 */
#define EXPANSION_BAUD_DEFAULT  9600
#define EXPANSION_BAUD_FAST     230400

/* Expansion protocol timing */
#define EXPANSION_HEARTBEAT_MS  500
#define EXPANSION_TIMEOUT_MS    250

/* WiFi credentials stored in NVS under namespace "btcpc" */
#define NVS_NAMESPACE           "btcpc"
#define NVS_KEY_SSID            "ssid"
#define NVS_KEY_PASS            "pass"

/* AP mode for first-run WiFi config */
#define BTCPC_AP_SSID           "BTCPC-Setup"
#define BTCPC_AP_PASS           "btcpc1234"
#define BTCPC_CONFIG_PORT       80
