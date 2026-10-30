#!/usr/bin/env python3
import os
import time, json, pathlib
from device import Device

D = Device(os.environ["YA_DEVICE_PORT"])
try:
    before = D.request({"op": "status"})
    for page in list(range(10)) * 3:
        assert D.request({"op": "page", "id": page})["ok"], page
        time.sleep(0.2)
    assert D.request({"op": "page", "id": 0})["ok"]
    time.sleep(0.4)
    after = D.request({"op": "status"})
    assert after["uptime_ms"] > before["uptime_ms"]
    assert before["heap"] - after["heap"] < 16384
    pathlib.Path("artifacts/ui-smoke-result.json").write_text(
        json.dumps(
            {"pages": 30, "before": before, "after": after},
            ensure_ascii=False,
            indent=2,
        )
    )
    print("UI lifecycle smoke passed", flush=True)
finally:
    D.close()
