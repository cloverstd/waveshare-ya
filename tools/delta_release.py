#!/usr/bin/env python3
"""Generate and byte-verify patches between original signed release images."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import detools
from release import image_metadata, UPDATE_BASE_URL, PRODUCT, BOARD, LAYOUT

FORMAT = 'detools-heatshrink-v1'
VERSION = re.compile(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\Z')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_image(image, key):
    version = image_metadata(image.read_bytes())
    if not VERSION.fullmatch(version):
        raise ValueError('Only stable signed releases can be delta bases/targets')
    subprocess.run([sys.executable, '-m', 'espsecure', 'verify_signature', '--version', '2',
                    '--keyfile', str(key), str(image)], check=True, capture_output=True)
    return version


def make_patch(base, target):
    patch = io.BytesIO()
    detools.create_patch(io.BytesIO(base), io.BytesIO(target), patch,
                         patch_type='sequential', compression='heatshrink')
    result = io.BytesIO()
    detools.apply_patch(io.BytesIO(base), io.BytesIO(patch.getvalue()), result)
    if result.getvalue() != target:
        raise ValueError('Delta round trip did not reproduce the signed target exactly')
    return patch.getvalue()


def package_delta(out, bases, key):
    manifest_path = out / 'manifest.json'
    m = json.loads(manifest_path.read_text())
    target_path = out / f'{PRODUCT}-{m["version"]}.bin'
    version = verify_image(target_path, key)
    target = target_path.read_bytes()
    if version != m['version'] or len(target) != m['size'] or digest(target) != m['sha256']:
        raise ValueError('Target manifest does not match signed image')
    patches = []
    generated = {}
    seen = set()
    if len(bases) > 3:
        raise ValueError('At most three delta bases are supported')
    for base_path in bases:
        old_version = verify_image(base_path, key)
        if old_version in seen or tuple(map(int, old_version.split('.'))) >= tuple(map(int, version.split('.'))):
            raise ValueError('Duplicate or non-older base')
        seen.add(old_version)
        base = base_path.read_bytes()
        data = make_patch(base, target)
        if len(data) >= len(target):
            continue
        name = f'{PRODUCT}-{old_version}-to-{version}.patch'
        generated[name] = data
        patches.append(dict(format=FORMAT, base_version=old_version, base_size=len(base),
                            base_sha256=digest(base), url=f'{UPDATE_BASE_URL}/firmware/{PRODUCT}/{version}/{name}',
                            size=len(data), sha256=digest(data)))
    index = dict(schema=1, product=PRODUCT, board=BOARD, layout=LAYOUT, version=version,
                 size=len(target), sha256=digest(target), patches=patches)
    raw = (json.dumps(index, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
    if len(raw) >= 4096:
        raise ValueError('Delta index exceeds device bound')
    # Finish all validation before replacing previously generated metadata/files.
    for name, data in generated.items():
        (out / name).write_bytes(data)
    for previous in out.glob(f'{PRODUCT}-*-to-{version}.patch'):
        if previous.name not in generated: previous.unlink()
    (out / 'delta-index.json').write_bytes(raw)
    m['delta_index_url'] = f'{UPDATE_BASE_URL}/firmware/{PRODUCT}/{version}/delta-index.json'
    m['delta_index_sha256'] = digest(raw)
    manifest_path.write_text(json.dumps(m, ensure_ascii=False, indent=2) + '\n')
    files=sorted(f for f in out.iterdir() if f.is_file() and f.name!='SHA256SUMS')
    (out/'SHA256SUMS').write_text(''.join(digest(f.read_bytes())+'  '+f.name+'\n' for f in files))
    return index


def recent_bases(version, releases):
    current = tuple(map(int, version.split('.')))
    eligible = [r for r in releases if not r['isDraft'] and not r['isPrerelease']
                and r['tagName'].startswith('v') and VERSION.fullmatch(r['tagName'][1:])
                and tuple(map(int, r['tagName'][1:].split('.'))) < current]
    return sorted(eligible, key=lambda r: tuple(map(int, r['tagName'][1:].split('.'))), reverse=True)[:3]


def download_bases(version, directory):
    # gh paginates instead of silently restricting eligibility to its default list size.
    releases = json.loads(subprocess.check_output([
        'gh', 'api', '--paginate', '--slurp', 'repos/cloverstd/waveshare-ya/releases?per_page=100']))
    records = [dict(isDraft=r['draft'], isPrerelease=r['prerelease'], tagName=r['tag_name'])
               for page in releases for r in page]
    bases = []
    for r in recent_bases(version, records):
        old = r['tagName'][1:]
        dest = directory / old
        dest.mkdir()
        subprocess.run(['gh', 'release', 'download', r['tagName'], '--repo', 'cloverstd/waveshare-ya',
                        '--dir', str(dest), '--pattern', f'{PRODUCT}-{old}.bin',
                        '--pattern', 'manifest.json'], check=True)
        path = dest / f'{PRODUCT}-{old}.bin'
        m = json.loads((dest / 'manifest.json').read_text())
        data = path.read_bytes()
        if (m['version'] != old or m['size'] != len(data) or m['sha256'] != digest(data)
                or image_metadata(data) != old):
            raise ValueError('Historical release identity/digest mismatch')
        bases.append(path)
    return bases


def verify_delta(out, m):
    expected_url=f'{UPDATE_BASE_URL}/firmware/{PRODUCT}/{m["version"]}/delta-index.json'
    if m['delta_index_url'] != expected_url: raise ValueError('Invalid delta index URL')
    raw = (out / 'delta-index.json').read_bytes()
    if digest(raw) != m['delta_index_sha256']:
        raise ValueError('Delta index digest mismatch')
    index = json.loads(raw)
    for k in ('product', 'board', 'layout', 'version', 'size', 'sha256'):
        if index[k] != m[k]: raise ValueError('Delta target mismatch')
    if index['schema'] != 1 or len(index['patches']) > 3 or len(raw) >= 4096:
        raise ValueError('Invalid delta index')
    for p in index['patches']:
        name = f'{PRODUCT}-{p["base_version"]}-to-{m["version"]}.patch'
        expected = f'{UPDATE_BASE_URL}/firmware/{PRODUCT}/{m["version"]}/{name}'
        if (not VERSION.fullmatch(p['base_version']) or p['url'] != expected or p['format'] != FORMAT
                or not re.fullmatch('[a-f0-9]{64}', p['base_sha256'])):
            raise ValueError('Invalid delta entry')
        data = (out / name).read_bytes()
        if len(data) != p['size'] or not 0 < len(data) < m['size'] or digest(data) != p['sha256']:
            raise ValueError('Patch digest/size mismatch')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('out', type=Path)
    parser.add_argument('--base', action='append', type=Path, default=[])
    parser.add_argument('--github-bases', action='store_true')
    parser.add_argument('--key', type=Path, default=Path('docs/ota-signing.pub'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        version = json.loads((args.out / 'manifest.json').read_text())['version']
        bases = download_bases(version, Path(tmp)) if args.github_bases else args.base
        index = package_delta(args.out, bases, args.key)
        print('Verified delta packages:', len(index['patches']))
