# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
for published **components** (`components/iotmer/idf_component.yml` versions).

## [Unreleased]

## [0.1.12] - 2026-04-24

### Added

- **`components/iotmer/examples/05_ble_json/`** — BLE JSON channel example packaged with the **`iotmertech/iotmer`** component so it appears in the **ESP Component Registry → Examples** tab (mirrors `examples/05_ble_json`).

## [0.1.11] - 2026-04-24

### Added

- **`iotmer`:** `device_http_token` in `iotmer_creds_t` (from `POST …/provision/device`), NVS key `dht`, and `iotmer_device_auth_bind_claim()` for `POST …/devices/auth/bind-claim` (Bearer `dht_…`).
- **BLE:** Optional component **`iotmer_ble`**: NimBLE GATT JSON channel (RX write / TX notify) intended for application-layer device commands and provisioning flows.
- **Example 05 / `pc_ble_client`:** `examples/05_ble_json` + Python client for desktop testing (Bleak).
- English SDK docs: **`docs/sdk/esp-idf/ble-json-channel.md`** and **`docs/sdk/esp-idf/ble-json-provisioning.md`**.

### Changed

- **Docs (`docs/sdk/esp-idf/`):** `intro`, `index`, `examples`, `configuration`, `provisioning`, `troubleshooting` updated for the features above.
- **`iotmer_provision`:** after a successful parse, logs whether **`device_http_token`** is `set` or `absent` (token value is **never** logged), aligned with current **`POST …/provision/device`** JSON.
- Optional component **`iotmer_ble`**: NimBLE GATT JSON channel used as the base transport for BLE provisioning and device commands.
- Example **`examples/05_ble_json`**: reference JSON command contract over BLE (includes `wifi.set` / `wifi.clear` demo).
- English SDK docs **`docs/sdk/esp-idf/ble-json-channel.md`** and **`docs/sdk/esp-idf/ble-json-provisioning.md`**.

### Fixed

- NimBLE `BLE_UUID128_INIT` byte order for clock field so centrals see canonical UUID `…-8024-…`.
- Advertising payload size: service UUID in advertising PDU, GAP name in **scan response** (fits within the 31-byte advertising payload limit).
