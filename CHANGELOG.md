# Changelog

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions track `components/iotmer/idf_component.yml`.

## [Unreleased]

## [0.2.0] - 2026-07-03

Breaking release. Legacy API shims removed; all bundled examples updated.

### Added

**iotmer**

- MQTT lifecycle callbacks: `on_connected`, `on_disconnected`, `on_published`, `on_subscribed`, `on_auth_rejected`, `on_phantom_detected`, `user_ctx`
- Phantom connection watchdog (`phantom_timeout_ms`) + `iotmer_ms_since_broker_activity()`
- `iotmer_reconnect_hard()` — full MQTT client restart
- Subscription registry: `iotmer_subscribe()` / `iotmer_unsubscribe()` with wildcard dispatch
- Composable bring-up: `iotmer_wifi_up()`, public `iotmer_ota_apply_if_needed()`, `iotmer_finalize_provisioning()`
- TLS hooks: `on_tls_acquire` / `on_tls_release` + `IOTMER_TLS_MIN_HEAP_GUARD`

**iotmer_ble**

- Lifecycle callbacks: `on_connect`, `on_disconnect`, `on_subscribe`, `on_mtu`
- Configurable `device_name`
- Queued RX dispatch (`rx_queue_len`, `rx_task_stack`)
- `iotmer_ble_suspend()` / `iotmer_ble_resume()` / `iotmer_ble_is_suspended()`

### Fixed

- **Wi‑Fi:** Persistent reconnect with 15–60 s backoff after fast retries. Clears `s_connected` on disconnect. Handles already-started Wi‑Fi without 30 s timeout.
- **MQTT:** Reassembles fragmented inbound messages (`IOTMER_MQTT_RX_ASSEMBLY_MAX`, default 8 KB).
- **OTA:** SHA256 verification before image activation (`ESP_ERR_OTA_VALIDATE_FAILED` on mismatch).
- **Presence:** JSON payload on `{workspace_slug}/{device_key}/presence`. LWT: `{"status":"offline","ts":0}`.
- **Outbox:** Cap via `IOTMER_MQTT_OUTBOX_LIMIT` (default 16 KB).

### Changed

- Removed `commands_cb` / `config_cb` from `iotmer_client_t`. Use `iotmer_subscribe()` or convenience wrappers.
- Presence format: plain `ONLINE`/`OFFLINE` → JSON. Update consumers.
- Component versions: `iotmer` and `iotmer_ble` → **0.2.0**.

### Notes

- BLE encryption: `IOTMER_BLE_REQUIRE_ENC`, `IOTMER_BLE_SMP_IO_CAP` (unchanged).
- BLE multi-frame reassembly: deferred (would break existing mobile wire format).

## [0.1.16] - 2026-06-20

### Fixed

- `iotmer_provision`: send `device_key` in provision POST body when set before the call.

## [0.1.15] - 2026-06-19

### Added

- `iotmer_mqtt_publish()` — all SDK publishes use `esp_mqtt_client_enqueue()`
- Config callbacks deferred to worker task by default (`IOTMER_CONFIG_DEFER_CALLBACKS`)

### Changed

- Presence and subscribe topics: trailing slashes removed

### Fixed

- MQTT thread safety: telemetry, presence, config no longer call `esp_mqtt_client_publish()` from event handler

## [0.1.12] - 2026-04-24

### Added

- `components/iotmer/examples/05_ble_json/` for ESP Component Registry Examples tab

## [0.1.11] - 2026-04-24

### Added

- `device_http_token`, NVS key `dht`, `iotmer_device_auth_bind_claim()`
- Optional `iotmer_ble` component (NimBLE GATT JSON)
- Example `05_ble_json` + Python PC client
- SDK docs: BLE JSON channel and provisioning

### Changed

- Provision logs `device_http_token` presence (never the value)
- Updated `docs/sdk/esp-idf/` scaffold

### Fixed

- BLE advertising payload size on 31-byte limit
