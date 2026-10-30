#!/usr/bin/env python3
"""Install a signed application via the existing LAN session and verify boot.

--expect-rollback is only for an intentionally fault-injected test image.
No USB flash, eFuse writes, or credential output.
"""
import argparse
import json
import pathlib
import threading
import time
import urllib.parse
import requests
from device import Device


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--image", required=True, type=pathlib.Path)
    p.add_argument("--out", required=True, type=pathlib.Path)
    p.add_argument("--expect-rollback", action="store_true")
    a = p.parse_args()
    data = a.image.read_bytes()
    expected_hash = data[176:208].hex()
    expected_version = data[48:80].split(b"\0")[0].decode()
    if a.expect_rollback:
        assert "test" in expected_version, "Rollback test requires explicit test version"

    def rpc(body):
        d = Device(a.port)
        try:
            return d.request(body)
        finally:
            d.close()

    before = rpc({"op": "status"})
    assert not before["pending"] and before["battery_valid"]
    assert before["usb"] or before["battery"] >= 30
    assert rpc({"op": "ota_session"})["ok"]
    time.sleep(0.3)
    url = rpc({"op": "ota_url"})["url"]
    endpoint = urllib.parse.urlsplit(url)
    assert endpoint.scheme == "http" and endpoint.hostname and endpoint.fragment
    response = {}

    def upload():
        try:
            client = requests.Session()
            client.trust_env = False
            r = client.post(f"http://{endpoint.netloc}/api/ota", data=data,
                            headers={"Authorization": "Bearer " + endpoint.fragment}, timeout=(240, 240))
            response.update(status=r.status_code, body=r.text)
        except Exception as error:
            response["error"] = type(error).__name__

    thread = threading.Thread(target=upload)
    thread.start()
    samples = []
    while thread.is_alive():
        try:
            s = rpc({"op": "status"})
            samples.append({k: s.get(k) for k in ("version", "ota_phase", "ota_progress", "ota_received", "ota_total", "internal_heap")})
        except Exception:
            pass
        thread.join(0.3)
    assert response.get("status") == 200, response
    print("Signed upload accepted; waiting for boot verification", flush=True)
    deadline = time.monotonic() + 110
    after = None
    while time.monotonic() < deadline:
        try:
            s = rpc({"op": "status"})
            if a.expect_rollback:
                valid = (s["build_sha256"] == before["build_sha256"] and
                         s["uptime_ms"] < before["uptime_ms"] and "回退" in s["ota"])
            else:
                valid = (s["build_sha256"] == expected_hash and s["version"] == expected_version
                         and s["slot"] != before["slot"])
            if valid and not s["pending"] and s["uptime_ms"] > 35000:
                after = s
                break
        except Exception:
            pass
        time.sleep(2)
    assert after, "Expected firmware did not finish boot/rollback verification"
    safe = ("version", "build_sha256", "slot", "brightness", "sleep_battery", "sleep_usb", "rtc_valid", "time_valid", "pending", "uptime_ms", "internal_heap", "main_stack_free_min_bytes", "diag_stack_free_min_bytes")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps({"before": {k: before.get(k) for k in safe},
                                 "after": {k: after.get(k) for k in safe},
                                 "expected_rollback": a.expect_rollback,
                                 "upload_http_status": response["status"], "samples": samples},
                                ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"version": after["version"], "slot": after["slot"], "pending": after["pending"], "evidence": str(a.out)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
