#!/usr/bin/env python3
"""0.2.0 device regression. Uses the identified port; restores preferences.

Software scrolls measure rendering, not physical touch or panel ghosting.
Never powers down the device or changes its Wi-Fi credentials.
"""
import argparse
import json
import pathlib
import time
from device import Device


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("artifacts/regression-0.2.0.json"))
    args = parser.parse_args()
    device = Device(args.port)
    results = []
    before = device.request({"op": "status"})
    assert before["version"] == "0.2.0", before["version"]

    def rpc(op, **fields):
        response = device.request({"op": op, **fields})
        assert response["ok"], (op, fields, response)
        return response

    def eventually(predicate, timeout=12):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            state = rpc("status")
            if predicate(state):
                return state
            time.sleep(0.3)
        raise AssertionError("Timed out waiting for expected state")

    try:
        rpc("set", key="sleep_usb", value=0)
        rpc("page", id=5)
        time.sleep(0.3)
        for value in (25, 85, 60, 100):
            rpc("set", key="brightness", value=value)
            time.sleep(0.3)
            assert rpc("status")["brightness"] == value
            text = rpc("ui")["text"]
            assert f"{value}%" in text, (value, text)
        results.append({"test": "open-page-brightness-follows-external-settings", "passed": True})

        for key, valid, invalid in (
            ("sleep_battery", (30, 60, 300), (0, 600, 31)),
            ("sleep_usb", (60, 300, 600, 0), (30, 301)),
        ):
            for value in valid:
                rpc("set", key=key, value=value)
                assert rpc("status")[key] == value
            for value in invalid:
                assert not device.request({"op": "set", "key": key, "value": value})["ok"]
        legacy = device.request({"op": "set", "key": "sleep", "value": 60})
        assert not legacy["ok"] and legacy["error"] == "use_sleep_battery_or_sleep_usb"
        results.append({"test": "independent-sleep-ranges-and-legacy-error", "passed": True})

        # One OFF/ON cycle exercises end-state notifications without touching credentials.
        rpc("set", key="wifi", value=0)
        eventually(lambda s: not s["wifi_connected"])
        rpc("set", key="wifi", value=1)
        connected = eventually(lambda s: s["wifi_connected"], 40)
        assert connected["wifi_ssid"] and connected["wifi_rssi"] < 0
        rpc("page", id=3)
        time.sleep(0.4)
        text = rpc("ui")["text"]
        assert "正在连接网络" not in text
        results.append({"test": "wifi-reconnect-clears-connecting-notice", "passed": True})

        rpc("time_sync")
        clock = eventually(lambda s: s["time_valid"] and not s["time_syncing"], 40)
        results.append({"test": "network-time", "time_valid": clock["time_valid"],
                        "rtc_valid": clock["rtc_valid"], "last_time_sync": clock["last_time_sync"]})

        for page in [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12] * 3:
            rpc("page", id=page)
            time.sleep(0.1)
        results.append({"test": "page-lifecycle", "transitions": 33, "passed": True})

        # Repeatable software animation: results must not be called a touch test.
        for fps in (15, 30, 60):
            rpc("set", key="fps", value=fps)
            rpc("page", id=1)
            time.sleep(0.4)
            rpc("metrics_reset")
            samples = []
            for turn in range(16):
                rpc("scroll", y=1000 if turn % 2 == 0 else 0)
                time.sleep(0.25)
                samples.append(rpc("status"))
            keys = ("actual_fps", "refresh_p50_us", "refresh_p95_us", "refresh_work_p95_us",
                    "flush_wait_p95_us", "refresh_count", "refresh_work_total_us", "flush_submit_total_us", "frame_interval_samples", "long_gap_count", "max_gap_us", "flush_chunks", "internal_heap", "internal_min")
            results.append({"test": "software-scroll", "target_fps": fps,
                            "samples": [{k: s[k] for k in keys} for s in samples]})
    finally:
        for key in ("brightness", "fps", "sleep_battery", "sleep_usb"):
            device.request({"op": "set", "key": key, "value": before[key]})
        device.request({"op": "set", "key": "wifi", "value": int(before.get("wifi_enabled", True))})
        device.request({"op": "page", "id": 0})
        time.sleep(0.4)
        after = device.request({"op": "status"})
        device.close()
        args.out.parent.mkdir(parents=True, exist_ok=True)
        # Store bounded evidence without SSIDs, addresses, or access URLs.
        safe = ("version", "build_sha256", "slot", "uptime_ms", "internal_heap", "internal_min", "heap", "main_stack_free_min_bytes", "diag_stack_free_min_bytes")
        args.out.write_text(json.dumps({"completed": False, "before": {k: before.get(k) for k in safe},
                                       "after": {k: after.get(k) for k in safe},
                                       "results": results}, ensure_ascii=False, indent=2) + "\n")
    assert after["uptime_ms"] > before["uptime_ms"], "Unexpected restart"
    assert after["main_stack_free_min_bytes"] >= 2048, "Main stack margin too small"
    assert after["diag_stack_free_min_bytes"] >= 2048, "Diagnostics stack margin too small"
    evidence = json.loads(args.out.read_text())
    evidence["completed"] = True
    args.out.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
    print(f"Regression passed; physical button/panel tests remain separate: {args.out}")


if __name__ == "__main__":
    main()
