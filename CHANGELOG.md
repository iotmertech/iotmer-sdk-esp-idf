# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
for published **components** (`components/iotmer/idf_component.yml` versions).

## [Unreleased]

### Added

- Optional component **`iotmer_ble_wifi_prov`**: NimBLE GATT Wi‑Fi credential provisioning (protocol v1), public API `iotmer_ble_wifi_prov.h`.
- Example **`examples/05_ble_wifi_prov`**: minimal integration with `iotmer_wifi_reconnect()` after successful COMMIT.
- Reference Python client **`examples/05_ble_wifi_prov/pc_ble_client`** (Bleak) for desktop testing.
- English SDK doc **`docs/sdk/esp-idf/ble-wifi-provisioning.md`** (GATT layout, opcodes, events, security, QA checklist).

### Fixed

- NimBLE `BLE_UUID128_INIT` byte order for clock field so centrals see canonical UUID `…-8024-…` (matches `IOTMER_BLE_WIFI_PROV_UUID_*_STR` in the public header).
- Legacy advertising payload size: service UUID in advertising PDU, GAP name in **scan response** (fits within 31-byte legacy limits).
- `iotmer_ble_wifi_prov` **CMake** always declares `REQUIRES bt` / `PRIV_REQUIRES bt` so NimBLE headers resolve; stub path when `CONFIG_IOTMER_BLE_WIFI_PROV` is off is implemented in C with `#if CONFIG_IOTMER_BLE_WIFI_PROV`.

### Changed

- **`iotmer` Kconfig:** BLE-specific options moved to `components/iotmer_ble_wifi_prov/Kconfig.projbuild` so symbols exist whenever the companion component is present in the build.
