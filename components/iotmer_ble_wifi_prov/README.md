# `iotmer_ble_wifi_prov` (optional ESP-IDF component)

**IOTMER BLE Wi‑Fi provisioning** — NimBLE GATT server implementing **protocol v1**: transfer Wi‑Fi STA SSID/password from a phone or PC to the device, then persist via `iotmer_wifi_set_credentials()` (same NVS layout as the core `iotmer` component).

This component is **optional**: applications add it via `EXTRA_COMPONENT_DIRS` or a local path dependency and `REQUIRES iotmer_ble_wifi_prov`. It does **not** replace Espressif `wifi_prov_mgr` or its phone apps; the byte layout is IOTMER-specific.

## Public API

- Header: [`include/iotmer_ble_wifi_prov.h`](include/iotmer_ble_wifi_prov.h)
- Kconfig: [`Kconfig.projbuild`](Kconfig.projbuild) (under **Component config → IOTMER BLE Wi‑Fi provisioning** when the component is in the project)

## Documentation

- **Protocol, GATT table, opcodes, events, security:** [`docs/sdk/esp-idf/ble-wifi-provisioning.md`](../../docs/sdk/esp-idf/ble-wifi-provisioning.md) (English; copy into the main docs site if needed)
- **Reference firmware:** [`examples/05_ble_wifi_prov/`](../../examples/05_ble_wifi_prov/)
- **Python test client (Bleak):** [`examples/05_ble_wifi_prov/pc_ble_client/`](../../examples/05_ble_wifi_prov/pc_ble_client/)

## Requirements

- ESP-IDF ≥ 5.0 (NimBLE host + controller enabled for your target)
- `CONFIG_SOC_BT_SUPPORTED` targets only (no BLE on e.g. ESP32-S2)
- `main` (or another component) must `REQUIRES iotmer_ble_wifi_prov` and link `iotmer` / Wi‑Fi stack as appropriate

## License

MIT — same as the repository root [`LICENSE`](../../LICENSE).
