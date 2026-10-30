#!/usr/bin/env python3
"""Exercise the real ten-minute OTA deadline with an authenticated slow sender."""

import pathlib, time, socket, select, urllib.parse, json
from ota_smoke import session, state

before = state()
base, token = session()
u = urllib.parse.urlsplit(base)
payload = pathlib.Path("build/waveshare-ya.bin").read_bytes()
s = socket.create_connection((u.hostname, 80), timeout=8)
s.settimeout(3)
header = f"POST /api/ota HTTP/1.1\r\nHost: {u.hostname}\r\nAuthorization: Bearer {token}\r\nContent-Length: {len(payload)}\r\nConnection: close\r\n\r\n".encode()
start = time.monotonic()
s.sendall(header + payload[:1024])
offset = 1024
response = bytearray()
closed = False
try:
    while time.monotonic() - start < 640:
        if select.select([s], [], [], 0)[0]:
            try:
                chunk = s.recv(4096)
            except ConnectionResetError:
                chunk = b""
            if not chunk:
                closed = True
                break
            response.extend(chunk)
            if b"400" in response:
                closed = True
                break
        try:
            s.sendall(payload[offset : offset + 16])
            offset += 16
        except (BrokenPipeError, ConnectionResetError):
            closed = True
            break
        elapsed = time.monotonic() - start
        if int(elapsed) % 60 == 0:
            print(f"Slow upload {elapsed:.0f}s; {offset} bytes sent", flush=True)
        time.sleep(1)
finally:
    s.close()
elapsed = time.monotonic() - start
after = state()
assert closed and 590 <= elapsed <= 640, (elapsed, bytes(response[:80]))
assert (
    after["build_sha256"] == before["build_sha256"]
    and after["slot"] == before["slot"]
    and after["uptime_ms"] > before["uptime_ms"]
)
assert "失败" in after["ota"], after
result = {
    "test": "slow-upload-total-deadline",
    "elapsed_seconds": elapsed,
    "bytes_sent": offset,
    "connection_closed": closed,
    "running_build_preserved": True,
    "after": after,
}
pathlib.Path("artifacts/slow-upload-result.json").write_text(
    json.dumps(result, ensure_ascii=False, indent=2)
)
print("Slow-upload deadline passed", elapsed, flush=True)
