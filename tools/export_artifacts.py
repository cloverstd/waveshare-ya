#!/usr/bin/env python3
"""Export explicit allowlists only. Never archive the workspace root wholesale."""

import pathlib, zipfile, hashlib, json, sys, shutil, argparse
from release import package, image_metadata
from public_files import public_files

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=pathlib.Path, default=root / "build-release")
parser.add_argument("--out", type=pathlib.Path, default=root / "artifacts/release/0.2.0")
parser.add_argument("--notes", default=None)
parser.add_argument("--key", type=pathlib.Path, default=root / "docs/ota-signing.pub")
args = parser.parse_args()
build = args.build.resolve()
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
version = image_metadata((build / "waveshare-ya.bin").read_bytes())
assert "test" not in version, "Do not export fault firmware as a release"
package(
    build / "waveshare-ya.bin",
    out,
    f"https://ya.cloverstd.com/firmware/waveshare-ya/{version}/waveshare-ya-{version}.bin",
    args.key,
    **({"notes": args.notes} if args.notes else {}),
)

with zipfile.ZipFile(
    out / f"waveshare-ya-{version}-first-install.zip", "w", zipfile.ZIP_DEFLATED
) as z:
    for rel in [
        "bootloader/bootloader.bin",
        "partition_table/partition-table.bin",
        "ota_data_initial.bin",
        "waveshare-ya.bin",
        "flasher_args.json",
    ]:
        z.write(build / rel, rel)
    z.writestr("empty-nvs.bin", b"\xff" * 0xC000)
    z.writestr(
        "READ-ME.txt",
        "First install only; back up the original 16MB Flash and read docs/BUILD.md first. This package initializes NVS and resets settings. Never upload the ZIP via OTA.\nOffsets: bootloader 0x0; partitions 0x8000; empty NVS 0x9000; otadata 0x15000; application 0x20000.\n",
    )
with zipfile.ZipFile(
    out / f"waveshare-ya-{version}-source.zip", "w", zipfile.ZIP_DEFLATED
) as z:
    for p in public_files(root):
        z.write(p, p.relative_to(root))
files = sorted(p for p in out.iterdir() if p.is_file() and p.name != "SHA256SUMS")
(out / "SHA256SUMS").write_text(
    "".join(
        hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.name + "\n" for p in files
    )
)
for p in out.glob("*.zip"):
    with zipfile.ZipFile(p) as z:
        assert all(
            not n.startswith(
                ("private/", ".tools/", "managed_components/", "artifacts/")
            )
            for n in z.namelist()
        )
        assert all(not n.endswith((".pem", ".key")) for n in z.namelist())
print("Exported", version, "artifacts without credentials or backups")
