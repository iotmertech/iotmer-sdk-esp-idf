import argparse
import asyncio
import json
import sys
import uuid

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError


SVC_UUID = "1d14d6ee-1001-4000-8024-b5a3c0ffee01"
RX_UUID = "1d14d6ee-1002-4000-8024-b5a3c0ffee01"
TX_UUID = "1d14d6ee-1003-4000-8024-b5a3c0ffee01"


def _norm_uuid(u: str) -> str:
    # Some platforms may format UUIDs differently; compare by 128-bit value.
    return str(uuid.UUID(u)).lower()

def _uuid_matches(candidate: str, target: str) -> bool:
    """
    macOS CoreBluetooth may display the same 128-bit UUID with a different hyphenated string.
    Compare by raw 128-bit value using a few common transformations:
    - direct bytes
    - little-endian bytes (UUID fields 0..2 swapped)
    - full 16-byte reverse (observed on some CoreBluetooth surfaces)
    - reverse of little-endian bytes
    """
    try:
        c = uuid.UUID(str(candidate))
        t = uuid.UUID(str(target))
    except Exception:
        return False

    t_variants = {
        t.bytes,
        t.bytes_le,
        t.bytes[::-1],
        t.bytes_le[::-1],
    }
    c_variants = {
        c.bytes,
        c.bytes_le,
        c.bytes[::-1],
        c.bytes_le[::-1],
    }

    return not t_variants.isdisjoint(c_variants)


async def scan_with_adv(timeout: float = 5.0):
    """
    Robust macOS-friendly scan:
    uses BleakScanner detection callback to capture AdvertisementData (local_name, service_uuids, rssi).
    Returns list of dict rows and a mapping of address->BLEDevice.
    """
    seen: dict[str, dict] = {}
    dev_by_addr: dict[str, object] = {}

    def on_detect(device, advertisement_data):
        addr = getattr(device, "address", None) or str(device)
        name = getattr(advertisement_data, "local_name", None) or getattr(device, "name", None) or "-"
        rssi = getattr(advertisement_data, "rssi", None)
        su = getattr(advertisement_data, "service_uuids", None) or []
        su_norm = [str(u).lower() for u in su]
        seen[addr] = {
            "name": name,
            "address": addr,
            "rssi": rssi,
            "service_uuids": su_norm,
        }
        dev_by_addr[addr] = device

    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    try:
        await asyncio.sleep(timeout)
    finally:
        await scanner.stop()

    rows = list(seen.values())
    rows.sort(key=lambda r: (r["name"] or "", r["address"]))
    return rows, dev_by_addr

async def find_device(timeout: float, name_prefix: str | None, service_uuid: str | None):
    rows, dev_by_addr = await scan_with_adv(timeout=timeout)
    svc_target = str(service_uuid).lower() if service_uuid else None

    def matches(row: dict) -> bool:
        if svc_target:
            for u in row.get("service_uuids", []):
                if _uuid_matches(u, svc_target):
                    return True
        if name_prefix:
            return (row.get("name") or "").startswith(name_prefix)
        return False

    for row in rows:
        if matches(row):
            addr = row["address"]
            return dev_by_addr.get(addr), row, rows

    # fallback: return first seen device if any
    if rows:
        addr = rows[0]["address"]
        return dev_by_addr.get(addr), rows[0], rows

    return None, None, rows


