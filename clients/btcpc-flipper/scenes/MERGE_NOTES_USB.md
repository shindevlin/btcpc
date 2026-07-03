# USB Safety Scene — Merge Notes

The files `btcpc_scene_usb.c` and `btcpc_scene_usb.h` are self-contained and
compile-ready. The scene is dormant until the following changes are merged into
the files currently locked by other agents.

---

## btcpc_protocol.h additions needed

**1. Add to `BtcpcMsgType` enum:**
```c
BTCPC_MSG_USB_SAFETY = 0x0B,  /* USB juice-jack safety scan result */
```

**2. Add payload struct (move from btcpc_scene_usb.c local definition):**
```c
typedef struct __attribute__((packed)) {
    uint8_t  verdict;            /* 0=safe, 1=data_detected, 2=active_attack */
    uint8_t  usb_class;          /* USB class seen (0=none, 0x02=CDC, 0x0B=CCID, 0xFF=unknown) */
    uint8_t  enumeration_count;
    uint16_t monitor_ms;
} BtcpcUsbSafety;
```

**3. Add builder declaration:**
```c
size_t btcpc_build_usb_safety(BtcpcFrame* frame, const BtcpcUsbSafety* result, const uint8_t sk[64]);
```

---

## btcpc_protocol.c additions needed

**Add builder implementation (same pattern as btcpc_build_rfid):**
```c
size_t btcpc_build_usb_safety(BtcpcFrame*            frame,
                               const BtcpcUsbSafety*  result,
                               const uint8_t          sk[BTCPC_ED25519_SK_LEN]) {
    uint16_t plen = (uint16_t)sizeof(BtcpcUsbSafety);
    frame_init(&frame->hdr, BTCPC_MSG_USB_SAFETY, plen);
    memcpy(frame->payload, result, plen);
    frame_sign(&frame->hdr, frame->payload, sk);
    return BTCPC_FRAME_HEADER_SIZE + plen;
}
```

Once this exists, replace the manual frame build in `btcpc_usb_push_result()`
with a call to `btcpc_build_usb_safety()`.

---

## btcpc.h additions needed

**1. Add to `BtcpcCustomEvent` enum:**
```c
BtcpcEventUsbSafetyTick = 110, /* USB safety scene: 1-second poll tick */
```

**2. Add to `BtcpcScene` enum:**
```c
BtcpcSceneUsb,
```
(Insert before `BtcpcSceneCount`.)

**3. Add to `BtcpcMenuItem` enum (for main menu entry):**
```c
BtcpcMenuUsb = 2,
```

---

## btcpc.c additions needed

**Add to scene handler arrays** (in `btcpc_scene_handlers`):
```c
[BtcpcSceneUsb] = {
    .on_enter = btcpc_scene_usb_on_enter,
    .on_event = btcpc_scene_usb_on_event,
    .on_exit  = btcpc_scene_usb_on_exit,
},
```

Include the header at the top of btcpc.c:
```c
#include "scenes/btcpc_scene_usb.h"
```

---

## btcpc_scene_main.c additions needed

**Add "USB Safety" menu item:**
```c
submenu_add_item(
    app->submenu,
    "USB Safety",
    BtcpcMenuUsb,
    btcpc_scene_main_submenu_cb,
    app);
```

**Add to submenu callback switch:**
```c
case BtcpcMenuUsb:
    scene_manager_next_scene(app->scene_manager, BtcpcSceneUsb);
    break;
```

---

## btcpc_scene_ble.c additions needed

**This is the trigger path.** Without this change the USB safety scene
cannot be activated from the phone. Add to the `BTCPC_MSG_SENSOR_REQ`
dispatch switch (around line 910):

```c
case 0x0Bu: /* BTCPC_MSG_USB_SAFETY — replace with enum once added */
    scene_manager_next_scene(app->scene_manager, BtcpcSceneUsb);
    break;
```

Note: `scene_manager_next_scene` must be called on the app thread, which is
the case here (DATA_RX is dispatched via view_dispatcher_send_custom_event).

---

## Android FlipperFragment.kt additions needed

**1. Add frame type constant:**
```kotlin
const val BTCPC_MSG_USB_SAFETY = 0x0B
```

**2. In the DATA_CHANNEL frame handler, add a case for 0x0B:**
```kotlin
BTCPC_MSG_USB_SAFETY -> {
    val verdict = payload[0].toInt() and 0xFF
    val usbClass = payload[1].toInt() and 0xFF
    val enumCount = payload[2].toInt() and 0xFF
    handleUsbSafetyResult(verdict, usbClass, enumCount)
}
```

**3. Implement handleUsbSafetyResult:**
- verdict == 0: dismiss any active warning dialog
- verdict == 1: show AlertDialog "WARNING: Data-capable USB port detected"
  - Include exact GPS coordinates from app's last GPS fix (not grid-cell)
  - Show Toast: "WARNING: Data-capable USB port"
- verdict == 2: show AlertDialog "ATTACK: Malicious USB injection detected"
  - Show Toast: "ATTACK: Malicious USB detected — reported to chain"
  - Submit chain entry with:
    - entry type: `UsbThreatReport` (new entry type, define in btcpc-types)
    - exact lat/lon from last GPS fix (app.lastLatE7 / app.lastLonE7 as doubles / 1e7)
    - usb_class byte
    - flipper device public key (for on-chain correlation)

**4. GPS precision requirement:**
Use exact coordinates, NOT grid-cell rounding. The chain entry must carry
sufficient precision for other wallets to route around the malicious port.
Minimum: 5 decimal places of degree precision (≈ 1 m accuracy).

---

## Verdict → USB class mapping (reference)

| verdict | meaning                                        | usb_class value |
|---------|------------------------------------------------|-----------------|
| 0       | SAFE — no host bus activity in 10s             | 0x00 (none)     |
| 1       | WARNING — host enumerated (DescriptorRequest)  | 0x02            |
| 1 or 2  | WARNING/ATTACK — Reset only, no Descriptor     | 0xFF            |
| 2       | ATTACK — repeated bus resets (>1 reset seen)   | 0xFF            |

Detection is event-driven via `furi_hal_usb_set_state_callback()`:
- `FuriHalUsbStateEventReset` → host is driving data lines (sets usb_class=0xFF)
- `FuriHalUsbStateEventDescriptorRequest` → full enumeration attempt (sets usb_class=0x02)

Power-only chargers generate neither event. False positive rate for this approach
is near zero — only a genuine USB host on the cable triggers these events.

---

## Known limitations (post-merge improvement opportunities)

- USB class bytes in BtcpcUsbSafetyResult are descriptive tags only, not actual
  USB class descriptor values. A privileged system service with access to the
  STM32WB USB peripheral registers could read the actual bDeviceClass/bInterfaceClass
  from the Setup packet and populate usb_class with the real value.

- VBUS detection via `furi_hal_power_get_usb_voltage()` requires the power IC
  to be reporting correctly. Confirmed FAP-accessible (furi_hal_power.h).

- The 10-second window is fixed. A future version could make it configurable
  via the `BtcpcSensorReq.duration_ms` field.
