# PC BLE client

Python + Bleak desktop client for the `iotmer_ble` JSON channel.

Writes requests to **RX**, reads responses from **TX** notifications.

## Setup

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

```bash
python client.py --scan
python client.py --name-prefix MER- --ping
python client.py --name-prefix MER- --wifi-set --ssid MyWiFi --pass MyPassword
python client.py --name-prefix MER- --wifi-clear
```

## UUIDs

| Characteristic | UUID |
|----------------|------|
| Service | `1d14d6ee-1001-4000-8024-b5a3c0ffee01` |
| RX | `1d14d6ee-1002-4000-8024-b5a3c0ffee01` |
| TX | `1d14d6ee-1003-4000-8024-b5a3c0ffee01` |

## macOS: CBError 14

Stale BLE bond on Mac — not a firmware bug. After reflash or UUID change:

1. Remove `MER-…` from **System Settings → Bluetooth**
2. Toggle Bluetooth off/on if needed
3. Retry

With `IOTMER_BLE_REQUIRE_ENC=y`, pairing is required on first connect. A broken bond triggers the same error — forget the device and re-pair.
