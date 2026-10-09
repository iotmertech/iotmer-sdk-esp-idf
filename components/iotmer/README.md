# iotmer

ESP-IDF component for [IOTMER](https://iotmer.com): Wi‑Fi, HTTPS provisioning, NVS credentials, MQTT (TLS), telemetry, presence, and optional OTA (HTTP(S) auto-OTA and MQTT command).

Optional BLE transport: [`iotmer_ble`](../iotmer_ble/) (`iotmer_ble.h`).

## Install

Add to `idf_component.yml`:

```yaml
dependencies:
  iotmertech/iotmer: "0.3.5"
```

```bash
idf.py update-dependencies
```

This release always solves `espressif/cjson` `1.7.19~2`, `espressif/mqtt` `1.0.0`, and `espressif/zlib` `1.3.2`. ESP-IDF is the copy you install (`>=6.0.0,<6.2.0`); it is not downloaded with the component.

Registry: [`iotmertech/iotmer`](https://components.espressif.com/components/iotmertech/iotmer)

**Requires:** ESP-IDF ≥ 6.0 and &lt; 6.2 · ESP32 family

## TLS trust store

By default the SDK verifies MQTT and HTTPS peers with the ESP-IDF Mozilla CA bundle (`esp_crt_bundle_attach`).

IOTMER production hosts typically use **two different CA families**:

| Path | Default / typical host | CA family (today) |
|------|------------------------|-------------------|
| MQTT | `mqtt*.iotmer.cloud:8883` (from provision JSON) | Let’s Encrypt → **ISRG Root X1 / X2** |
| HTTPS provision / bind-claim | `CONFIG_IOTMER_PROVISION_API_URL` → `https://console.iotmer.com/api/v1` | Cloudflare → **Google Trust Services (e.g. GTS R4)** |
| HTTPS OTA | e.g. `firmwares.iotmer.com` (Cloudflare) | Same as console → **GTS R4** |

For constrained devices (e.g. ESP32-C3 + BLE), pin a small concatenated PEM of **both** roots (AWS IoT-style), not the server leaf:

```c
/* Flash: ISRG Root X1 + X2 + GTS Root R4 (NUL-terminated concat). */
extern const char iotmer_ca_store_pem[] asm("_binary_iotmer_ca_store_pem_start");

iotmer_config_t cfg = IOTMER_CONFIG_DEFAULT();
cfg.ca_cert_pem = iotmer_ca_store_pem;
iotmer_init(&client, &cfg);
```

Or call `iotmer_tls_set_ca_cert_pem()` **before** the first provision/OTA if those run without `iotmer_init`.

Rules:

- Embed **root** CAs only (not the server leaf). Leaf renewal must not require a firmware flash.
- Keep PEMs valid for the process lifetime (typically `embed_txtfiles` / `.rodata`).
- When pinned PEM is set, the bundle is **not** used for IoTMER MQTT/HTTPS paths.
- If you later move API under `*.iotmer.cloud` on the same LE chain, you can drop GTS — until then keep both.
- Plan OTA updates that can add a new root before rotating broker/API certificates.

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
| Wi‑Fi | Retries with 15–60 s backoff after fast retries exhaust. Never stops trying. `iotmer_wifi_kick()` restarts the STA without blocking. |
| MQTT fragments | Reassembles messages larger than `CONFIG_MQTT_BUFFER_SIZE` (cap: `IOTMER_MQTT_RX_ASSEMBLY_MAX`, default 8 KB). |
| OTA | HTTP(S) auto-OTA streams a GET (erase staging **before** TLS, Range resume, SHA256 on the fly vs provision checksum) then activates. MQTT `"cmd":"ota"` is application-owned (download still HTTP(S)). |
| Presence | Retained JSON on `{workspace_slug}/{device_key}/presence`. LWT on unexpected disconnect. Graceful `iotmer_disconnect()` publishes retained offline when LWT is enabled. |
| Outbox | QoS1+ queue capped at `IOTMER_MQTT_OUTBOX_LIMIT` (default 16 KB). |

Presence payload:

```json
{"status":"online","ts":1748000000}
{"status":"offline","ts":0}
```

## Wi‑Fi

`iotmer_wifi_connect()` / `iotmer_wifi_up()` block until an IP is acquired or 30 s elapses (`WIFI_CONNECT_TIMEOUT_MS`). After association the BSSID is locked. Fast retries, then a 15–60 s backoff, keep trying.

`iotmer_wifi_kick()` restarts the STA and returns immediately. It does not call `iotmer_wifi_connect()` and does not wait on the event group.

- Reconnect hold set → `ESP_ERR_INVALID_STATE`, radio unchanged. Release the hold first.
- No STA credential (NVS, otherwise Kconfig) → `ESP_ERR_INVALID_STATE`.
- Otherwise the BSSID lock is cleared through the existing station config (`lock_bssid` false): next join uses an all-channel scan and picks the strongest AP. Authmode threshold, PMF, and failure retry count stay as they are. The application does not write `wifi_config_t`.
- Clears the connected flag, fast-retry count, and backoff delay, and stops the backoff timer so a pending 15 s retry does not fight the restart.
- `esp_wifi_disconnect()`, then `esp_wifi_stop()` (the started flag is cleared in this call because `WIFI_EVENT_STA_STOP` is async), then `esp_wifi_start()`. `ESP_ERR_WIFI_NOT_STARTED` from disconnect/stop is ignored. Any other error from stop or `esp_wifi_start()` is returned.
- `ESP_OK` means start was requested, not that an IP exists. `WIFI_EVENT_STA_START` calls `esp_wifi_connect()` when autoconnect is on; kick does not connect again.

`iotmer_wifi_set_autoconnect()` defaults to enabled (today's behavior: `STA_START` always connects, and disconnect/backoff retry). Set it false before a provisioning scan that calls `esp_wifi_start()` while the radio was stopped for BLE: `STA_START` only marks the radio started and does not join the saved AP, and disconnect/backoff do not reconnect until autoconnect is enabled again. Credentials are not cleared. Turning autoconnect back on does not itself connect — call `iotmer_wifi_kick()` or `iotmer_wifi_connect()`.

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

## HTTPS OTA (Cloudflare / ESP32-C3)

`iotmer_ota_apply_if_needed()` downloads with a raw `esp_http_client` GET — not `esp_https_ota`. Flash erase of the staging slot runs **before** the TLS handshake so the CDN body is not stalled by CPU-bound erase. SHA256 is updated per 4 KB chunk and compared to `firmware_checksum_sha256`; mismatch aborts without changing the boot partition. Same-SHA skip and `IOTMER_OTA_APPLY_EVEN_IF_SAME_SHA` are unchanged.

Required for Cloudflare/R2 (16 KB TLS records):

```
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
```

A 4–8 KB IN buffer fails around ~50 KB with `MBEDTLS_ERR_SSL_INVALID_RECORD` (-0x7200). Timeout: 120 s is often too short for ~1.5 MB on C3; use 180–300 s (`CONFIG_IOTMER_OTA_TIMEOUT_MS`). Factory example `01_provisioning` sets 300 s.

## Low-RAM targets

Add to `sdkconfig.defaults` when BLE and TLS coexist:

```
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
```

Keep `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` even with asymmetric buffers — Cloudflare OTA needs a 16 KB inbound record.

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
