#!/usr/bin/env python3
"""Verify RTC across an offline software reboot; not a physical power-off test."""
import argparse
import json
import pathlib
import time
from device import Device


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--out", type=pathlib.Path, default=pathlib.Path("artifacts/qa/0.2.0/rtc-reboot.json"))
    a = p.parse_args()
    def rpc(body):
        d = Device(a.port)
        try: return d.request(body)
        finally: d.close()
    before = rpc({"op": "status"})
    assert before["rtc_valid"] and before["time_valid"] and not before["pending"]
    start = time.monotonic()
    after = None
    try:
        assert rpc({"op": "set", "key": "wifi", "value": 0})["ok"]
        time.sleep(1.5)
        assert not rpc({"op": "status"})["wifi_connected"]
        try: rpc({"op": "reboot"})
        except (OSError, TimeoutError): pass  # Reboot may precede the USB reply.
        end = time.monotonic() + 45
        while time.monotonic() < end:
            try:
                s = rpc({"op": "status"})
                if s["uptime_ms"] < before["uptime_ms"] and s["uptime_ms"] > 3000:
                    after = s
                    break
            except (OSError, TimeoutError): pass
            time.sleep(1)
        assert after is not None
        assert after["time_valid"] and after["rtc_valid"] and not after["wifi_connected"]
        error = after["epoch"] - before["epoch"] - (time.monotonic() - start)
        assert abs(error) < 4, error
        assert "RTC" in after["time_note"], after["time_note"]
        a.out.parent.mkdir(parents=True, exist_ok=True)
        a.out.write_text(json.dumps({"software_reboot_only": True, "rtc_restored": True,
                                     "network_disabled_at_boot": True, "clock_error_seconds": error,
                                     "build_sha256": after["build_sha256"]}, indent=2) + "\n")
        print("RTC restored valid time with Wi-Fi disabled across software reboot", flush=True)
    finally:
        rpc({"op": "set", "key": "wifi", "value": int(before["wifi_enabled"])})


if __name__ == "__main__": main()
