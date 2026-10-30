#!/usr/bin/env python3
"""Verify version identity, hashes and the existing production signing trust."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from release import image_metadata, firmware_url

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--tag', required=True)
    p.add_argument('--provenance', action='store_true')
    a = p.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', version) or a.tag != 'v' + version or len(version) > 31:
        raise SystemExit('Tag must equal v + VERSION, using a stable semantic version')
    m = json.loads((a.out / 'manifest.json').read_text())
    image = a.out / f'waveshare-ya-{version}.bin'
    data = image.read_bytes()
    if not (image_metadata(data) == version == m['version'] and m['url'] == firmware_url(version)
            and m['size'] == len(data) and m['sha256'] == hashlib.sha256(data).hexdigest()):
        raise SystemExit('Release identity or digest mismatch')
    subprocess.run([sys.executable, '-m', 'espsecure', 'verify_signature', '--version', '2',
                    '--keyfile', str(ROOT / 'docs/ota-signing.pub'), str(image)], check=True, capture_output=True)
    if 'delta_index_url' in m:
        from delta_release import verify_delta
        verify_delta(a.out, m)
    if a.provenance:
        (a.out / 'release-info.json').write_text(json.dumps({
            'repository': 'cloverstd/waveshare-ya', 'tag': a.tag, 'version': version,
            'commit': os.environ['GITHUB_SHA'],
            'workflow_run': 'https://github.com/cloverstd/waveshare-ya/actions/runs/' + os.environ['GITHUB_RUN_ID'],
            'idf': '5.5.5', 'board': m['board'], 'layout': m['layout'],
            'size': m['size'], 'sha256': m['sha256'],
        }, indent=2) + '\n')
        files = sorted(f for f in a.out.iterdir() if f.is_file() and f.name != 'SHA256SUMS')
        (a.out / 'SHA256SUMS').write_text(''.join(
            hashlib.sha256(f.read_bytes()).hexdigest() + '  ' + f.name + '\n' for f in files))
    for line in (a.out / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if Path(name).name != name or hashlib.sha256((a.out / name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Release checksum mismatch')
    print('Release tag, image, manifest, checksums and production signature verified')


if __name__ == '__main__':
    main()
