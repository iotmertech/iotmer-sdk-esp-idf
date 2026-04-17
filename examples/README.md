# ESP-IDF examples

Reference applications for this repository’s `iotmer` component. **More examples will be added** under `examples/` as the SDK evolves; the table below lists what exists **today**. Create a workspace and credentials in the **[console](https://iotmer.com)** and follow **[documentation](https://docs.iotmer.com/)** when wiring devices to the cloud.

| Directory | Role |
|-----------|------|
| [`01_provisioning`](01_provisioning/) | Factory bring-up: HTTPS provision, NVS save, optional OTA. No MQTT client. |
| [`02_telemetry`](02_telemetry/) | Field-style app: MQTT connect, subscribe, telemetry loop. |
| [`03_lwt_presence`](03_lwt_presence/) | Retained presence + MQTT last-will ONLINE/OFFLINE. |
| [`04_config`](04_config/) | MQTT Config Protocol v1: `config/meta` → `config/get` → `config/resp` (gzip+base64 chunks) → `config/status`. |

## Prerequisites

- ESP-IDF installed; shell must load the IDF environment (`export.sh` / `export.ps1`) so `idf.py` is on `PATH`.
- For **01**, set **`IOTMER_PROVISION_AUTH_CODE`** and **`IOTMER_WORKSPACE_ID`** in menuconfig (and Wi‑Fi). **`workspace_slug`** for MQTT (`{workspace_slug}/{device_key}/…`) comes from the **provision API** response and is stored in NVS — **`IOTMER_WORKSPACE_SLUG`** in menuconfig is only an optional override (e.g. testing). For **02** after NVS is populated, the auth code may be left empty so HTTPS is skipped.

## First build (clone)

`build/`, `managed_components/`, and per-example `sdkconfig` are not committed (see repo `.gitignore`). After clone:

```bash
cd examples/01_provisioning   # or 02_telemetry / 04_config / …
idf.py set-target esp32c3     # or esp32, esp32s3, …
idf.py build
```

Component Manager restores dependencies from each example’s `dependencies.lock`.

## Configure

**menuconfig:** `idf.py menuconfig` → **Component config → IOTMER** (Wi‑Fi, provision auth, workspace id, optional **workspace slug** override, OTA options, …). Slug for topics normally comes from the **provision JSON**, not menuconfig.

**Or** edit `sdkconfig.defaults` (CI / templates). Do not commit production secrets in `sdkconfig` to a public repo.

Full Kconfig symbol list: `components/iotmer/Kconfig.projbuild`.

## Flash & monitor

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with the host serial device (e.g. `/dev/ttyUSB0`, `COM5`). Exit monitor: `Ctrl+]`.

## Provisioning & OTA (summary)

On each boot, `iotmer_init()` loads NVS, optionally `POST`s to `{IOTMER_PROVISION_API_URL}/provision/device` when the auth code is set, saves credentials, then may run HTTPS OTA when firmware metadata is present. If the auth code is empty and NVS holds a complete session, HTTPS is skipped.

MQTT topics follow the **console** ACL pattern `{workspace_slug}/{device_key}/…` — details in **[documentation](https://docs.iotmer.com/)**.

## Troubleshooting

| Symptom | Action |
|---------|--------|
| `idf.py: command not found` | Source ESP-IDF `export` script in the shell. |
| Linux serial permission denied | Add user to `dialout`, re-login. |
| Wrong chip | `idf.py set-target <chip>` in the example directory. |
| TLS / `s_dummy_crt` / verify errors | Ensure `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` options match the example `sdkconfig.defaults`; `fullclean` + rebuild. |
| `esp-x509-crt-bundle: No matching trusted root` | Enable cross-signed bundle verify (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`) as in defaults. |
| Task watchdog during TLS | Increase `CONFIG_ESP_TASK_WDT_TIMEOUT_S` and/or main stack (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) per example defaults. |
| MQTT `not authorized` | NVS MQTT password out of date vs console; re-run provision with auth code or rotate credentials and provision again. |

## SDK layout (reference)

```
components/iotmer/
├── include/iotmer_client.h   ← public API
├── include/iotmer_config.h   ← config protocol v1 API
├── iotmer_client.c           ← init / connect / MQTT
├── iotmer_provision.c        ← HTTPS provision
├── iotmer_nvs.c              ← credentials NVS
├── iotmer_ota.c              ← HTTPS OTA (Kconfig)
├── iotmer_telemetry.c        ← publish helpers
├── iotmer_config.c           ← MQTT Config Protocol v1 (device)
├── iotmer_topics.c           ← topic strings
└── iotmer_wifi.c             ← STA connect
```

`iotmer_internal.h` is for component sources only, not for application include paths.

Design notes: stack-built topics; manual MQTT reconnect timer; subscriptions reissued on each `MQTT_EVENT_CONNECTED`; no `nvs_flash_init()` inside the component (application must call it once).
