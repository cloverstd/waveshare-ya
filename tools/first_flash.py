#!/usr/bin/env python3
"""First-install only: validates backup and signed image before changing partition layout."""

import argparse, pathlib, hashlib, subprocess, sys, json
from release import image_metadata


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True)
    p.add_argument("--backup", type=pathlib.Path, required=True)
    p.add_argument("--backup-sha256", required=True)
    p.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build"))
    p.add_argument(
        "--key", type=pathlib.Path, default=pathlib.Path("private/keys/ota-signing.pem")
    )
    a = p.parse_args()
    backup = a.backup.read_bytes()
    if (
        len(backup) != 0x1000000
        or hashlib.sha256(backup).hexdigest() != a.backup_sha256
    ):
        raise SystemExit("Invalid backup; refusing flash")
    image = a.build / "waveshare-ya.bin"
    version = image_metadata(image.read_bytes())
    if "test" in version:
        raise SystemExit("Do not first-install QA firmware")
    subprocess.run(
        [
            sys.executable,
            "-m",
            "espsecure",
            "verify_signature",
            "--version",
            "2",
            "--keyfile",
            str(a.key),
            str(image),
        ],
        check=True,
    )
    args = json.loads((a.build / "flasher_args.json").read_text())
    files = args["flash_files"]
    if "0x20000" not in files or "0x15000" not in files:
        raise SystemExit("Unexpected partition layout")
    empty = pathlib.Path("private/empty-nvs.bin")
    empty.write_bytes(b"\xff" * 0xC000)
    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        a.port,
        "--baud",
        "460800",
        "write_flash",
        "--flash_mode",
        "dio",
        "--flash_size",
        "16MB",
        "--flash_freq",
        "80m",
    ]
    for offset, path in sorted(files.items(), key=lambda kv: int(kv[0], 0)):
        command.extend([offset, str(a.build / path)])
    command.extend(["0x9000", str(empty)])
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
