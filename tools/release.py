#!/usr/bin/env python3
"""Package an already signed ESP-IDF application; never include a private key."""

import argparse, hashlib, json, pathlib, re, shutil, struct, subprocess, sys
from urllib.parse import urlsplit

PRODUCT = "waveshare-ya"
BOARD = "esp32-s3-touch-amoled-2.16"
LAYOUT = "ab6-v1"
UPDATE_BASE_URL = "https://ya.cloverstd.com"
MANIFEST_URL = f"{UPDATE_BASE_URL}/api/v1/ota/manifest.json"
DEFAULT_NOTES = "系统状态、时间同步、供电息屏与签名 OTA"


def image_metadata(data):
    if len(data) < 512 or len(data) > 0x600000 or data[0] != 0xE9:
        raise ValueError("Invalid application size/header")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("Missing app descriptor")

    def s(offset, n):
        return data[offset : offset + n].split(b"\0")[0].decode()

    version = s(48, 32)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+_-]{0,30}", version):
        raise ValueError("Invalid firmware version (1-31 ASCII version characters required)")
    product = s(80, 32)
    if (
        struct.unpack_from("<I", data, 288)[0] != 0x59414142
        or s(292, 40) != BOARD
        or s(332, 16) != LAYOUT
    ):
        raise ValueError("Wrong board or partition layout")
    if product != PRODUCT:
        raise ValueError("Wrong product")
    return version


def firmware_url(version):
    return f"{UPDATE_BASE_URL}/firmware/{PRODUCT}/{version}/{PRODUCT}-{version}.bin"


def validate_url(url):
    try:
        parts = urlsplit(url)
        # Accessing port also rejects malformed and out-of-range port numbers.
        _ = parts.port
        valid = (parts.scheme == "https" and parts.hostname and parts.username is None
                 and parts.password is None and not parts.fragment)
    except ValueError:
        valid = False
    if (not valid or not url.startswith("https://") or len(url.encode("utf-8")) > 383
            or any(ord(ch) <= 32 or ord(ch) >= 127 for ch in url) or "#" in url):
        raise ValueError("An HTTPS firmware URL is required (ASCII, <=383 bytes, no credentials/fragment)")


def package(image, out, url, key, allow_test=False, notes=DEFAULT_NOTES):
    data = image.read_bytes()
    version = image_metadata(data)
    if "test" in version and not allow_test:
        raise ValueError(
            "Test firmware requires --allow-test and must not be distributed"
        )
    url = url or firmware_url(version)
    validate_url(url)
    if not isinstance(notes, str) or len(notes.encode("utf-8")) > 255 or "\0" in notes:
        raise ValueError("Release notes must be a string of at most 255 UTF-8 bytes without NUL")
    subprocess.run(
        [
            sys.executable,
            "-m",
            "espsecure",
            "verify_signature",
            "--version",
            "2",
            "--keyfile",
            str(key),
            str(image),
        ],
        check=True,
        capture_output=True,
    )
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"{PRODUCT}-{version}.bin"
    shutil.copyfile(image, target)
    manifest = {
        "product": PRODUCT,
        "board": BOARD,
        "layout": LAYOUT,
        "version": version,
        "url": url,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "notes": notes,
    }
    (out / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument(
        "--image", type=pathlib.Path, default=pathlib.Path("build/waveshare-ya.bin")
    )
    p.add_argument(
        "--out", type=pathlib.Path, default=pathlib.Path("artifacts/release")
    )
    p.add_argument("--url", help="Override the default HTTPS firmware URL on ya.cloverstd.com")
    p.add_argument("--notes", default=DEFAULT_NOTES, help="Release notes, at most 255 UTF-8 bytes")
    p.add_argument("--allow-test", action="store_true")
    p.add_argument(
        "--key", type=pathlib.Path, default=pathlib.Path("private/keys/ota-signing.pem")
    )
    a = p.parse_args()
    m = package(a.image, a.out, a.url, a.key, a.allow_test, a.notes)
    print("Packaged", m["version"], m["size"], "bytes", m["sha256"])
    print("Manifest endpoint:", MANIFEST_URL)
    print("Firmware URL:", m["url"])
