#!/usr/bin/env python3
"""Check a real trusted TLS response with BLE active; never installs a firmware.

The supplied test URL must serve a non-manifest response. The explicit restore
URL avoids silently replacing a user's custom update source.
"""
import argparse, json, pathlib, time
from device import Device

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--url", required=True)
    p.add_argument("--restore-url", required=True)
    p.add_argument("--out", type=pathlib.Path, required=True)
    a = p.parse_args()
    d = Device(a.port)
    def rpc(op, **fields):
        r = d.request({"op":op, **fields})
        assert r["ok"], (op, r)
        return r
    before = rpc("status")
    observations = []
    try:
        rpc("set", key="ble", value=1)
        time.sleep(.5)
        rpc("update_url", url=a.url)
        rpc("check")
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            time.sleep(.5)
            s = rpc("status")
            observations.append({k:s.get(k) for k in ("ota_phase", "internal_heap", "internal_min", "main_stack_free_min_bytes")})
            if s["ota_phase"] == "failed" and s["ota"] in (
                "更新清单格式、长度或响应类型无效", "更新服务器响应失败，请稍后重试",
                "服务器尚未发布固件，请稍后重试", "更新地址发生重定向，请使用直达地址"):
                assert s["uptime_ms"] > before["uptime_ms"]
                a.out.parent.mkdir(parents=True, exist_ok=True)
                a.out.write_text(json.dumps({"build_sha256":s["build_sha256"],
                                             "tls_http_response_received":True,
                                             "invalid_manifest_response_rejected":True,
                                             "note":s["ota"], "observations":observations}, ensure_ascii=False, indent=2) + "\n")
                print("Trusted TLS handshake and HTTP response succeeded with BLE enabled; invalid manifest rejected")
                return
        raise AssertionError("No verified HTTP response; TLS/network availability remains unverified")
    finally:
        rpc("update_url", url=a.restore_url)
        rpc("set", key="ble", value=int(before["ble_enabled"]))
        d.close()

if __name__ == "__main__": main()
