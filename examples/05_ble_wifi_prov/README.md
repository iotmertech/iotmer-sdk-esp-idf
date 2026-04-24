# Example 05 — BLE Wi‑Fi provisioning + claim + bind

**Registry:** the same project is also shipped under **`components/iotmer/examples/05_ble_wifi_prov/`** for the [ESP Component Registry](https://components.espressif.com/) **Examples** tab (`iotmertech/iotmer`). When you change `main/`, `sdkconfig.defaults`, or Kconfig here, update that copy too.

ESP-IDF sample using **`iotmer_ble_wifi_prov`** (NimBLE GATT **protocol v1**) for STA **SSID/password**, optional **Claim** JSON (`claim.set` / `claim.ack` on UUID `…0404…`), then:

1. **`iotmer_wifi_reconnect()`** and **`iotmer_ble_wifi_prov_stop()`**
2. **`iotmer_provision()`** — HTTPS `POST …/provision/device` → MQTT creds + **`device_http_token`** (NVS key `dht`)
3. **`iotmer_nvs_save_creds()`**
4. If a **`claim_code`** was received over BLE: **`iotmer_device_auth_bind_claim()`** → `POST …/devices/auth/bind-claim`, then **`iotmer_ble_wifi_prov_clear_claim_code()`** and zero the stack buffer

Set **`IOTMER_PROVISION_AUTH_CODE`**, **`IOTMER_WORKSPACE_ID`**, and Wi‑Fi in **menuconfig** for the HTTPS and bind steps to succeed. The app partition is **large** (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` in `sdkconfig.defaults`) so the link fits BLE + iotmer + cJSON.

`sdkconfig.defaults` enables **`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`** and **`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`** so **`iotmer_provision()`** and **`iotmer_device_auth_bind_claim()`** can complete TLS to the default console API host (same requirement as **`examples/01_provisioning`**). If you see `esp-x509-crt-bundle: No matching trusted root` on HTTPS, re-run **`idf.py fullclean build`** after pulling these defaults or align your `sdkconfig` with that block.

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

- **Component config → IOTMER** — Wi‑Fi STA, provision API URL, **provision auth code**, **workspace_id**, template, etc.
- **IOTMER example 05 (BLE Wi‑Fi + claim demo)** — optional **demo claim code** (lab only) if you do not use GATT `claim.set`.
- **Component config → IOTMER BLE Wi‑Fi provisioning** — advertising / session timeouts, GAP name prefix, SMP IO capability.
- **Component config → Bluetooth** — ensure **NimBLE** host (and controller) are enabled for the target.

Do not commit real Wi‑Fi or API secrets in `sdkconfig`; use **`sdkconfig.defaults`** or local `sdkconfig` only.

## Run-time behaviour

1. On boot, the firmware initialises **`iotmer_ble_wifi_prov`** and starts advertising.
2. A central connects, enables **Events** notifications, and may write **Claim** JSON, then the usual **Control/Data** sequence (see **`docs/sdk/esp-idf/ble-wifi-provisioning.md`**).
3. After a successful **COMMIT**, the example reconnects Wi‑Fi, stops BLE, runs **HTTPS provision**, saves NVS, and optionally **bind-claim** if a `claim_code` is in RAM.

## PC test client (optional)

Folder **`pc_ble_client/`** — **Bleak** script with **`--claim`** and **`--claim-after-pass`** for the Claim GATT. See **`pc_ble_client/README.md`**.

## Further reading

- [BLE Wi‑Fi provisioning (protocol v1)](../../docs/sdk/esp-idf/ble-wifi-provisioning.md)
- [`components/iotmer_ble_wifi_prov/README.md`](../../components/iotmer_ble_wifi_prov/README.md)
