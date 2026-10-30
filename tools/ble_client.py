#!/usr/bin/env python3
"""BLE v1 client. Pair on the device first; macOS will request the displayed passkey."""

import asyncio, argparse, json, getpass, pathlib
from bleak import BleakScanner, BleakClient

RX = "98f10002-8f7c-4cb1-9e50-6bf589c91010"
TX = "98f10003-8f7c-4cb1-9e50-6bf589c91010"


async def run(args):
    device = await BleakScanner.find_device_by_filter(
        lambda d, a: d.name == "Waveshare-YA", timeout=15
    )
    if not device:
        raise RuntimeError("Enable BLE and pairing on the device")
    request = {"v": 1, "id": 1, "op": "status"}
    if args.wifi_file:
        conf = json.loads(args.wifi_file.read_text())
        request.update(op="wifi", ssid=conf["ssid"], password=conf["password"])
    if args.ssid:
        request.update(
            op="wifi", ssid=args.ssid, password=getpass.getpass("Wi-Fi password: ")
        )
    if args.key:
        request.update(op="set", key=args.key, value=args.value)
    async with BleakClient(device, pair=True) as client:
        response = bytearray()
        done = asyncio.Event()

        def notify(_, data):
            response.extend(data)
            if b"\n" in response:
                done.set()

        await client.start_notify(TX, notify)
        raw = (json.dumps(request) + "\n").encode()
        for i in range(0, len(raw), 20):
            await client.write_gatt_char(RX, raw[i : i + 20], response=True)
        try:
            await asyncio.wait_for(done.wait(), 8)
        except asyncio.TimeoutError:
            response = await client.read_gatt_char(TX)
        print(json.dumps(json.loads(response), ensure_ascii=False))


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--ssid")
    p.add_argument("--wifi-file", type=pathlib.Path)
    p.add_argument("--key", choices=["brightness", "volume", "mute", "fps", "sleep_battery", "sleep_usb"])
    p.add_argument("--value", type=int)
    asyncio.run(run(p.parse_args()))
