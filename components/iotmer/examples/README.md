## Examples

This component ships **ESP-IDF example projects** under this folder so the ESP Component Registry can index them in the **Examples** tab.

Each example is a standalone ESP-IDF project (mirrored here for the **ESP Component Registry → Examples** tab):

- `01_provisioning/`
- `02_telemetry/`
- `03_lwt_presence/`
- `04_config/` — MQTT Config Protocol v1 (`config/meta`, `config/get`, chunked `config/resp`, `config/status`)

**`05_ble_wifi_prov` is not packaged** under this folder: it depends on the sibling component **`iotmer_ble_wifi_prov`** and lives only in the **monorepo** tree below. When changing examples, update **`examples/05_ble_wifi_prov/`** on GitHub; keep **`components/iotmer/examples/01`–`04`** in sync with **`examples/01`–`04`** when their `sdkconfig.defaults`, `main`, or CMake diverge.

The canonical source for all examples:

- `https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples`

