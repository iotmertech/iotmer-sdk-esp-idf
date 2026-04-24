# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
for published **components** (`components/iotmer/idf_component.yml` versions).

## [Unreleased]

## [0.1.12] - 2026-04-24

### Added

- **`components/iotmer/examples/05_ble_wifi_prov/`** — BLE + Wi‑Fi + Claim + bind-claim example packaged with the **`iotmertech/iotmer`** component so it appears in the **ESP Component Registry → Examples** tab (registry-style `idf_component.yml` with **`iotmertech/iotmer_ble_wifi_prov` ^0.1.2**). The monorepo path **`examples/05_ble_wifi_prov/`** remains the develop-with-local-`components/` copy; keep them in sync.

## [0.1.11] - 2026-04-24

### Added

- **`iotmer`:** `device_http_token` in `iotmer_creds_t` (from `POST …/provision/device`), NVS key `dht`, and `iotmer_device_auth_bind_claim()` for `POST …/devices/auth/bind-claim` (Bearer `dht_…`).
- **BLE:** GATT **Claim** characteristic (`…0404…`) with JSON `claim.set` / `claim.ack`; RAM-only `claim_code` getters/clear.
- **Example 05 / `pc_ble_client`:** `--claim` / `--claim-after-pass`; optional Kconfig `IOTMER_05_DEMO_CLAIM_CODE` for lab bind without GATT claim.
- Optional component **`iotmer_ble_wifi_prov`**: NimBLE GATT Wi‑Fi credential provisioning (protocol v1), public API `iotmer_ble_wifi_prov.h`.
- Example **`examples/05_ble_wifi_prov`**: minimal integration with `iotmer_wifi_reconnect()` after successful COMMIT.
- Reference Python client **`examples/05_ble_wifi_prov/pc_ble_client`** (Bleak) for desktop testing.
- English SDK docs: **`ble-wifi-provisioning.md`**, **`ble-mobile-integration-v1.md`** (mobile GATT/claim wire contract), other `docs/sdk/esp-idf/` pages updated for **Claim** GATT, **`device_http_token` / bind-claim**, example **05** and **`pc_ble_client`**.

### Changed

- **Docs (`docs/sdk/esp-idf/`):** `intro`, `index`, `examples`, `configuration`, `provisioning`, `troubleshooting`, `ble-wifi-provisioning` updated for the features above.
- **`iotmer_provision`:** after a successful parse, logs whether **`device_http_token`** is `set` or `absent` (token value is **never** logged), aligned with current **`POST …/provision/device`** JSON.
- **`iotmer` Kconfig:** BLE-specific options moved to `components/iotmer_ble_wifi_prov/Kconfig.projbuild` so symbols exist whenever the companion component is present in the build.

### Fixed

- NimBLE `BLE_UUID128_INIT` byte order for clock field so centrals see canonical UUID `…-8024-…` (matches `IOTMER_BLE_WIFI_PROV_UUID_*_STR` in the public header).
- Legacy advertising payload size: service UUID in advertising PDU, GAP name in **scan response** (fits within 31-byte legacy limits).
- `iotmer_ble_wifi_prov` **CMake** always declares `REQUIRES bt` / `PRIV_REQUIRES bt` so NimBLE headers resolve; stub path when `CONFIG_IOTMER_BLE_WIFI_PROV` is off is implemented in C with `#if CONFIG_IOTMER_BLE_WIFI_PROV`.
