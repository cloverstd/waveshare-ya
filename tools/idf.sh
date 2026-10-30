#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export IDF_PATH="$PWD/.tools/esp-idf"
export IDF_TOOLS_PATH="$PWD/.tools/espressif"
export IDF_PYTHON_ENV_PATH="$PWD/.tools/venv"
export PATH="$PWD/.tools/venv/bin:$PATH"
eval "$(python "$IDF_PATH/tools/idf_tools.py" export --format key-value)"
exec python "$IDF_PATH/tools/idf.py" "$@"
