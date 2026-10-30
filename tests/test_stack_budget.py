"""Guard the nested main -> OTA boot-validation stack regression using GCC evidence."""
import pathlib
import os
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("source,symbol,budget", [
    ("companion.cpp", "cJSON* ya::companion_request(int, const cJSON*)", 2048),
    ("data.cpp", "void ya::{anonymous}::worker(void*)", 1024),
    ("main.cpp", "void app_main()", 512),
    ("board.cpp", "void ya::key_task(void*)", 512),
    ("bluetooth.cpp", "void ya::ble_stack_init()", 512),
    ("ota.cpp", "void ya::ota_boot_check()", 1024),
    ("ota.cpp", "void ya::check_task(void*)", 8192),
    ("ota.cpp", "void ya::install_task(void*)", 8192),
    ("diagnostics.cpp", "void ya::task(void*)", 2048),
    ("ui.cpp", "void ya::refresh(lv_timer_t*)", 4096),
    ("ui.cpp", "void ya::wifi_detail(uint32_t)", 1024),
    ("logs.cpp", "int ya::{anonymous}::capture(const char*, va_list)", 512),
])
def test_main_call_chain_static_stack(source, symbol, budget):
    usage = ROOT / os.environ.get("YA_BUILD_DIR", "build-release") / "esp-idf/main/CMakeFiles/__idf_main.dir" / (source + ".su")
    if not usage.exists() or usage.stat().st_mtime < (ROOT / "main" / source).stat().st_mtime:
        pytest.skip("Build the current firmware before checking compiler stack budgets")
    record = next(line for line in usage.read_text().splitlines() if symbol in line)
    assert int(record.split("\t")[1]) <= budget, record
