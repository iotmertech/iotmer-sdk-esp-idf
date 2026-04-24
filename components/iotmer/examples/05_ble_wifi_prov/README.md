# 05_ble_wifi_prov

ESP-IDF sample using **`iotmer_ble_wifi_prov`** (NimBLE GATT **protocol v1**) for STA **SSID/password**, optional **Claim** JSON (`claim.set` / `claim.ack` on UUID `…0404…`), then `iotmer_wifi_reconnect`, **`iotmer_provision()`** (HTTPS `POST …/provision/device` + optional **`device_http_token`**), NVS save, and optional **`iotmer_device_auth_bind_claim()`**.

This project is a **shipped copy** in the `iotmertech/iotmer` package so it can appear in the [ESP Component Registry](https://components.espressif.com/) **Examples** tab. The [monorepo `examples/05_ble_wifi_prov`](https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/05_ble_wifi_prov) is the day-to-day path when developing this repository with **local** `components/` (see that README for `EXTRA_COMPONENT_DIRS`); keep them in sync when you change `main/`, Kconfig, or `sdkconfig.defaults`.

## Requirements

- A **BLE-capable** Espressif target (e.g. ESP32, ESP32-C3, ESP32-S3, ESP32-C6). **Not** ESP32-S2.
- [ESP Component Manager](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html): this tree resolves **`iotmertech/iotmer`** from the same component (`path: "../.."` in `idf_component.yml`) and **`iotmertech/iotmer_ble_wifi_prov`** from the registry.
- [Full mbedTLS CA bundle in `sdkconfig.defaults`](https://github.com/iotmertech/iotmer-sdk-esp-idf/blob/main/docs/sdk/esp-idf/provisioning.md) (same as other IOTMER HTTPS examples) for TLS to the default console API.

## Build

`CMakeLists.txt` sets **`EXTRA_COMPONENT_DIRS`** to the parent of the `iotmer/` component directory (repo `components/` in the monorepo, or your project’s `managed_components/` when this tree lives under `…/iotmertech__iotmer/examples/…`) so `main`’s **`REQUIRES iotmer iotmer_ble_wifi_prov`** resolves. The project `idf_component.yml` adds **`iotmertech/iotmer_ble_wifi_prov`** from the registry and pins **`iotmertech/iotmer`** to the same package via `path: "../.."`.

```bash
cd 05_ble_wifi_prov
idf.py set-target esp32c3
idf.py build
```

For a **monorepo** checkout, you can also use [GitHub `examples/05_ble_wifi_prov`](https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples/05_ble_wifi_prov) (local `components/` via `../../components` in that copy’s `CMakeLists.txt`).

## Configure

- **Component config → IOTMER** — Wi‑Fi STA, provision API URL, auth code, workspace, …
- **IOTMER example 05 (BLE Wi‑Fi + claim demo)** — optional lab **demo claim code** if you do not use GATT `claim.set`.
- **Component config → IOTMER BLE Wi‑Fi provisioning** — advertising, timeouts, GAP name.
- **Bluetooth** — **NimBLE** enabled for the target (see `sdkconfig.defaults`).

## PC test client (optional)

`pc_ble_client/` — Bleak-based script. See `pc_ble_client/README.md` in this directory.

## Further reading

- [BLE Wi‑Fi provisioning (GATT v1) — in-repo](https://github.com/iotmertech/iotmer-sdk-esp-idf/blob/main/docs/sdk/esp-idf/ble-wifi-provisioning.md)
- [iotmer_ble_wifi_prov — in-repo](https://github.com/iotmertech/iotmer-sdk-esp-idf/blob/main/components/iotmer_ble_wifi_prov/README.md)
- [BLE + Claim — mobile integration (v1)](https://github.com/iotmertech/iotmer-sdk-esp-idf/blob/main/docs/sdk/esp-idf/ble-mobile-integration-v1.md)
