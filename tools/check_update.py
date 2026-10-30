#!/usr/bin/env python3
"""Check deployed HTTP/1.1 framing and hashes without installing a firmware."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def download(url, folder, name):
    headers, body = folder / (name + '.headers'), folder / name
    subprocess.run(['curl', '--http1.1', '--silent', '--show-error', '--max-time', '120',
                    '-H', 'Accept-Encoding: identity', '-D', str(headers), '-o', str(body), url], check=True)
    blocks = headers.read_text().strip().replace('\r','').split('\n\n')
    lines = blocks[-1].splitlines()
    status = int(lines[0].split()[1])
    h = {k.lower():v.strip() for k,v in (line.split(':',1) for line in lines[1:] if ':' in line)}
    if status == 404 and name == 'manifest.json':
        print('Website reachable; no firmware published yet')
        return None
    assert status == 200, f'Unexpected HTTP status {status}'
    assert h.get('content-encoding','identity') == 'identity'
    assert 'transfer-encoding' not in h and 'location' not in h
    assert int(h['content-length']) == body.stat().st_size
    return body, h


def main():
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        result = download('https://ya.cloverstd.com/api/v1/ota/manifest.json',folder,'manifest.json')
        if result is None:
            return
        body, headers = result
        assert headers['content-type'].startswith('application/json') and body.stat().st_size < 4096
        m = json.loads(body.read_text())
        assert m['url'] == f"https://ya.cloverstd.com/firmware/waveshare-ya/{m['version']}/waveshare-ya-{m['version']}.bin"
        for name in ['cold.bin','warm.bin']:
            result = download(m['url'],folder,name)
            assert result is not None
            image, h = result
            assert h['content-type'] == 'application/octet-stream'
            assert image.stat().st_size == m['size'] and hashlib.sha256(image.read_bytes()).hexdigest() == m['sha256']
        print('Manifest and repeated firmware downloads match HTTP framing, size and SHA-256')


if __name__ == '__main__':
    main()
