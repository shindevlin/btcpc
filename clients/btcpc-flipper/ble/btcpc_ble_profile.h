/*
 * btcpc_ble_profile.h — BTCPC BLE profile template
 *
 * Wraps the BTCPC signing GATT service as a FuriHalBleProfileTemplate so it
 * can be started with bt_profile_start() from the app lifecycle. When active
 * the Flipper advertises as "BTCPC" and accepts one bonded connection.
 *
 * Usage:
 *   app->ble_profile = bt_profile_start(app->bt, ble_profile_btcpc, app);
 *
 * The profile_params pointer (FuriHalBleProfileParams) is cast to BtcpcApp*
 * inside the profile's start function to access the app's pk[] and to wire
 * up the sign callback.
 *
 * Shin Devlin — btcpc.network
 */

#pragma once

#include <furi_ble/profile_interface.h>
#include "btcpc_ble_svc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Advertised device name — visible during pairing scan on the phone */
#define BTCPC_BLE_ADV_NAME  "BTCPC"

/*
 * Profile instance struct. The first field MUST be FuriHalBleProfileBase so
 * the BT service can treat it as a base pointer.
 */
typedef struct {
    FuriHalBleProfileBase base; /* MUST be first */
    BtcpcBleSvc*          svc;
} BtcpcBleProfile;

/*
 * FuriHalBleProfileTemplate descriptor for the BTCPC signing profile.
 * Pass this to bt_profile_start():
 *
 *   app->ble_profile = bt_profile_start(app->bt, ble_profile_btcpc, app);
 */
extern const FuriHalBleProfileTemplate* const ble_profile_btcpc;

/*
 * Retrieve the BtcpcBleSvc* from a running profile instance.
 * Returns NULL if profile is not a BTCPC profile or has no active service.
 */
BtcpcBleSvc* btcpc_ble_profile_get_svc(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
