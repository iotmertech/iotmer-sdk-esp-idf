import argparse
import asyncio
import json
import sys
import uuid

from bleak import BleakClient, BleakScanner


SVC_UUID = "1d14d6ee-1001-4000-8024-b5a3c0ffee01"
RX_UUID = "1d14d6ee-1002-4000-8024-b5a3c0ffee01"
TX_UUID = "1d14d6ee-1003-4000-8024-b5a3c0ffee01"


def _norm_uuid(u: str) -> str:
    # Some platforms may format UUIDs differently; compare by 128-bit value.
    return str(uuid.UUID(u)).lower()


def _pick_device(devices, name_prefix: str | None):
    if not devices:
        return None
    if name_prefix:
        for d in devices:
            if (d.name or "").startswith(name_prefix):
                return d
    return devices[0]


async def scan_once(timeout: float = 5.0):
    devices = await BleakScanner.discover(timeout=timeout)
    rows = []
    for d in devices:
        rows.append((d.name, d.address, d.rssi))
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
        _, rows = await scan_once(timeout=args.timeout)
        for name, addr, rssi in rows:
            print(f"{name or '-':32}  {addr:20}  rssi={rssi}")
        return 0

    if args.address is None:
        devices, _ = await scan_once(timeout=args.timeout)
        d = _pick_device(devices, args.name_prefix)
        if d is None:
            print("No BLE devices found.", file=sys.stderr)
            return 2
        address = d.address
        print(f"Selected: name={d.name!r} address={address}")
    else:
        address = args.address

    rx_uuid = _norm_uuid(RX_UUID)
    tx_uuid = _norm_uuid(TX_UUID)

    notif_queue: asyncio.Queue[str] = asyncio.Queue()

    def on_tx(_, data: bytearray):
        try:
            text = bytes(data).decode("utf-8", errors="replace")
        except Exception:
            text = repr(bytes(data))
        notif_queue.put_nowait(text)

    async with BleakClient(address) as client:
        if not client.is_connected:
            print("Connect failed.", file=sys.stderr)
            return 3

        svcs = await client.get_services()
        # Best-effort check that the service exists (compare by value).
        have_svc = any(_norm_uuid(str(s.uuid)) == _norm_uuid(SVC_UUID) for s in svcs)
        if not have_svc:
            print("Warning: iotmer_ble service UUID not found in GATT services (may be cached/hidden).")

        # Find concrete characteristic UUIDs as the OS reports them.
        rx_char = None
        tx_char = None
        for s in svcs:
            for c in s.characteristics:
                cu = _norm_uuid(str(c.uuid))
                if cu == rx_uuid:
                    rx_char = c.uuid
                elif cu == tx_uuid:
                    tx_char = c.uuid

        if rx_char is None or tx_char is None:
            print("RX/TX characteristics not found. If you changed UUIDs, remove device from OS Bluetooth cache.",
                  file=sys.stderr)
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

        # Wait for one response (or more if user keeps the script running).
        try:
            resp = await asyncio.wait_for(notif_queue.get(), timeout=10.0)
            print(f"RX<-TX: {resp}")
        except asyncio.TimeoutError:
            print("No TX notification received (timeout).", file=sys.stderr)
            return 5
        finally:
            await client.stop_notify(tx_char)

    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))

