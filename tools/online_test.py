#!/usr/bin/env python3
import os, sys, time, json, pathlib
from device import Device
from ota_smoke import state, wait_boot


def request(j):
    d = Device(os.environ["YA_DEVICE_PORT"])
    try:
        return d.request(j)
    finally:
        d.close()


def main():
    before = state()
    print("Before", before["version"], before["slot"], flush=True)
    assert request(
        {"op": "update_url", "url": os.environ["YA_TEST_MANIFEST_URL"]}
    )["ok"]
    assert request({"op": "check"})["ok"]
    end = time.monotonic() + 60
    while time.monotonic() < end:
        time.sleep(2)
        s = state()
        if "发现可用版本" in s["ota"]:
            break
        if "检查更新失败" in s["ota"] or "内存不足" in s["ota"]:
            raise RuntimeError("HTTPS update check failed")
    else:
        raise TimeoutError("HTTPS manifest fetch")
    print("Manifest fetched and TLS verified", flush=True)
    assert request({"op": "install"})["ok"]
    time.sleep(6)
    end = time.monotonic() + 150
    while time.monotonic() < end:
        try:
            s = state()
            if "回退" in s["ota"] and s["uptime_ms"] < 180000:
                assert s["version"] == before["version"] and s["slot"] == before["slot"]
                print("Recovered", json.dumps(s, ensure_ascii=False), flush=True)
                pathlib.Path("artifacts/https-abort-result.json").write_text(
                    json.dumps(
                        {
                            "https_download": True,
                            "startup_abort_rollback": True,
                            "state": s,
                        },
                        ensure_ascii=False,
                        indent=2,
                    )
                )
                return
        except (TimeoutError, OSError):
            pass
        time.sleep(2)
    raise TimeoutError("Expected rollback did not occur")


if __name__ == "__main__":
    main()
