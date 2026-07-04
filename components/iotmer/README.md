# iotmer

ESP-IDF component for [IOTMER](https://iotmer.com): Wi‑Fi, HTTPS provisioning, NVS credentials, MQTT (TLS), telemetry, presence, and optional HTTPS OTA.

Optional BLE transport: [`iotmer_ble`](../iotmer_ble/) (`iotmer_ble.h`).

## Install

Add to `idf_component.yml`:

```yaml
dependencies:
  iotmertech/iotmer: "*"
```

```bash
idf.py update-dependencies
```

Registry: [`iotmertech/iotmer`](https://components.espressif.com/components/iotmertech/iotmer)

**Requires:** ESP-IDF ≥ 5.0 · ESP32 family

## Quick start

```c
#include "nvs_flash.h"
#include "iotmer_client.h"

void app_main(void)
{
    nvs_flash_init();

    iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
    iotmer_client_t client;

    iotmer_init(&client, &cfg);
    iotmer_connect(&client);
}
```

Publish telemetry after connect:

```c
iotmer_telemetry_publish(&client, "{\"temp\":22.5}");
```

Subscribe to commands:

```c
static void on_command(const char *topic, const char *payload, int len, void *ctx)
{
    (void)topic; (void)len; (void)ctx;
    /* handle payload */
}

iotmer_subscribe_commands(&client, on_command, NULL);
```

Wildcards (`+`, `#`) are supported. Filters re-subscribe automatically on reconnect:

```c
iotmer_subscribe(&client, "myws/mydev/custom/#", 1, on_custom, NULL);
```

## Built-in behavior

| Feature | Behavior |
|---------|----------|
| Wi‑Fi | Retries with 15–60 s backoff after fast retries exhaust. Never stops trying. |
| MQTT fragments | Reassembles messages larger than `CONFIG_MQTT_BUFFER_SIZE` (cap: `IOTMER_MQTT_RX_ASSEMBLY_MAX`, default 8 KB). |
| OTA | Verifies SHA256 against provision checksum before activating the image. |
| Presence | Retained JSON on `{workspace_slug}/{device_key}/presence`. LWT on unexpected disconnect. Graceful `iotmer_disconnect()` publishes retained offline when LWT is enabled. |
| Outbox | QoS1+ queue capped at `IOTMER_MQTT_OUTBOX_LIMIT` (default 16 KB). |

Presence payload:

```json
{"status":"online","ts":1748000000}
{"status":"offline","ts":0}
```

## Configuration

Set optional callbacks on `iotmer_config_t` before `iotmer_init()`:

| Field | Use |
|-------|-----|
| `on_connected` / `on_disconnected` | MQTT session lifecycle |
| `on_published` / `on_subscribed` | PUBACK / SUBACK |
| `on_auth_rejected` | CONNACK rc 4 or 5 — trigger re-provision |
| `on_phantom_detected` | Half-open connection detected |
| `phantom_timeout_ms` | Idle timeout before hard reconnect (0 = off) |
| `on_tls_acquire` / `on_tls_release` | Free RAM before TLS handshake (e.g. suspend BLE) |

Call `iotmer_reconnect_hard()` for a full MQTT client restart when the socket is dead.

Config pull stuck? Call `iotmer_config_abort()` or wait for `IOTMER_CONFIG_TRANSFER_TIMEOUT_MS` (default 30 s).

## Advanced bring-up

`iotmer_init()` wraps the full sequence. For staged boot, call steps directly:

```
iotmer_wifi_up() → iotmer_nvs_load_creds() → iotmer_provision()
→ iotmer_nvs_save_creds() → iotmer_ota_apply_if_needed() → iotmer_connect()
```

Use `iotmer_finalize_provisioning()` to disconnect and optionally reboot after a claim flow.

**Stack:** Give the task that calls `iotmer_init()` ≥ 8 KB — it runs Wi‑Fi (blocking) and HTTPS provisioning (TLS).

## Low-RAM targets

Add to `sdkconfig.defaults` when BLE and TLS coexist:

```
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
```

Pair with `CONFIG_IOTMER_TLS_MIN_HEAP_GUARD` and `iotmer_ble_suspend()`.

## Examples

| Example | Purpose |
|---------|---------|
| `01_provisioning` | Factory HTTPS provision + OTA |
| `02_telemetry` | Field MQTT telemetry |
| `03_lwt_presence` | Presence + LWT |
| `04_config` | MQTT Config Protocol |
| `05_ble_json` | BLE JSON channel demo |

[Full list](https://github.com/iotmertech/iotmer-sdk-esp-idf/tree/main/examples)

## Docs

- Platform: [docs.iotmer.com](https://docs.iotmer.com/)
- ESP-IDF guide: [docs.iotmer.com/docs/sdk/esp-idf/](https://docs.iotmer.com/docs/sdk/esp-idf/)
