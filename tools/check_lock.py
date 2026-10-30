#!/usr/bin/env python3
"""Reject dependency drift, allowing only the local BSP's checkout path to vary."""
import pathlib
import sys
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]


def normalized(path):
    lock = yaml.safe_load(path.read_text())
    source = lock['dependencies']['waveshare/esp32_s3_touch_amoled_2_16']['source']
    resolved = pathlib.Path(source['path'])
    if not resolved.is_absolute():
        resolved = ROOT / resolved
    if resolved.resolve() != ROOT / 'components/waveshare__esp32_s3_touch_amoled_2_16':
        raise ValueError('Unexpected local BSP path')
    source['path'] = 'components/waveshare__esp32_s3_touch_amoled_2_16'
    return lock


if __name__ == '__main__':
    if normalized(ROOT / 'dependencies.lock') != normalized(pathlib.Path(sys.argv[1])):
        raise SystemExit('Dependency lock changed; release refused')
    print('Pinned dependency versions and hashes match')
