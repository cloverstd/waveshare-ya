#!/usr/bin/env python3
"""Hardware OTA tests. Requires the USB diagnostics port and the device LAN address."""

import os
import sys, pathlib, json, time, socket, urllib.parse, requests
from device import Device

PORT = os.environ["YA_DEVICE_PORT"]


def session():
    d = Device(PORT)
    try:
        d.request({"op": "ota_session"})
        time.sleep(0.3)
        url = d.request({"op": "ota_url"})["url"]
    finally:
        d.close()
    u = urllib.parse.urlsplit(url)
    return "http://" + u.netloc, u.fragment


def state():
    d = Device(PORT)
    try:
        return d.request({"op": "status"}, timeout=12)
    finally:
        d.close()


def wait_boot(timeout=80, expected_hash=None):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            s = state()
            if (
                not s["pending"]
                and s["uptime_ms"] > 35000
                and (expected_hash is None or s.get("build_sha256") == expected_hash)
            ):
                return s
        except Exception:
            pass
        time.sleep(2)
    raise TimeoutError("Device did not complete boot verification")


def upload(path):
    base, token = session()
    client = requests.Session()
    client.trust_env = False
    with open(path, "rb") as f:
        return client.post(
            base + "/api/ota",
            headers={"Authorization": "Bearer " + token},
            data=f,
            timeout=(240, 240),
        )


def negative_tests(resume=False):
    base, token = session()
    client = requests.Session()
    client.trust_env = False
    original = pathlib.Path("build/waveshare-ya.bin").read_bytes()
    before = state()
    out = (
        json.loads(pathlib.Path("artifacts/ota-negative-results.json").read_text())
        if resume
        else []
    )
    for name, payload, headers in [
        ("unauthorized", b"x" * 512, {}),
        (
            "wrong-product",
            original[:80] + b"WRONG-PRODUCT" + original[93:],
            {"Authorization": "Bearer " + token},
        ),
        (
            "wrong-board",
            original[:292] + b"WRONG-BOARD" + original[303:],
            {"Authorization": "Bearer " + token},
        ),
        (
            "wrong-layout",
            original[:332] + b"ab3-v0" + original[338:],
            {"Authorization": "Bearer " + token},
        ),
        (
            "bad-signature",
            original[:4096] + bytes([original[4096] ^ 1]) + original[4097:],
            {"Authorization": "Bearer " + token},
        ),
        ("truncated-image", original[:65536], {"Authorization": "Bearer " + token}),
    ]:
        if any(x["test"] == name for x in out):
            continue
        response = client.post(
            base + "/api/ota", data=payload, headers=headers, timeout=(240, 240)
        )
        after = state()
        assert response.status_code >= 400, (name, response.status_code)
        assert (
            after["version"] == before["version"]
            and after["slot"] == before["slot"]
            and not after["pending"]
        )
        out.append(
            {
                "test": name,
                "http_status": response.status_code,
                "running_slot_preserved": True,
            }
        )
        print(out[-1], flush=True)
        pathlib.Path("artifacts/ota-negative-results.json").write_text(
            json.dumps(out, indent=2)
        )
    # Oversized length is rejected before receiving a body.
    u = urllib.parse.urlsplit(base)
    sock = socket.create_connection((u.hostname, 80), timeout=8)
    sock.sendall(
        f"POST /api/ota HTTP/1.1\r\nHost: {u.hostname}\r\nAuthorization: Bearer {token}\r\nContent-Length: 6291457\r\nConnection: close\r\n\r\nx".encode()
    )
    response = sock.recv(1024)
    sock.close()
    assert b"413" in response
    out.append({"test": "oversized-image", "http_status": 413})
    # Declare the real size, close while writing to exercise TCP interruption.
    u = urllib.parse.urlsplit(base)
    sock = socket.create_connection((u.hostname, 80), timeout=8)
    head = f"POST /api/ota HTTP/1.1\r\nHost: {u.hostname}\r\nAuthorization: Bearer {token}\r\nContent-Length: {len(original)}\r\nConnection: close\r\n\r\n".encode()
    sock.sendall(head + original[:65536])
    sock.close()
    time.sleep(3)
    after = state()
    assert after["slot"] == before["slot"] and not after["pending"]
    out.append({"test": "interrupted-upload", "running_slot_preserved": True})
    pathlib.Path("artifacts/ota-negative-results.json").write_text(
        json.dumps(out, indent=2)
    )


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--resume":
        negative_tests(True)
    elif len(sys.argv) > 1:
        r = upload(pathlib.Path(sys.argv[1]))
        r.encoding = "utf-8"
        print("HTTP", r.status_code, r.text)
        r.raise_for_status()
        time.sleep(3)
        print(
            json.dumps(
                wait_boot(100, pathlib.Path(sys.argv[1]).read_bytes()[176:208].hex()),
                ensure_ascii=False,
            )
        )
    else:
        negative_tests()
