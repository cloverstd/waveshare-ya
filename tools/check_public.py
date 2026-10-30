#!/usr/bin/env python3
"""Scan publishable files without printing matching secret values."""
import argparse
import json
import re
import subprocess
import zipfile
from pathlib import Path
from public_files import ROOT, public_files

PATTERNS = {
    'private key': rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----[\r\n]+[A-Za-z0-9+/=\r\n]{64,}-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'access token': rb'(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[A-Z0-9]{16})',
    'personal path': rb'/Users/[A-Za-z0-9_.-]+/',
    'device port': rb'/dev/(?:cu|tty)\.usbmodem[0-9A-Za-z]+',
    'private IPv4': rb'\b(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3})\b',
    # Locally administered dummy test addresses (02:...) are intentionally allowed.
    'hardware MAC': rb'(?i)\b(?:[0-9a-f][048c])(?::[0-9a-f]{2}){5}\b',
}


def private_values():
    values = set()
    def walk(x):
        if isinstance(x, dict):
            for k, v in x.items():
                if k.lower() in {'ssid', 'password', 'passwd', 'psk', 'wifi_password'} and isinstance(v, str) and len(v) >= 4:
                    values.add(v.encode())
                else:
                    walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)
    for p in (ROOT / 'private').glob('*.json'):
        if p.stat().st_size > 262144:
            continue
        try:
            walk(json.loads(p.read_text()))
        except (ValueError, UnicodeError):
            pass
    return values


def check(name, data, known):
    errors = []
    for label, pattern in PATTERNS.items():
        if re.search(pattern, data):
            errors.append(f'{name}: {label}')
    if any(value in data for value in known):
        errors.append(f'{name}: known private network value')
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tracked', action='store_true')
    p.add_argument('--artifacts', type=Path)
    a = p.parse_args()
    known = private_values()
    files = list(public_files())
    errors = []
    if a.tracked:
        tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
        allowed = {str(f.relative_to(ROOT)) for f in files}
        errors.extend(f'{n}: outside source allowlist' for n in tracked if n and n not in allowed)
    for f in files:
        errors.extend(check(str(f.relative_to(ROOT)), f.read_bytes(), known))
    if a.artifacts:
        for f in a.artifacts.iterdir():
            if f.suffix == '.zip':
                with zipfile.ZipFile(f) as z:
                    for n in z.namelist():
                        parts = Path(n).parts
                        if any(x in {'private', '.tools', 'node_modules', '.git'} for x in parts) or n.endswith(('.pem', '.key')):
                            errors.append(f'{f.name}/{n}: prohibited archive entry')
                        errors.extend(check(f'{f.name}/{n}', z.read(n), known))
            elif f.is_file():
                errors.extend(check(f.name, f.read_bytes(), known))
    if errors:
        raise SystemExit('\n'.join(errors))
    print('Public source and requested artifacts contain no detected private values')


if __name__ == '__main__':
    main()
