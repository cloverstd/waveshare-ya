#!/usr/bin/env python3
"""Targeted 0.1.1 regression; no audio playback and original preferences restored."""

import os
import sys, time, json, pathlib
from device import Device


def rpc(body):
    d = Device(os.environ["YA_DEVICE_PORT"])
    try:
        return d.request(body)
    finally:
        d.close()


def wait_for(predicate, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        s = rpc({"op": "status"})
        if predicate(s):
            return s
        time.sleep(0.5)
    raise AssertionError(s)


before = rpc({"op": "status"})
assert before["version"] == "0.1.1"
results = []
try:
    nested = 0
    for _ in range(80):
        nested = {"x": nested}
    response = rpc({"op": "set", "key": "volume", "value": nested})
    assert not response["ok"]
    assert rpc({"op": "status"})["uptime_ms"] > before["uptime_ms"]
    results.append(
        {"test": "nested-control-json-rejected-without-reset", "passed": True}
    )
    for mute, volume, expected in [(1, 100, 1), (0, 0, 1), (0, 30, 0)]:
        assert rpc({"op": "set", "key": "volume", "value": volume})["ok"]
        assert rpc({"op": "set", "key": "mute", "value": mute})["ok"]
        s = wait_for(
            lambda s: s["audio_mute_applied"] == expected and s["mute"] == bool(mute)
        )
        results.append(
            {
                "test": "hardware-mute-state",
                "mute": mute,
                "volume": volume,
                "applied": expected,
            }
        )
    assert rpc({"op": "set", "key": "wifi", "value": 0})["ok"]
    time.sleep(2)
    assert not rpc({"op": "status"})["ip"]
    time.sleep(2)
    assert not rpc({"op": "status"})["ip"]
    assert rpc({"op": "set", "key": "wifi", "value": 1})["ok"]
    wait_for(lambda s: bool(s["ip"]), 30)
    results.append({"test": "wifi-off-and-reconnect", "passed": True})
    # Exercise cancel during a scan/connection when the named local fixture is present.
    assert rpc({"op": "set", "key": "ble", "value": 1})["ok"]
    time.sleep(0.5)
    assert rpc({"op": "ble_scan"})["ok"]
    time.sleep(11)
    peers = rpc({"op": "ble_peers"})["peers"]
    fixture = [p for p in peers if p["name"] == "YA-QA-Sensor"]
    if fixture:
        rpc({"op": "ble_connect", "index": fixture[0]["index"]})
        time.sleep(0.02)
    assert rpc({"op": "set", "key": "ble", "value": 0})["ok"]
    time.sleep(3)
    assert not rpc({"op": "status"})["ble_enabled"]
    assert not any(p["connected"] for p in rpc({"op": "ble_peers"})["peers"])
    results.append(
        {
            "test": "ble-off-cancels-or-disconnects",
            "fixture_available": bool(fixture),
            "passed": True,
        }
    )
finally:
    for key in ["volume", "mute", "ble_enabled"]:
        setting_key = "ble" if key == "ble_enabled" else key
        rpc({"op": "set", "key": setting_key, "value": int(before[key])})
    rpc({"op": "set", "key": "wifi", "value": 1})
after = wait_for(
    lambda s: bool(s["ip"])
    and s["audio_mute_applied"] == int(before["mute"] or before["volume"] == 0),
    30,
)
assert after["uptime_ms"] > before["uptime_ms"]
pathlib.Path("artifacts/patch-regression.json").write_text(
    json.dumps(
        {"before": before, "after": after, "results": results},
        ensure_ascii=False,
        indent=2,
    )
)
print("Patch regression passed", flush=True)
