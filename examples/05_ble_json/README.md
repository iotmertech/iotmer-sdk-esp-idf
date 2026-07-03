# 05_ble_json

BLE GATT JSON channel demo (`iotmer_ble`). Implements `ping`, `wifi.set`, `wifi.clear`, and optional bind-claim.

Full protocol: [BLE JSON provisioning](https://docs.iotmer.com/docs/sdk/esp-idf/ble-json-provisioning)

## Build

```bash
idf.py set-target esp32c3
idf.py menuconfig    # IOTMER BLE + Bluetooth → NimBLE
idf.py build flash monitor
```

Default GAP name prefix: `MER-` + MAC hex (`sdkconfig.defaults`).

## GATT

| Characteristic | UUID |
|----------------|------|
| Service | `1d14d6ee-1001-4000-8024-b5a3c0ffee01` |
| RX (write) | `1d14d6ee-1002-4000-8024-b5a3c0ffee01` |
| TX (notify) | `1d14d6ee-1003-4000-8024-b5a3c0ffee01` |

Enable TX notifications before sending requests.

## Commands

| Request | Response |
|---------|----------|
| `{"type":"ping","rid":"1"}` | `{"type":"pong","rid":"1"}` |
| `{"type":"wifi.set","rid":"2","ssid":"…","pass":"…"}` | `{"type":"wifi.set.ok","rid":"2"}` |
| `{"type":"wifi.clear","rid":"3"}` | `{"type":"wifi.clear.ok","rid":"3"}` |

Add `"claim_code":"pc_…"` to `wifi.set` for bind-claim. HTTP 410 = expired claim.

## macOS

CBError 14 ("Peer removed pairing information"): remove device from **System Settings → Bluetooth**, retry.

## PC client

`pc_ble_client/` — Python + Bleak test harness.
