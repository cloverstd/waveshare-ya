#!/usr/bin/env python3
"""Repeatable software scroll benchmark, not a physical touch/panel test."""
import argparse, json, pathlib, time
from device import Device

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--out", required=True, type=pathlib.Path)
    a = p.parse_args()
    d = Device(a.port)
    def rpc(op, **kwargs):
        for attempt in range(3 if op == "status" else 1):
            try:
                r = d.request({"op":op, **kwargs})
                assert r["ok"], r
                return r
            except TimeoutError:
                if op != "status" or attempt == 2: raise
    before = rpc("status")
    results = []
    try:
        rpc("set", key="sleep_usb", value=0)
        for fps in (15, 30, 60):
            rpc("set", key="fps", value=fps)
            rpc("page", id=1)
            time.sleep(0.4)
            rpc("metrics_reset")
            for turn in range(16):
                rpc("scroll", y=1000 if turn % 2 == 0 else 0)
                time.sleep(0.25)
            s = rpc("status")
            keys = ("actual_fps", "refresh_count", "refresh_p50_us", "refresh_p95_us",
                    "refresh_work_p95_us", "flush_wait_p95_us", "flush_chunks",
                    "refresh_work_total_us", "flush_submit_total_us", "internal_heap",
                    "internal_min", "long_gap_count", "max_gap_us")
            result = {"target_fps":fps, **{k:s.get(k) for k in keys}}
            results.append(result)
            print(json.dumps(result), flush=True)
    finally:
        rpc("set", key="fps", value=before["fps"])
        rpc("set", key="sleep_usb", value=before["sleep_usb"])
        rpc("page", id=0)
        d.close()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps({"build_sha256":before["build_sha256"], "software_scroll":True,
                                 "results":results}, indent=2) + "\n")

if __name__ == "__main__": main()
