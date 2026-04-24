# PC BLE test client (example 05)

Small **[Bleak](https://github.com/hbldh/bleak)** script for **IOTMER BLE Wi‑Fi provisioning (GATT v1)** and optional **Claim** JSON on UUID `…0404…` (`claim.set` / `claim.ack` on **Events**). Use against firmware built from **`examples/05_ble_wifi_prov`**.

Protocol: [`docs/sdk/esp-idf/ble-wifi-provisioning.md`](../../../docs/sdk/esp-idf/ble-wifi-provisioning.md).

Firmware on the board must be built with the **mbedTLS certificate bundle** options in **`examples/05_ble_wifi_prov/sdkconfig.defaults`** (full + cross-signed) so **HTTPS provision** and **bind-claim** after BLE succeed; see the **HTTPS / TLS** section in that doc.

## Requirements

- Python **3.10+**
- A machine with **BLE** (most laptops; otherwise USB Bluetooth)
- Board running example **05**, advertising a name like **`IOTMER-`…**

## Setup

```bash
cd examples/05_ble_wifi_prov/pc_ble_client
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

## Usage

Scan:

```bash
python3 prov_client.py --scan-only
```

Wi‑Fi only:

```bash
python3 prov_client.py --ssid "MyWiFi" --password "secret"
```

**Claim code** (e.g. from `POST /moid/provision/claims`) — sent **before COMMIT** by default (firmware may stop BLE right after a successful commit):

```bash
python3 prov_client.py --claim "pc_your_code_here" --ssid "MyWiFi" --password "secret"
```

Send the claim **after** password writes, just before **COMMIT**:

```bash
python3 prov_client.py --claim "pc_..." --claim-after-pass --ssid "MyWiFi" --password "secret"
```

Known BLE address:

```bash
python3 prov_client.py --address '…' --claim "pc_…" --ssid "MyWiFi" --password "secret"
```

Optional **PING** first:

```bash
python3 prov_client.py --ping --claim "pc_…" --ssid "MyWiFi" --password "secret"
```

Print **GATT tree** to stderr:

```bash
python3 prov_client.py --dump-gatt --ssid "MyWiFi" --password "secret"
```

## Notes

- **Linux:** you may need `dialout` (BlueZ) or equivalent; distro-dependent.
- **macOS / Windows:** often works as-is; compare with **nRF Connect** if in doubt.
- **macOS Core Bluetooth** may show UUID strings that look different from the RFC form; the script **canonicalises** 128-bit values. After a firmware UUID change, remove the device under **System Settings → Bluetooth** once.
- The script does **not** call your backend; it only drives GATT. The device firmware then runs **HTTPS provision** and **bind-claim** as in `main/main.c`.
- If the peripheral does not expose the **Claim** characteristic, update the firmware; the script now expects **ctrl / data / claim / events**.

## Lab without `--claim`

In **menuconfig**, open **IOTMER example 05** and set **Optional: demo claim code** to test `bind-claim` without writing the Claim GATT (leave empty for production).
