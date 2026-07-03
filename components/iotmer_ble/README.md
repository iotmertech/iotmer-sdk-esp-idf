# iotmer_ble

Optional ESP-IDF component: NimBLE GATT JSON channel (RX write / TX notify).

Registry: [`iotmertech/iotmer_ble`](https://components.espressif.com/components/iotmertech/iotmer_ble)

```yaml
dependencies:
  iotmertech/iotmer_ble: "*"
```

Provisioning over BLE is built on top of this transport — see `examples/05_ble_json` and the [BLE JSON provisioning contract](../../docs/sdk/esp-idf/ble-json-provisioning.md).

## GATT layout

| Characteristic | UUID constant | Direction |
|----------------|---------------|-----------|
| Service | `IOTMER_BLE_UUID_SVC_STR` | — |
| RX | `IOTMER_BLE_UUID_RX_STR` | Central → device (write) |
| TX | `IOTMER_BLE_UUID_TX_STR` | Device → central (notify) |

## Configuration

Header: `include/iotmer_ble.h` · Kconfig: **Component config → IOTMER BLE**

| Field | Description |
|-------|-------------|
| `device_name` | GAP name. NULL → `{prefix}{mac_hex}` |
| `on_rx_json` | Incoming JSON. Runs on NimBLE host thread by default. |
| `on_connect` / `on_disconnect` / `on_subscribe` / `on_mtu` | Connection lifecycle |
| `rx_queue_len` / `rx_task_stack` | Set `rx_queue_len > 0` to dispatch RX on a worker task (recommended) |

## Suspend / resume

On RAM-constrained chips (e.g. ESP32-C3), suspend BLE before a TLS handshake:

```c
iotmer_ble_suspend();   /* stop + deinit, release controller RAM */
/* … MQTT or HTTPS TLS … */
iotmer_ble_resume();    /* restore advertising */
```

Pair with `iotmer_config_t.on_tls_acquire` / `on_tls_release`.

## Notes

- Service UUID goes in the advertising PDU; GAP name in scan response (31-byte limit).
- `CONFIG_IOTMER_BLE_REQUIRE_ENC=y` requires pairing before RX writes.
- Max RX frame: `CONFIG_IOTMER_BLE_MAX_RX_LEN`. No multi-frame reassembly — keep JSON within one write or raise MTU.
- If BLE never starts on some boot paths, call `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` in `app_main()` to reclaim Classic BT RAM.
