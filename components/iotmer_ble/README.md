# `iotmer_ble` (optional ESP-IDF component)

`iotmer_ble` adds a small **BLE GATT JSON channel** built on **ESP-IDF NimBLE**.

- **Purpose**: A general transport for UTF-8 JSON messages between a phone/PC and the device.
- **Relation to provisioning**: Wi‑Fi provisioning is implemented *on top* of this channel via the JSON contract in `docs/sdk/esp-idf/ble-json-provisioning.md` (see `examples/05_ble_json`).

## Public API

- Header: `include/iotmer_ble.h`
- Kconfig: `Kconfig.projbuild` (Component config → IOTMER BLE)

## GATT layout

- Service UUID: `IOTMER_BLE_UUID_SVC_STR`
- RX characteristic: write JSON (`IOTMER_BLE_UUID_RX_STR`)
- TX characteristic: notify/read JSON (`IOTMER_BLE_UUID_TX_STR`)

## Notes

- Advertising places **service UUID** in the advertising PDU and the **GAP name** in scan response to stay within common **31‑byte** advertising payload limits.
- If `CONFIG_IOTMER_BLE_REQUIRE_ENC=y`, centrals must pair/encrypt before writing to RX.

