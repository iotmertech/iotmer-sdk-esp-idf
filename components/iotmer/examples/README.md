## Examples

This component ships **ESP-IDF example projects** under this folder so the ESP Component Registry can index them in the **Examples** tab.

Each example is a standalone ESP-IDF project (mirrored here for the **ESP Component Registry → Examples** tab):

- `01_provisioning/`
- `02_telemetry/`
- `03_lwt_presence/`
- `04_config/` — MQTT Config Protocol (`config/meta`, `config/get`, chunked `config/resp`, `config/status`)
- `05_ble_json/` — BLE JSON channel + provisioning demo (uses `iotmer_ble`)

When changing examples, keep **`components/iotmer/examples/*`** in sync with the **monorepo** copies under **`examples/*`** (same `main/`, `sdkconfig.defaults`, and Kconfig) — see each pair’s `README` for the dev path with **local** `components/`.

The canonical source for all examples:

- `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples`

