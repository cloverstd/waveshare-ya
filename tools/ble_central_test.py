#!/usr/bin/env python3
import os
import sys, time, json, pathlib
from device import Device

D = Device(os.environ["YA_DEVICE_PORT"])
log = []
result = {}
try:
    peers = D.request({"op": "ble_peers"})["peers"]
    fixture = next(p for p in peers if p["name"] == "YA-QA-Sensor")
    D.request({"op": "ble_pair"})
    D.request({"op": "ble_connect", "index": fixture["index"]})
    end = time.monotonic() + 25
    sent = False
    while time.monotonic() < end:
        peers = D.request({"op": "ble_peers"})["peers"]
        current = next(p for p in peers if p["name"] == "YA-QA-Sensor")
        events = D.request({"op": "gatt_history"})["events"]
        if current["connected"]:
            candidates = [
                e
                for e in events
                if e["connection"] == current["handle"]
                and e["kind"] == 4
                and e["uuid"] == "0x2a6e"
            ]
            if candidates and not sent:
                attr = candidates[-1]
                desc = [
                    e
                    for e in events
                    if e["connection"] == current["handle"]
                    and e["kind"] == 5
                    and e["uuid"] == "0x2902"
                    and e["handle"] > attr["handle"]
                ]
                if desc:
                    result["read_queued"] = D.request(
                        {
                            "op": "ble_read",
                            "connection": current["handle"],
                            "handle": attr["handle"],
                        }
                    )["ok"]
                    result["subscribe_queued"] = D.request(
                        {
                            "op": "ble_subscribe",
                            "connection": current["handle"],
                            "handle": desc[-1]["handle"],
                        }
                    )["ok"]
                    sent = True
        values = [e for e in events if e["kind"] in [1, 6] and e["data"] == "ca08"]
        log.append({"peer": current, "events": events[-3:]})
        if any(e["kind"] == 1 for e in values) and any(e["kind"] == 6 for e in values):
            result.update(passed=True, temperature_c=22.5, values=values)
            break
        time.sleep(0.5)
    pathlib.Path("private/ble-central-trace.json").write_text(json.dumps(log, indent=2))
    pathlib.Path("artifacts/ble-central-result.json").write_text(
        json.dumps(result, indent=2)
    )
    print(result, flush=True)
finally:
    D.close()
