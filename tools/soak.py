#!/usr/bin/env python3
"""Bounded online/UI soak. Exits nonzero on reset, lost health, or sustained network loss."""

import os
import argparse, time, json, pathlib, datetime
from device import Device


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seconds", type=int, default=7200)
    p.add_argument("--port", default=os.environ["YA_DEVICE_PORT"])
    p.add_argument(
        "--output", type=pathlib.Path, default=pathlib.Path("artifacts/soak.jsonl")
    )
    a = p.parse_args()
    start = time.monotonic()
    last_uptime = -1
    baseline = None
    disconnected = 0
    iteration = 0
    a.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with a.output.open("w", buffering=1) as f:
            while True:
                d = Device(a.port)
                try:
                    s = d.request({"op": "status"}, timeout=15)
                finally:
                    d.close()
                elapsed = time.monotonic() - start
                assert s["ok"] and s["battery_valid"] and not s["pending"], s
                assert s["uptime_ms"] > last_uptime, "Unexpected reset"
                last_uptime = s["uptime_ms"]
                baseline = baseline or (s["version"], s["slot"])
                assert (
                    s["version"],
                    s["slot"],
                ) == baseline, "Version changed during soak"
                disconnected = 0 if s["ip"] else disconnected + 1
                assert disconnected < 3, "Network lost for 60 seconds"
                f.write(
                    json.dumps(
                        {
                            "elapsed": elapsed,
                            "utc": datetime.datetime.now(
                                datetime.timezone.utc
                            ).isoformat(),
                            "state": s,
                        },
                        ensure_ascii=False,
                    )
                    + "\n"
                )
                if iteration % 6 == 0:
                    print(
                        f'SOAK {elapsed:.0f}/{a.seconds}s heap={s["heap"]} internal={s.get("internal_heap")} uptime={s["uptime_ms"]}',
                        flush=True,
                    )
                if elapsed >= a.seconds:
                    break
                # Exercise application lifecycle without changing user settings or starting radio scans.
                page = [0, 1, 2, 5, 6, 7, 9][iteration % 7]
                d = Device(a.port)
                try:
                    assert d.request({"op": "page", "id": page})["ok"]
                finally:
                    d.close()
                iteration += 1
                time.sleep(min(20, max(0, a.seconds - (time.monotonic() - start))))
        d = Device(a.port)
        try:
            d.request({"op": "page", "id": 0})
        finally:
            d.close()
        print("SOAK PASSED", flush=True)
    finally:
        pass


if __name__ == "__main__":
    main()
