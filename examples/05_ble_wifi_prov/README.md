# Example 05 — BLE Wi‑Fi provisioning

Minimal ESP-IDF app that uses the optional **`iotmer_ble_wifi_prov`** component (NimBLE GATT **protocol v1**) to receive STA **SSID and password** over BLE, store them with **`iotmer_wifi_set_credentials()`**, then call **`iotmer_wifi_reconnect()`** and stop BLE advertising.

This example does **not** perform IOTMER HTTPS provisioning or MQTT; it focuses only on the BLE → Wi‑Fi credential path.

## Requirements

- A **BLE-capable** Espressif target (e.g. ESP32, ESP32-C3, ESP32-S3, ESP32-C6). **Not** ESP32-S2.
- ESP-IDF with **NimBLE** enabled (see `sdkconfig.defaults` in this directory).

## Build

```bash
cd examples/05_ble_wifi_prov
idf.py set-target esp32c3    # or esp32, esp32s3, esp32c6, …
idf.py build
```

`EXTRA_COMPONENT_DIRS` in this example’s root **`CMakeLists.txt`** points at the repository **`components/`** tree so both **`iotmer`** and **`iotmer_ble_wifi_prov`** are available.

## Configure

```bash
idf.py menuconfig
```

- **Component config → IOTMER** — Wi‑Fi STA defaults, optional IOTMER cloud fields if you extend the app.
- **Component config → IOTMER BLE Wi‑Fi provisioning** — advertising / session timeouts, GAP name prefix, SMP IO capability.
- **Component config → Bluetooth** — ensure **NimBLE** host (and controller) are enabled for the target.

Do not commit real Wi‑Fi or API secrets in `sdkconfig`; use **`sdkconfig.defaults`** or local `sdkconfig` only.

## Run-time behaviour

1. On boot, the firmware initialises NimBLE and **`iotmer_ble_wifi_prov`**.
2. The device advertises as **`IOTMER-`** + short hex suffix (from MAC).
3. A central (phone app, **nRF Connect**, or the **Python Bleak** client under `pc_ble_client/`) connects, enables **Events** notifications, and follows the opcode sequence in **`docs/sdk/esp-idf/ble-wifi-provisioning.md`**.
4. After a successful **COMMIT**, the example calls **`iotmer_wifi_reconnect()`** and **`iotmer_ble_wifi_prov_stop()`**.

## PC test client (optional)

Folder **`pc_ble_client/`** — small **Bleak** script for macOS / Linux / Windows. See **`pc_ble_client/README.md`**.

## Further reading

- [BLE Wi‑Fi provisioning (protocol v1)](../../docs/sdk/esp-idf/ble-wifi-provisioning.md)
- [`components/iotmer_ble_wifi_prov/README.md`](../../components/iotmer_ble_wifi_prov/README.md)
