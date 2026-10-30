#!/usr/bin/env python3
"""USB diagnostics. Never prints Wi-Fi credentials or OTA access tokens by default."""

import argparse, json, time, pathlib, serial, sys, fcntl, re


class NoResetSerial(serial.Serial):
    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass


class Device:
    def __init__(self, port):
        self.lock = (
            pathlib.Path(__file__).resolve().parents[1] / "private/serial.lock"
        ).open("a")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        self.serial = NoResetSerial(port=None, baudrate=115200, timeout=0.3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()

    def request(self, request, timeout=15):
        self.serial.reset_input_buffer()
        request = dict(request)
        request["seq"] = int(time.monotonic_ns() & 0x7FFFFFFF)
        self.serial.write((json.dumps(request, ensure_ascii=False) + "\n").encode())
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.serial.readline().decode(errors="replace").strip()
            marker = line.find("YA_JSON ")
            if marker >= 0:
                try:
                    result = json.JSONDecoder().raw_decode(line[marker + 8 :])[0]
                except (ValueError, json.JSONDecodeError):
                    continue
                if result.get("seq") == request["seq"] or (
                    "seq" not in result and result.get("ok") is False
                ):
                    return result
        raise TimeoutError("No USB diagnostics response")

    def screenshot(self, path):
        self.serial.reset_input_buffer()
        self.serial.write(b'{"op":"snapshot"}\n')
        end = time.monotonic() + 90
        data = bytearray()
        seen = set()
        geometry = None
        buffered = bytearray()
        finished = False
        while time.monotonic() < end and not finished:
            buffered.extend(self.serial.read(8192))
            while b"\n" in buffered:
                rawline, _, buffered = buffered.partition(b"\n")
                line = rawline.decode(errors="replace").strip()
                # Console log prefixes may precede a diagnostic marker on a line.
                # Keep strict length/offset validation for the pixel payload itself.
                marker = line.find("YA_")
                if marker >= 0:
                    line = line[marker:]
                if line.startswith("YA_IMAGE "):
                    width, height, stride = map(int, line.split()[1:4])
                    geometry = (width, height, stride)
                    data = bytearray(stride * height)
                elif line.startswith("YA_PIX ") and geometry:
                    match = re.search(r"YA_PIX (\d+) ([0-9a-f]+)", line)
                    if not match:
                        continue
                    offset = int(match.group(1))
                    hexdata = match.group(2)
                    expected = min(128, len(data) - offset)
                    if len(hexdata) != expected * 2:
                        continue
                    raw = bytes.fromhex(hexdata)
                    if offset >= len(data):
                        continue
                    data[offset : offset + len(raw)] = raw
                    seen.add(offset)
                elif line == "YA_IMAGE_END":
                    finished = True
                    break
        if not geometry or len(seen) != (len(data) + 127) // 128:
            raise RuntimeError("Incomplete screenshot")
        from PIL import Image

        w, h, stride = geometry
        rgb = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                offset = y * stride + x * 2
                v = data[offset] | data[offset + 1] << 8
                i = (y * w + x) * 3
                rgb[i : i + 3] = bytes(
                    (
                        (v >> 11) * 255 // 31,
                        ((v >> 5) & 63) * 255 // 63,
                        (v & 31) * 255 // 31,
                    )
                )
        Image.frombytes("RGB", (w, h), bytes(rgb)).save(path)

    def close(self):
        self.serial.close()
        fcntl.flock(self.lock, fcntl.LOCK_UN)
        self.lock.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    p.add_argument("--file", type=pathlib.Path)
    p.add_argument("--snapshot", type=pathlib.Path)
    p.add_argument("request", nargs="?", default='{"op":"status"}')
    args = p.parse_args()
    d = Device(args.port)
    try:
        if args.snapshot:
            d.screenshot(args.snapshot)
            print("Saved", args.snapshot)
        else:
            request = json.loads(args.file.read_text() if args.file else args.request)
            result = d.request(request)
            if request.get("op") == "ota_url":
                result = {
                    "ok": result.get("ok"),
                    "url": "[redacted; use Python API to consume]",
                }
            print(json.dumps(result, ensure_ascii=False))
    finally:
        d.close()


if __name__ == "__main__":
    main()
