#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .tools private/keys private/backups artifacts
chmod 700 private private/keys private/backups
if [ "${1:-}" = '--fonts' ]; then
  if [ ! -x .tools/venv/bin/python ]; then python3 -m venv .tools/venv; fi
  .tools/venv/bin/pip install fonttools==4.68.0
  npm install --prefix .tools/font-tools lv_font_conv@1.5.3
  .tools/venv/bin/python tools/fonts.py --fetch
  exit 0
fi
if [ ! -d .tools/esp-idf ]; then
  git clone --branch v5.5.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git .tools/esp-idf
fi
if [ "$(git -C .tools/esp-idf rev-parse HEAD)" != b774170ff46c393eeb5e495ea37936038d3f4f4f ]; then
  echo 'Unexpected ESP-IDF revision; refusing to replace an existing SDK' >&2
  exit 1
fi
if [ ! -x .tools/venv/bin/python ]; then python3 -m venv .tools/venv; fi
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
export IDF_PYTHON_ENV_PATH="$PWD/.tools/venv"
.tools/venv/bin/python .tools/esp-idf/tools/idf_tools.py install --targets esp32s3
.tools/venv/bin/python .tools/esp-idf/tools/idf_tools.py install-python-env
.tools/venv/bin/pip install -r tools/ci_requirements.txt
if [ ! -f private/keys/ota-signing.pem ]; then
  if [ "${YA_REQUIRE_SIGNING_KEY:-0}" = 1 ]; then
    echo 'Release signing key is required; refusing to generate a replacement' >&2
    exit 1
  fi
  .tools/venv/bin/python -m espsecure generate_signing_key --version 2 private/keys/ota-signing.pem
fi
chmod 600 private/keys/ota-signing.pem
printf 'Ready: ./tools/idf.sh build\n'
