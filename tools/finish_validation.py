#!/usr/bin/env python3
"""Wait for the baseline soak, then install and validate the scoped patch."""

import pathlib, time, subprocess, sys, json, hashlib

progress = pathlib.Path("artifacts/soak-progress.log")
while True:
    text = progress.read_text() if progress.exists() else ""
    if "SOAK PASSED" in text:
        break
    if "Traceback" in text or "AssertionError" in text:
        raise RuntimeError("Baseline soak failed; do not install patch automatically")
    time.sleep(15)
samples = [
    json.loads(l) for l in pathlib.Path("artifacts/soak.jsonl").read_text().splitlines()
]
assert samples[-1]["elapsed"] >= 7200
print("BASELINE SOAK CONFIRMED", flush=True)
for command, log in [
    (
        [sys.executable, "tools/ota_smoke.py", "build-patch/waveshare-ya.bin"],
        "artifacts/patch-install.log",
    ),
    (
        [sys.executable, "tools/patch_regression.py"],
        "artifacts/patch-regression-run.log",
    ),
    ([sys.executable, "tools/ui_smoke.py"], "artifacts/patch-ui-smoke-run.log"),
    (
        [
            sys.executable,
            "tools/soak.py",
            "--seconds",
            "900",
            "--output",
            "artifacts/patch-soak.jsonl",
        ],
        "artifacts/patch-soak-progress.log",
    ),
]:
    with open(log, "w") as stream:
        subprocess.run(command, check=True, stdout=stream, stderr=subprocess.STDOUT)
    print("COMPLETED", command[1], flush=True)
print("VALIDATION COMPLETE", flush=True)
