#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export IDF_PATH="$(cd .tools/esp-idf && pwd -P)"
export IDF_TOOLS_PATH="$(cd .tools/espressif && pwd -P)"
export IDF_PYTHON_ENV_PATH="$(cd .tools/venv && pwd -P)"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
ya_tool_env="$(python "$IDF_PATH/tools/idf_tools.py" export --format key-value)"
eval "$ya_tool_env"
exec python "$IDF_PATH/tools/idf.py" "$@"
