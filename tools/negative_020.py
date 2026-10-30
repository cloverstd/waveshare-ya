#!/usr/bin/env python3
"""Bounded LAN OTA rejection tests; never switches the current running slot."""
import argparse
import json
import pathlib
import socket
import time
import urllib.parse
import requests
from device import Device


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--image", required=True, type=pathlib.Path)
    p.add_argument("--out", type=pathlib.Path, default=pathlib.Path("artifacts/qa/0.2.0/negative.json"))
    a = p.parse_args()
    def rpc(body):
        d = Device(a.port)
        try: return d.request(body)
        finally: d.close()
    before = rpc({"op": "status"})
    assert before["version"] == "0.2.0" and not before["pending"]
    try:
        assert rpc({"op": "set", "key": "ble", "value": 1})["ok"]
        time.sleep(0.5)
        rpc({"op": "ble_scan"})
        assert rpc({"op": "ota_session"})["ok"]
        time.sleep(0.3)
        u = urllib.parse.urlsplit(rpc({"op": "ota_url"})["url"])
        base = "http://" + u.netloc
        auth = {"Authorization": "Bearer " + u.fragment}
        data = a.image.read_bytes()
        client = requests.Session()
        client.trust_env = False
        results = []
        cases = [("unauthorized", b"x" * 512, {}),
                 ("wrong-product", data[:80] + b"WRONG-PRODUCT" + data[92:], auth),
                 ("wrong-board", data[:292] + b"WRONG-BOARD" + data[303:], auth),
                 ("wrong-layout", data[:332] + b"ab3-v0" + data[338:], auth),
                 ("bad-signature", data[:4096] + bytes([data[4096] ^ 1]) + data[4097:], auth),
                 ("truncated-tail", data[:-4096], auth),
                 ("truncated-short", data[:65536], auth)]
        def unchanged():
            s = rpc({"op": "status"})
            assert s["slot"] == before["slot"] and s["build_sha256"] == before["build_sha256"]
            assert not s["pending"] and s["uptime_ms"] >= before["uptime_ms"]
            return s
        for name, body, headers in cases:
            r = client.post(base + "/api/ota", data=body, headers=headers, timeout=(240, 240))
            assert r.status_code >= 400, (name, r.status_code)
            s = unchanged()
            results.append({"test": name, "http_status": r.status_code, "slot_preserved": True,
                            "phase": s["ota_phase"], "progress": s["ota_progress"]})
            print(name, r.status_code, flush=True)
        for name, length, body in (("oversized", 6291457, b"x"),
                                   ("interrupted", len(data), data[:65536])):
            sock = socket.create_connection((u.hostname, u.port or 80), timeout=10)
            head = (f"POST /api/ota HTTP/1.1\r\nHost: {u.hostname}\r\n"
                    f"Authorization: Bearer {u.fragment}\r\nContent-Length: {length}\r\n"
                    "Connection: close\r\n\r\n").encode()
            sock.sendall(head + body)
            if name == "oversized": assert b"413" in sock.recv(1024)
            sock.close()
            time.sleep(2)
            unchanged()
            results.append({"test": name, "slot_preserved": True})
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(json.dumps({"build_sha256": before["build_sha256"], "results": results}, indent=2) + "\n")
        print("OTA rejection checks passed", flush=True)
    finally:
        deadline = time.monotonic() + 60
        while True:
            restored = rpc({"op": "set", "key": "ble", "value": int(before["ble_enabled"])})
            if restored["ok"]: break
            if time.monotonic() >= deadline: raise RuntimeError("BLE setting restore deferred by OTA; restore after transfer ends")
            time.sleep(1)



if __name__ == "__main__": main()
