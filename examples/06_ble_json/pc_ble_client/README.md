# PC BLE client (Python + Bleak) — `iotmer_ble` JSON provisioning

Small desktop client that connects to the device’s **`iotmer_ble`** service and exchanges JSON:

- Writes JSON requests to **RX**
- Receives JSON responses via **TX notifications**

## Requirements

- Python 3.9+
- BLE adapter (macOS / Linux / Windows)

## Install

```bash
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

From this directory:

```bash
python client.py --scan
python client.py --name-prefix IOTMER- --ping
python client.py --name-prefix IOTMER- --wifi-set --ssid MyWiFi --pass MyPassword
python client.py --name-prefix IOTMER- --wifi-clear
```

## UUIDs

- Service: `1d14d6ee-1001-4000-8024-b5a3c0ffee01`
- RX (write): `1d14d6ee-1002-4000-8024-b5a3c0ffee01`
- TX (notify/read): `1d14d6ee-1003-4000-8024-b5a3c0ffee01`

If you change firmware UUIDs, macOS may cache GATT; remove the device from **System Settings → Bluetooth** once.

