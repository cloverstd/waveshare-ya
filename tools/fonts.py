#!/usr/bin/env python3
"""Generate YA 2x fonts with pinned sources and lv_font_conv 1.5.3.

Use --fetch to download missing source fonts; normal regeneration is offline.
Only generated LVGL assets and licenses belong in the public source tree.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".tools/fonts"
SOURCES = {
    "NotoSansSC.ttf": (
        "https://raw.githubusercontent.com/google/fonts/2894aab31764f10f29c421bdfd2340d3b382d384/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf",
        "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da",
    ),
    "InterVariable.ttf": (
        "https://raw.githubusercontent.com/rsms/inter/v4.1/docs/font-files/InterVariable.ttf",
        "4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf",
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true")
    args = parser.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    for name, (url, digest) in SOURCES.items():
        path = CACHE / name
        if not path.exists() and args.fetch:
            data = urllib.request.urlopen(url, timeout=60).read()
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit(f"Source font checksum mismatch: {name}")
            path.write_bytes(data)
        if not path.exists():
            raise SystemExit(f"Missing {name}; run tools/bootstrap.sh --fonts")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise SystemExit(f"Source font checksum mismatch: {name}")

    converter = ROOT / ".tools/font-tools/node_modules/.bin/lv_font_conv"
    package = converter.resolve().parent / "package.json"
    if json.loads(package.read_text())["version"] != "1.5.3":
        raise SystemExit("lv_font_conv must be version 1.5.3")

    # Headers and the example app also contain UI text. Exclude generated C
    # assets so removed UI text can leave the subset on the next regeneration.
    sources = sorted(p for folder in (ROOT / "main", ROOT / "examples")
                     for p in folder.rglob("*") if p.suffix in (".cpp", ".hpp", ".h"))
    chars = set("".join(p.read_text() for p in sources))
    with TTFont(CACHE / "NotoSansSC.ttf") as font:
        cmap = font.getBestCmap()
    # Private-use LV_SYMBOL_* characters stay in the Montserrat fallback.
    symbols = "".join(sorted(c for c in chars if ord(c) > 127 and ord(c) in cmap
                             and not 0xE000 <= ord(c) <= 0xF8FF))
    for weight in (400, 500):
        with TTFont(CACHE / "NotoSansSC.ttf") as font:
            instantiateVariableFont(font, {"wght": weight}, inplace=True).save(CACHE / f"Noto-{weight}.ttf")
        with TTFont(CACHE / "InterVariable.ttf") as font:
            # Text optical size preserves open counters at small screen sizes.
            instantiateVariableFont(font, {"wght": weight, "opsz": 14}, inplace=True).save(CACHE / f"Inter-{weight}.ttf")

    specs = [("font_zh_caption", 10, 400, True), ("font_zh", 12, 400, True),
             ("font_zh_title", 14, 500, True),
             *[(f"font_inter_{size}", size // 2, 500, False) for size in (20, 24, 28, 40)]]
    report = {"scale": 2, "bpp": 4, "sources": {n: d for n, (_, d) in SOURCES.items()}, "fonts": []}
    for name, logical_size, weight, chinese in specs:
        size = logical_size * 2
        fallback = f"lv_font_montserrat_{20 if size == 20 else 40 if size == 40 else 28}"
        command = [str(converter.relative_to(ROOT)), "--font", f".tools/fonts/Inter-{weight}.ttf", "--range", "0x20-0x7e"]
        if chinese:
            command += ["--font", f".tools/fonts/Noto-{weight}.ttf", "--symbols", symbols]
        command += ["--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                    "--lv-include", "lvgl.h", "--lv-fallback", fallback, "-o", f"main/{name}.c"]
        subprocess.run(command, cwd=ROOT, check=True)
        output = ROOT / f"main/{name}.c"
        output.write_text("/* YA generated font; Noto: docs/FONT-LICENSE.txt; Inter: docs/INTER-LICENSE.txt. */\n" + output.read_text().rstrip() + "\n")
        report["fonts"].append({"name": name, "logical_size": logical_size, "pixel_size": size,
                                "weight": weight, "cjk": chinese, "fallback": fallback,
                                "sha256": hashlib.sha256(output.read_bytes()).hexdigest()})
        print(f"{name}: {size}px / {weight}, {'CJK subset + Inter' if chinese else 'Inter'}, 4bpp", flush=True)
    (CACHE / "generated.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
