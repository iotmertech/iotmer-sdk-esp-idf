# Examples

Reference firmware for the `iotmer` component. Create a workspace in the [console](https://iotmer.com) before flashing.

**Custom PCB?** Use [`tools/iotmer.py create-app`](../tools/README.md) for a `hw/` + `cloud/` scaffold, or copy these examples as-is.

## Factory vs field

| Profile | Example | Auth code | Use |
|---------|---------|-----------|-----|
| **Factory** | [`01_provisioning`](01_provisioning/) | Set in menuconfig | Production line: HTTPS provision + OTA |
| **Field** | [`02_telemetry`](02_telemetry/) | Empty | Deployed device: NVS session + MQTT |

Details: [`docs/sdk/esp-idf/factory-field-profiles.md`](../docs/sdk/esp-idf/factory-field-profiles.md)

## Examples

| Example | Use case |
|---------|----------|
| [`01_provisioning`](01_provisioning/) | Factory: HTTPS provision, NVS, optional OTA |
| [`02_telemetry`](02_telemetry/) | Field: MQTT connect, subscribe, telemetry loop |
| [`03_lwt_presence`](03_lwt_presence/) | Retained JSON presence + MQTT LWT |
| [`04_config`](04_config/) | MQTT Config Protocol |
| [`05_ble_json`](05_ble_json/) | BLE JSON channel (`wifi.set`, `wifi.clear` demo) |

## Build

**Do not run `idf.py` from the repository root** — there is no firmware project there. Always `cd` into an example:

```bash
cd examples/02_telemetry
idf.py set-target esp32c3
idf.py build flash monitor
```

After clone, run `set-target` once — `build/`, `managed_components/`, and `sdkconfig` are gitignored and must not be committed.

Clean accidental artifacts:

```bash
bash ../tools/ci/clean.sh   # from examples/, or from repo root
```

## Configure

`idf.py menuconfig` → **Component config → IOTMER**

| Setting | When to set |
|---------|-------------|
| `IOTMER_PROVISION_AUTH_CODE` | Factory image (`01`). Leave empty in field firmware after first provision. |
| `IOTMER_WORKSPACE_ID` | Required when auth code is set |
| `IOTMER_WIFI_SSID` / `PASSWORD` | Always (or store via BLE / NVS) |
| `IOTMER_WORKSPACE_SLUG` | Optional override. Normally comes from provision JSON → NVS. |

Kconfig sources: `components/iotmer/Kconfig.projbuild`, `components/iotmer_ble/Kconfig.projbuild`

Use `sdkconfig.defaults` for CI. Do not commit production secrets.

Validate before release:

```bash
python ../tools/iotmer.py doctor --project .
```

## Boot flow

`iotmer_init()` on each boot:

1. Connect Wi‑Fi
2. Load NVS credentials
3. HTTPS provision (skipped when auth code is empty and NVS session is complete)
4. Save credentials
5. HTTPS OTA (when firmware metadata is present)

MQTT topics: `{workspace_slug}/{device_key}/…` — see [docs.iotmer.com](https://docs.iotmer.com/).

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `idf.py: command not found` | Source ESP-IDF `export.sh` |
| Serial permission denied (Linux) | Add user to `dialout`, re-login |
| Wrong chip | `idf.py set-target <chip>` |
| TLS verify errors | Match example `sdkconfig.defaults`; `idf.py fullclean build` |
| MQTT `not authorized` | Re-provision with auth code or rotate console credentials |
| Task WDT during TLS | Increase `CONFIG_ESP_TASK_WDT_TIMEOUT_S` and main stack per example defaults |
| BLE: missing `ble_gap.h` | `REQUIRES iotmer_ble` in app CMakeLists; `idf.py fullclean` |
| BLE (macOS): UUID not found | Compare 128-bit values; remove peripheral from Bluetooth settings |

## Component layout

```
components/iotmer/
├── include/iotmer_client.h   # public API
├── include/iotmer_config.h   # config protocol API
├── iotmer_client.c           # init, connect, MQTT
├── iotmer_provision.c        # HTTPS provision
├── iotmer_nvs.c              # credentials
├── iotmer_ota.c              # HTTPS OTA
├── iotmer_wifi.c             # Wi‑Fi STA
└── …

components/iotmer_ble/
└── include/iotmer_ble.h      # optional BLE JSON transport
```

`iotmer_internal.h` is component-private — do not include from application code.

Application must call `nvs_flash_init()` once before `iotmer_init()`.