async def scan_once(timeout: float = 5.0):
    # Bleak API differs by platform/version:
    # - Some versions return BLEDevice without RSSI.
    # - Newer versions can return (device, advertisement_data) pairs with RSSI.
    rows = []

    try:
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)

        def _dev_name_addr(obj):
            name = getattr(obj, "name", None)
            address = getattr(obj, "address", None)
            if name is None and hasattr(obj, "__str__"):
                # Fallback for platform-specific wrapper objects.
                name = str(obj)
            if address is None and hasattr(obj, "__str__"):
                address = str(obj)
            return name, address

        devices = []

        # Possible shapes across platforms:
        # - dict[BLEDevice, AdvertisementData]
        # - dict[address(str), AdvertisementData]
        # - list[(BLEDevice, AdvertisementData)]
        if isinstance(found, dict):
            for k, adv in found.items():
                rssi = getattr(adv, "rssi", None)
                name, address = _dev_name_addr(k)
                devices.append(k)
                rows.append((name, address, rssi))
        else:
            for item in found:
                if isinstance(item, tuple) and len(item) >= 2:
                    d, adv = item[0], item[1]
                    rssi = getattr(adv, "rssi", None)
                    name, address = _dev_name_addr(d)
                    devices.append(d)
                    rows.append((name, address, rssi))
                else:
                    name, address = _dev_name_addr(item)
                    devices.append(item)
                    rows.append((name, address, None))
    except TypeError:
        devices = await BleakScanner.discover(timeout=timeout)
        for d in devices:
            rssi = getattr(d, "rssi", None)
            rows.append((d.name, d.address, rssi))

    rows.sort(key=lambda x: (x[0] or "", x[1]))
    return devices, rows


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", action="store_true", help="Scan and list nearby BLE devices")
    ap.add_argument("--name-prefix", default=None, help="Pick first device whose name starts with prefix")
    ap.add_argument("--address", default=None, help="Connect directly to a BLE address (overrides name-prefix)")
    ap.add_argument("--timeout", type=float, default=7.0, help="Scan timeout seconds")

    cmd = ap.add_mutually_exclusive_group()
    cmd.add_argument("--ping", action="store_true")
    cmd.add_argument("--wifi-set", action="store_true")
    cmd.add_argument("--wifi-clear", action="store_true")
    cmd.add_argument("--send", default=None, help="Send raw JSON string")

    ap.add_argument("--ssid", default=None)
    ap.add_argument("--pass", dest="password", default=None)
    args = ap.parse_args()

    if args.scan:
        rows, _ = await scan_with_adv(timeout=args.timeout)
        for r in rows:
            svc = ",".join(r.get("service_uuids", [])[:3])
            more = "" if len(r.get("service_uuids", [])) <= 3 else ",..."
            print(f"{(r.get('name') or '-')[:32]:32}  {r.get('address'):20}  rssi={r.get('rssi')}  svc={svc}{more}")
        return 0

    device = None
    if args.address is None:
        device, picked, _all = await find_device(timeout=args.timeout, name_prefix=args.name_prefix, service_uuid=SVC_UUID)
        if device is None:
            print("No BLE devices found.", file=sys.stderr)
            return 2
        address = picked["address"]
        print(f"Selected: name={picked['name']!r} address={address}")
    else:
        address = args.address

    rx_uuid = _norm_uuid(RX_UUID)
    tx_uuid = _norm_uuid(TX_UUID)
    rx_target = RX_UUID
    tx_target = TX_UUID

    notif_queue: asyncio.Queue[str] = asyncio.Queue()

    def on_tx(_, data: bytearray):
        try:
            text = bytes(data).decode("utf-8", errors="replace")
        except Exception:
            text = repr(bytes(data))
        notif_queue.put_nowait(text)

    try:
        async with BleakClient(device or address) as client:
            if not client.is_connected:
                print("Connect failed.", file=sys.stderr)
                return 3

            # Bleak API differs by version: some expose get_services(), others populate .services.
            svcs = None
            if hasattr(client, "get_services"):
                svcs = await client.get_services()
            else:
                svcs = getattr(client, "services", None)
                if svcs is None:
                    backend = getattr(client, "_backend", None)
                    if backend is not None and hasattr(backend, "get_services"):
                        svcs = await backend.get_services()

            if svcs is None:
                print("Failed to enumerate GATT services on this Bleak version/backend.", file=sys.stderr)
                return 4

            have_svc = any(_uuid_matches(str(s.uuid), SVC_UUID) for s in svcs)
            if not have_svc:
                print("Warning: iotmer_ble service UUID not found in GATT services (may be cached/hidden).")

            rx_char = None
            tx_char = None
            for s in svcs:
                for c in s.characteristics:
                    cu = str(c.uuid)
                    if _uuid_matches(cu, rx_target):
                        rx_char = c.uuid
                    elif _uuid_matches(cu, tx_target):
                        tx_char = c.uuid

            if rx_char is None or tx_char is None:
                print(
                    "RX/TX characteristics not found. If you changed UUIDs, remove device from OS Bluetooth cache.",
                    file=sys.stderr,
                )
                return 4

            await client.start_notify(tx_char, on_tx)

            if args.send is not None:
                payload = args.send
            elif args.ping:
                payload = json.dumps({"type": "ping", "rid": "1"})
            elif args.wifi_set:
                if not args.ssid or args.password is None:
                    print("--wifi-set requires --ssid and --pass", file=sys.stderr)
                    return 2
                payload = json.dumps({"type": "wifi.set", "rid": "2", "ssid": args.ssid, "pass": args.password})
            elif args.wifi_clear:
                payload = json.dumps({"type": "wifi.clear", "rid": "3"})
            else:
                print("No command given. Use --ping/--wifi-set/--wifi-clear/--send.", file=sys.stderr)
                return 2

            print(f"TX->RX: {payload}")
            await client.write_gatt_char(rx_char, payload.encode("utf-8"), response=True)

            try:
                resp = await asyncio.wait_for(notif_queue.get(), timeout=10.0)
                print(f"RX<-TX: {resp}")
            except asyncio.TimeoutError:
                print("No TX notification received (timeout).", file=sys.stderr)
                return 5
            finally:
                await client.stop_notify(tx_char)

    except BleakError as e:
        err = str(e)
        if "Peer removed pairing information" in err or "Code=14" in err:
            print(
                "macOS: eski BLE eşleşmesi (bond) ile cihaz uyuşmuyor (CoreBluetooth 14). "
                "Ayarlar → Bluetooth → cihazı (MER-…) kaldır/forget; gerekirse Mac Bluetooth’u kapat-aç veya"
                " kartı yanında tekrar dene.\n"
                f"Orijinal hata: {e}",
                file=sys.stderr,
            )
            return 14
        raise

    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))

