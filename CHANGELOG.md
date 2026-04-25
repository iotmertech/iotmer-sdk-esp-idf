# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
for published **components** (`components/iotmer/idf_component.yml` versions).

## [Unreleased]

### Added

- Optional component **`iotmer_ble`**: NimBLE GATT JSON channel used as the base transport for BLE provisioning and device commands.
- Example **`examples/05_ble_json`**: reference JSON command contract over BLE (includes `wifi.set` / `wifi.clear` demo).
- English SDK docs **`docs/sdk/esp-idf/ble-json-channel.md`** and **`docs/sdk/esp-idf/ble-json-provisioning.md`**.

### Fixed

- NimBLE `BLE_UUID128_INIT` byte order for clock field so centrals see canonical UUID `…-8024-…`.
- Legacy advertising payload size: service UUID in advertising PDU, GAP name in **scan response** (fits within 31-byte legacy limits).

### Changed

- BLE provisioning is now defined as a JSON contract over `iotmer_ble` (transport), instead of a dedicated BLE provisioning component.
