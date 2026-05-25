/*
 * btcpc_ble_profile.c — BTCPC BLE profile template implementation
 *
 * Implements the FuriHalBleProfileTemplate callbacks (start / stop /
 * get_gap_config) and provides a bt_profile_start()-compatible descriptor.
 *
 * GAP configuration:
 *   - Bonding enabled — the Flipper stores one long-term key per peer.
 *     The BT service persists keys to /ext/apps_data/btcpc/bt.keys so they
 *     survive app restarts and Flipper power-cycles without re-pairing.
 *   - Pairing method: PIN code display — the Flipper shows a six-digit PIN
 *     on screen; the phone user confirms it. This prevents passive eavesdrop
 *     of the pairing exchange on the signing channel.
 *   - Advertisement name: "BTCPC" — hardcoded, visible during scan.
 *   - MAC address: uses Flipper's factory address XOR'd with a fixed mask
 *     derived from the app ID so it doesn't collide with the system profile.
 *
 * Shin Devlin — btcpc.network
 */

#include "btcpc_ble_profile.h"
#include "../btcpc.h"

#include <furi.h>
#include <furi_ble/profile_interface.h>
#include <ble/core/ble_defs.h>
#include <gap.h>
#include <furi_hal_version.h>

#include <string.h>

#undef TAG
#define TAG "BtcpcBleProfile"

/* XOR mask applied to the last two bytes of the BLE MAC address to
 * differentiate the BTCPC profile from the Flipper's system serial profile. */
#define BTCPC_MAC_XOR_0  0xA7U
#define BTCPC_MAC_XOR_1  0x3CU

/* ─── Profile template callbacks ───────────────────────────────────────── */

static FuriHalBleProfileBase* btcpc_profile_start(FuriHalBleProfileParams profile_params) {
    /* profile_params is the BtcpcApp* passed as the second arg to bt_profile_start() */
    BtcpcApp* app = (BtcpcApp*)profile_params;
    if(!app) {
        FURI_LOG_E(TAG, "profile_start: NULL params");
        return NULL;
    }

    BtcpcBleProfile* profile = malloc(sizeof(BtcpcBleProfile));
    if(!profile) return NULL;

    profile->base.config = ble_profile_btcpc;

    /* Start the GATT service — wires sign + data_rx callbacks to app thread queue */
    profile->svc = btcpc_ble_svc_start(app->pk, btcpc_ble_sign_req_cb, btcpc_ble_data_rx_cb, app);
    if(!profile->svc) {
        FURI_LOG_E(TAG, "btcpc_ble_svc_start failed");
        free(profile);
        return NULL;
    }

    FURI_LOG_I(TAG, "BTCPC BLE profile started");
    return &profile->base;
}

static void btcpc_profile_stop(FuriHalBleProfileBase* profile) {
    if(!profile) return;
    /* Verify this instance belongs to the BTCPC profile */
    if(profile->config != ble_profile_btcpc) return;

    BtcpcBleProfile* p = (BtcpcBleProfile*)profile;
    btcpc_ble_svc_stop(p->svc);
    p->svc = NULL;
    free(p);

    FURI_LOG_I(TAG, "BTCPC BLE profile stopped");
}

static void btcpc_profile_get_gap_config(
    GapConfig* cfg,
    FuriHalBleProfileParams profile_params) {
    UNUSED(profile_params);

    memset(cfg, 0, sizeof(GapConfig));

    /* Service UUID for advertising — use the BTCPC service UUID (128-bit) */
    cfg->adv_service.UUID_Type = UUID_TYPE_128;
    const uint8_t svc_uuid_bytes[16] = BTCPC_SVC_UUID;
    memcpy(cfg->adv_service.Service_UUID_128, svc_uuid_bytes, 16);

    /* Advertisement name */
    strncpy(cfg->adv_name, BTCPC_BLE_ADV_NAME, sizeof(cfg->adv_name) - 1);

    /* Use the device's factory BLE MAC, XOR last two bytes for uniqueness */
    const uint8_t* factory_mac = furi_hal_version_get_ble_mac();
    memcpy(cfg->mac_address, factory_mac, GAP_MAC_ADDR_SIZE);
    cfg->mac_address[0] ^= BTCPC_MAC_XOR_0;
    cfg->mac_address[1] ^= BTCPC_MAC_XOR_1;

    /* Bonding enabled — store long-term keys for auto-reconnect */
    cfg->bonding_mode    = true;
    cfg->pairing_method  = GapPairingPinCodeShow;

    /* Connection parameters: 60 ms interval, 0 latency, 4 s supervision */
    cfg->conn_param.conn_int_min      = 0x0030; /* 37.5 ms */
    cfg->conn_param.conn_int_max      = 0x0050; /* 62.5 ms */
    cfg->conn_param.slave_latency     = 0;
    cfg->conn_param.supervisor_timeout = 400;   /* 4 s in 10 ms units */
}

/* ─── Profile descriptor ────────────────────────────────────────────────── */

static const FuriHalBleProfileTemplate btcpc_profile_template = {
    .start          = btcpc_profile_start,
    .stop           = btcpc_profile_stop,
    .get_gap_config = btcpc_profile_get_gap_config,
};

const FuriHalBleProfileTemplate* const ble_profile_btcpc = &btcpc_profile_template;

/* ─── Helpers ───────────────────────────────────────────────────────────── */

BtcpcBleSvc* btcpc_ble_profile_get_svc(FuriHalBleProfileBase* profile) {
    if(!profile) return NULL;
    if(profile->config != ble_profile_btcpc) return NULL;
    return ((BtcpcBleProfile*)profile)->svc;
}
