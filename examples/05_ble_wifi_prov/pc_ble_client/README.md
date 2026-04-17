# PC BLE test client (example 05)

Small **[Bleak](https://github.com/hbldh/bleak)** script to exercise **IOTMER BLE Wi‑Fi provisioning (GATT v1)** from a desktop or laptop against firmware built from **`examples/05_ble_wifi_prov`**.

Protocol and UUIDs: [`docs/sdk/esp-idf/ble-wifi-provisioning.md`](../../../docs/sdk/esp-idf/ble-wifi-provisioning.md).

## Requirements

- Python **3.10+**
- Machine with **BLE** (most laptops; desktop + USB BLE adapter otherwise)
- ESP board running example **05** and advertising a name starting with **`IOTMER-`**

## Setup

```bash
cd examples/05_ble_wifi_prov/pc_ble_client
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

## Usage

Scan for devices (name prefix or advertised IOTMER service UUID):

```bash
python3 prov_client.py --scan-only
```

Auto-scan by name prefix, then send credentials (replace SSID/password):

```bash
python3 prov_client.py --ssid "MyWiFi" --password "secret"
```

If you already know the BLE address (format varies by OS; on macOS it is often a UUID string):

```bash
python3 prov_client.py --address 'E99C436C-3378-4E51-50B9-294F33BE3612' --ssid "MyWiFi" --password "secret"
```

Optional **PING** (`0x01`) before credential flow:

```bash
python3 prov_client.py --ping --ssid "MyWiFi" --password "secret"
```

Print the discovered **GATT tree** (raw UUID strings plus **canonical** form) to **stderr**:

```bash
python3 prov_client.py --dump-gatt --ssid "MyWiFi" --password "secret"
```

## Notes

- **Linux:** you may need `dialout` group membership or elevated rights for BlueZ, depending on distribution.
- **macOS / Windows:** usually works out of the box; if anything fails, verify the same device with **nRF Connect**.
- **macOS Core Bluetooth** sometimes shows **128-bit UUID strings** that look different from the RFC strings in the docs while still representing the same value. The script **canonicalises** those strings before matching. After **firmware UUID changes**, remove the ESP peripheral under **System Settings → Bluetooth** once to avoid a stale GATT cache.
- The script only pushes **Wi‑Fi credentials** to the device; it does **not** run IOTMER HTTPS provisioning or MQTT.
