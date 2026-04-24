#!/usr/bin/env python3
"""
BLE GATT client for IOTMER Wi-Fi provisioning protocol v1 (see docs/sdk/esp-idf/ble-wifi-provisioning.md).

Requires a BLE-capable PC and: pip install -r requirements.txt
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import uuid
from typing import Optional, Union

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice
from bleak.exc import BleakCharacteristicNotFoundError, BleakError

# Must match components/iotmer_ble_wifi_prov/include/iotmer_ble_wifi_prov.h
UUID_CTRL = "1d14d6ee-0101-4000-8024-b5a3c0ffee01"
UUID_DATA = "1d14d6ee-0202-4000-8024-b5a3c0ffee01"
UUID_CLAIM = "1d14d6ee-0404-4000-8024-b5a3c0ffee01"
UUID_EVT = "1d14d6ee-0303-4000-8024-b5a3c0ffee01"
UUID_SVC = "1d14d6ee-0001-4000-8024-b5a3c0ffee01"

OP_PING = 0x01
OP_BEGIN_SSID = 0x02
OP_BEGIN_PASS = 0x03
OP_COMMIT = 0x04
OP_ABORT = 0x05

PROTO_VER = 1

EVT_STATE = 0
EVT_PROGRESS = 1
EVT_DONE = 2
EVT_ERROR = 3

EVT_NAMES = {EVT_STATE: "STATE", EVT_PROGRESS: "PROGRESS", EVT_DONE: "DONE", EVT_ERROR: "ERROR"}


def _canonicalize_periph_uuid(u: str) -> str:
    """RFC dizesine çevir (macOS CoreBluetooth bazen 128-bit UUID’yi ters oktet sırasıyla döndürür)."""
    raw = uuid.UUID(str(u)).bytes
    return str(uuid.UUID(bytes_le=raw[::-1])).lower()


def _dump_gatt(client: BleakClient) -> None:
    print("[debug] GATT tree (service handle → char handle → uuid):", file=sys.stderr)
    svcs = client.services.services
    if not svcs:
        nchr = len(client.services.characteristics)
        print(
            f"  (servis listesi boş; flat karakteristik sayısı={nchr}. "
            "CoreBluetooth keşfi boş dönmüş olabilir.)",
            file=sys.stderr,
        )
        return
    for _h, svc in svcs.items():
        c_svc = _canonicalize_periph_uuid(svc.uuid)
        print(f"  svc {svc.handle}: {svc.uuid}  (canonical: {c_svc})", file=sys.stderr)
        for c in svc.characteristics:
            cc = _canonicalize_periph_uuid(c.uuid)
            print(f"    chr {c.handle}: {c.uuid}  (canonical: {cc}) props={c.properties}", file=sys.stderr)


def _pick_iotmer_characteristics(
    client: BleakClient,
) -> tuple[
    BleakGATTCharacteristic,
    BleakGATTCharacteristic,
    BleakGATTCharacteristic,
    BleakGATTCharacteristic,
]:
    """Ctrl, Data, Claim, Events — kanonik UUID’ler (…-8024-…, iotmer_ble_wifi_prov.h ile aynı)."""
    by_canon: dict[str, BleakGATTCharacteristic] = {}
    for ch in client.services.characteristics.values():
        by_canon[_canonicalize_periph_uuid(ch.uuid)] = ch

    ctrl_s = UUID_CTRL.lower()
    data_s = UUID_DATA.lower()
    claim_s = UUID_CLAIM.lower()
    evt_s = UUID_EVT.lower()

    if evt_s not in by_canon:
        raise BleakError(
            "IOTMER Events karakteristiği bulunamadı (kanonik …-8024-… UUID). "
            "`--dump-gatt` ile canonical satırlara bakın; macOS’ta oktet sırası farklı görünebilir."
        )

    try:
        return by_canon[ctrl_s], by_canon[data_s], by_canon[claim_s], by_canon[evt_s]
    except KeyError as ex:
        raise BleakError(
            "IOTMER ctrl/data/claim/evt karakteristikleri eksik (UUID eşlemesi başarısız)."
        ) from ex


def _att_payload_max(mtu: int) -> int:
    """ATT Write Request / Notification user octets for negotiated ATT MTU."""
    if mtu < 23:
        mtu = 23
    return max(1, mtu - 3)


def _parse_evt(data: bytearray) -> Optional[tuple[int, int, bytes]]:
    if len(data) < 6:
        return None
    ver = data[0]
    if ver != PROTO_VER:
        return None
    code = data[1]
    err = int.from_bytes(data[2:4], "little")
    n = int.from_bytes(data[4:6], "little")
    if len(data) < 6 + n:
        return None
    payload = bytes(data[6 : 6 + n])
    return code, err, payload


async def find_device(name_prefix: str, timeout: float) -> Optional[BLEDevice]:
    """Pick first connectable advertisement whose local name matches the prefix."""

    def pred(d: BLEDevice, adv) -> bool:
        name = adv.local_name or d.name or ""
        return name.startswith(name_prefix)

    return await BleakScanner.find_device_by_filter(pred, timeout=timeout)


async def _write_claim(
    client: BleakClient, ch_claim: BleakGATTCharacteristic, claim_code: str
) -> None:
    body = json.dumps(
        {"type": "claim.set", "claim_code": claim_code},
        separators=(",", ":"),
        ensure_ascii=False,
    )
    raw = body.encode("utf-8")
    if len(raw) > 512:
        raise ValueError(f"claim JSON too long: {len(raw)} B (max 512)")
    print(f"[info] writing Claim JSON ({len(raw)} B)")
    await client.write_gatt_char(ch_claim, raw, response=True)


async def run_prov(
    target: Union[str, BLEDevice],
    ssid: str,
    password: str,
    ping_first: bool,
    result_timeout: float,
    *,
    claim: Optional[str] = None,
    claim_after_pass: bool = False,
    dump_gatt: bool = False,
) -> int:
    evt_q: asyncio.Queue[tuple[int, int, bytes]] = asyncio.Queue()

    def on_notify(_sender, data: bytearray) -> None:
        parsed = _parse_evt(data)
        if parsed is None:
            print(f"[evt] raw ({len(data)} B): {data.hex()}", file=sys.stderr)
            return
        code, err, payload = parsed
        label = EVT_NAMES.get(code, f"code={code}")
        extra = f" err={err}" if code == EVT_ERROR else ""
        if payload:
            try:
                ptxt = payload.decode("utf-8", errors="replace")
                print(f"[evt] {label}{extra} payload={ptxt!r}")
            except Exception:
                print(f"[evt] {label}{extra} payload_hex={payload.hex()}")
        else:
            print(f"[evt] {label}{extra}")

        if code in (EVT_DONE, EVT_ERROR):
            evt_q.put_nowait((code, err, payload))

    # Tam servis keşfi: macOS’ta `services=[uuid…]` ile hedefli keşif sık sık **boş** ağaç döndürüyor.
    async with BleakClient(target) as client:
        try:
            ch_ctrl, ch_data, ch_claim, ch_evt = _pick_iotmer_characteristics(client)
        except BleakError:
            if not dump_gatt:
                _dump_gatt(client)
            raise
        if dump_gatt:
            _dump_gatt(client)

        mtu = int(getattr(client, "mtu_size", 23) or 23)
        exch = getattr(client, "exchange_mtu", None)
        if callable(exch):
            try:
                mtu = int(await exch(247))
            except Exception as ex:
                print(f"[warn] MTU exchange failed ({ex}); using mtu_size={mtu}.", file=sys.stderr)
                mtu = int(getattr(client, "mtu_size", 23) or 23)

        chunk = _att_payload_max(mtu)
        print(f"[info] negotiated ATT MTU≈{mtu}, data chunk size={chunk} B")

        try:
            await client.start_notify(ch_evt, on_notify)
        except BleakCharacteristicNotFoundError:
            _dump_gatt(client)
            raise

        if ping_first:
            await client.write_gatt_char(ch_ctrl, bytes([OP_PING]), response=True)
            try:
                c, e, _ = await asyncio.wait_for(evt_q.get(), timeout=5.0)
                if c == EVT_ERROR:
                    print(f"[warn] PING returned ERROR err={e}", file=sys.stderr)
            except asyncio.TimeoutError:
                print("[warn] no event after PING (continuing)", file=sys.stderr)

        if claim and not claim_after_pass:
            try:
                await _write_claim(client, ch_claim, claim)
            except ValueError as ex:
                print(f"[fail] {ex}", file=sys.stderr)
                return 1

        ssid_b = ssid.encode("utf-8")
        pass_b = password.encode("utf-8")

        await client.write_gatt_char(ch_ctrl, bytes([OP_BEGIN_SSID]), response=True)
        for i in range(0, len(ssid_b), chunk):
            await client.write_gatt_char(ch_data, ssid_b[i : i + chunk], response=True)

        await client.write_gatt_char(ch_ctrl, bytes([OP_BEGIN_PASS]), response=True)
        for i in range(0, len(pass_b), chunk):
            await client.write_gatt_char(ch_data, pass_b[i : i + chunk], response=True)

        if claim and claim_after_pass:
            try:
                await _write_claim(client, ch_claim, claim)
            except ValueError as ex:
                print(f"[fail] {ex}", file=sys.stderr)
                return 1

        await client.write_gatt_char(ch_ctrl, bytes([OP_COMMIT]), response=True)

        while True:
            code, err, _ = await asyncio.wait_for(evt_q.get(), timeout=result_timeout)
            if code == EVT_DONE and err == 0:
                print("[ok] COMMIT succeeded (DONE).")
                return 0
            if code == EVT_ERROR:
                print(f"[fail] ERROR code={err}", file=sys.stderr)
                return 1


async def scan_only(name_prefix: str, timeout: float) -> int:
    print(f"Scanning {timeout:.1f}s for names starting with {name_prefix!r} or service {UUID_SVC}…")
    seen: dict[str, str] = {}

    def _adv_has_iotmer(uuids: list[str]) -> bool:
        want = UUID_SVC.lower()
        for u in uuids:
            try:
                if _canonicalize_periph_uuid(u) == want:
                    return True
            except ValueError:
                continue
        return False

    def cb(d: BLEDevice, adv) -> None:
        name = adv.local_name or d.name or ""
        uuids = adv.service_uuids or []
        uuids_l = {u.lower() for u in uuids}
        hit = (
            name.startswith(name_prefix)
            or UUID_SVC.lower() in uuids_l
            or _adv_has_iotmer(uuids)
        )
        if hit and d.address not in seen:
            seen[d.address] = name or "(no name)"
            ustr = ", ".join(uuids) if uuids else "-"
            print(f"  {d.address}  name={name!r}  RSSI={adv.rssi}  uuids={ustr}")

    async with BleakScanner(detection_callback=cb):
        await asyncio.sleep(timeout)

    if not seen:
        print("No matching devices found.")
        return 1
    return 0


def main() -> None:
    p = argparse.ArgumentParser(description="IOTMER BLE Wi-Fi provisioning test client (protocol v1).")
    p.add_argument("--address", help="BLE address (e.g. AA:BB:CC:DD:EE:FF). If omitted, scan by name prefix.")
    p.add_argument("--name-prefix", default="IOTMER-", help="GAP name prefix when scanning (default: IOTMER-).")
    p.add_argument("--scan-timeout", type=float, default=10.0, help="Seconds to scan when --address not set.")
    p.add_argument("--ssid", default="", help="Wi-Fi SSID (UTF-8).")
    p.add_argument("--password", default="", help="Wi-Fi password (UTF-8).")
    p.add_argument("--scan-only", action="store_true", help="Only list nearby provisioning devices and exit.")
    p.add_argument("--ping", action="store_true", help="Send PING before credential transfer.")
    p.add_argument("--result-timeout", type=float, default=30.0, help="Seconds to wait for DONE/ERROR after COMMIT.")
    p.add_argument(
        "--claim",
        default="",
        help='Optional one-time claim_code (e.g. pc_...). Sent as JSON to Claim GATT (…0404…). Do this *before* COMMIT (firmware may stop BLE after success).',
    )
    p.add_argument(
        "--claim-after-pass",
        action="store_true",
        help="Send --claim after password fragments, immediately before COMMIT (instead of at start).",
    )
    p.add_argument(
        "--dump-gatt",
        action="store_true",
        help="Bağlantıdan sonra keşfedilen GATT servis/karakteristik UUID’lerini stderr’e yaz.",
    )
    args = p.parse_args()

    if args.scan_only:
        rc = asyncio.run(scan_only(args.name_prefix, args.scan_timeout))
        raise SystemExit(rc)

    if not args.ssid or not args.password:
        p.error("--ssid and --password are required unless --scan-only.")

    claim: Optional[str] = args.claim.strip() or None

    async def _go() -> tuple[str, int]:
        if args.address:
            return args.address, await run_prov(
                args.address,
                args.ssid,
                args.password,
                args.ping,
                args.result_timeout,
                claim=claim,
                claim_after_pass=bool(args.claim_after_pass),
                dump_gatt=args.dump_gatt,
            )
        dev = await find_device(args.name_prefix, args.scan_timeout)
        if dev is None:
            print(
                f"No device found with name prefix {args.name_prefix!r} in {args.scan_timeout}s.",
                file=sys.stderr,
            )
            return "", 1
        print(f"[info] connecting to {dev.address} ({dev.name!r})…")
        return dev.address, await run_prov(
            dev,
            args.ssid,
            args.password,
            args.ping,
            args.result_timeout,
            claim=claim,
            claim_after_pass=bool(args.claim_after_pass),
            dump_gatt=args.dump_gatt,
        )

    _, code = asyncio.run(_go())
    raise SystemExit(code)


if __name__ == "__main__":
    main()
