#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version="${1:-$(cat VERSION)}"
# Existing SDKCONFIG files retain disabled defaults across upgrades.
.tools/venv/bin/python - <<'PYCONFIG'
from pathlib import Path
import re
p=Path('build-release/sdkconfig')
if p.exists():
    s=p.read_text()
    flags=('PM_ENABLE','FREERTOS_USE_TICKLESS_IDLE','ESP_WIFI_11KV_SUPPORT','ESP_WIFI_RRM_SUPPORT','ESP_WIFI_WNM_SUPPORT','ESP_WIFI_11R_SUPPORT')
    for name in flags:
        s=re.sub(r'^.*CONFIG_'+name+r'(?:=.*| is not set)\n','',s,flags=re.M)
        s+='\nCONFIG_'+name+'=y\n'
    s=re.sub(r'^.*CONFIG_LOG_(?:DEFAULT_LEVEL|MAXIMUM_)[^\n]*\n','',s,flags=re.M)
    s+='\nCONFIG_LOG_DEFAULT_LEVEL_WARN=y\nCONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y\n'
    p.write_text(s)
PYCONFIG
./tools/idf.sh -B build-release -D SDKCONFIG=build-release/sdkconfig -DPROJECT_VER="$version" -DYA_FAULT=0 -DYA_HTTPS_FIXTURE=OFF build
.tools/venv/bin/python - <<'PY'
from pathlib import Path
s=Path('build-release/sdkconfig').read_text()
for flag in ['CONFIG_YA_TEST_HTTPS_CA=y','CONFIG_SECURE_BOOT=y','CONFIG_SECURE_FLASH_ENC_ENABLED=y']:
 if flag in s:raise SystemExit('Unexpected release configuration: '+flag)
if 'CONFIG_YA_OTA_TEST_FAILURE=0' not in s:raise SystemExit('Fault injection must be disabled')
PY
.tools/venv/bin/python tools/check_lock.py build-release/dependencies.lock
