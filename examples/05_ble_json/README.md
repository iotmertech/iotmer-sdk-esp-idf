# Example 05 — BLE JSON channel

Minimal ESP-IDF app that enables the optional **`iotmer_ble`** component and exposes a BLE GATT JSON channel.

## Build

```bash
cd examples/05_ble_json
idf.py set-target esp32c3    # or esp32, esp32s3, esp32c6, …
idf.py menuconfig
idf.py build flash monitor
```

## Configure

- **Component config → IOTMER BLE (JSON channel)**: enable `CONFIG_IOTMER_BLE=y`, set name prefix and security.
  - This example sets `CONFIG_IOTMER_BLE_GAP_NAME_PREFIX="MER-"` in `sdkconfig.defaults` (advertised name is `MER-` + short hex suffix).
- **Component config → Bluetooth**: enable controller + NimBLE host for your target.

## JSON commands (demo)

- `{"type":"ping","rid":"1"}`
- `{"type":"wifi.set","rid":"2","ssid":"...","pass":"..."}`
- `{"type":"wifi.clear","rid":"3"}`

The app responds via the TX characteristic notifications.

## Protocol documentation

See `docs/sdk/esp-idf/ble-json-provisioning.md` for the reference JSON contract carried over `iotmer_ble`.

## PC test client (optional)

See `pc_ble_client/` (Python + Bleak).

