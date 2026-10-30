#!/usr/bin/env python3
"""Regenerate UI font using Noto Sans SC (OFL), lv_font_conv 1.5.3."""

from pathlib import Path
import subprocess

chars = set("".join(p.read_text() for p in Path("main").glob("*.cpp")))
symbols = "".join(sorted(c for c in chars if ord(c) > 127))
subprocess.run(
    [
        ".tools/font-tools/node_modules/.bin/lv_font_conv",
        "--font",
        ".tools/fonts/NotoSansSC-Medium.ttf",
        "--size",
        "22",
        "--bpp",
        "4",
        "--format",
        "lvgl",
        "--range",
        "0x20-0x7e",
        "--symbols",
        symbols,
        "--no-compress",
        "--lv-include",
        "lvgl.h",
        "-o",
        "main/font_zh.c",
    ],
    check=True,
)
output = Path("main/font_zh.c")
output.write_text(
    output.read_text().replace(
        ".fallback = NULL,", ".fallback = &lv_font_montserrat_20,"
    )
)
