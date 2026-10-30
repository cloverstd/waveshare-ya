"""Guard the nested main -> OTA boot-validation stack regression using GCC evidence."""
import pathlib
import os
import re
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("source,symbol,budget", [
    ("gallery.cpp", "void ya::gallery::{anonymous}::worker(void*)", 1024),
    ("gallery.cpp", "bool ya::gallery::{anonymous}::fetch(", 3072),
    ("gallery_codec.cpp", "ya::gallery::decode(", 1024),
    ("companion.cpp", "cJSON* ya::companion_request(int, const cJSON*)", 2048),
    ("data.cpp", "void ya::{anonymous}::worker(void*)", 1024),
    ("main.cpp", "void app_main()", 512),
    ("board.cpp", "void ya::key_task(void*)", 512),
    ("bluetooth.cpp", "void ya::ble_stack_init()", 512),
    ("bluetooth.cpp", "void ya::rpc(uint16_t, const char*)", 1536),
    ("ota.cpp", "void ya::ota_boot_check()", 1024),
    ("ota.cpp", "void ya::check_task(void*)", 8192),
    ("ota.cpp", "void ya::install_task(void*)", 8192),
    ("ota.cpp", "esp_err_t ya::upload(httpd_req_t*)", 512),
    ("ota.cpp", "esp_err_t ya::info(httpd_req_t*)", 512),
    ("ota.cpp", "esp_err_t ya::error(", 1536),
    ("ota.cpp", "esp_err_t ya::receive_upload(", 6144),
    ("ota.cpp", "void ya::worker(void*)", 1024),
    ("diagnostics.cpp", "void ya::task(void*)", 2048),
    ("ui.cpp", "void ya::refresh(lv_timer_t*)", 4096),
    ("ui.cpp", "void ya::wifi_detail(uint32_t)", 1024),
    ("logs.cpp", "int ya::{anonymous}::capture(const char*, va_list)", 512),
])
def test_main_call_chain_static_stack(source, symbol, budget):
    usage = ROOT / os.environ.get("YA_BUILD_DIR", "build-release") / "esp-idf/main/CMakeFiles/__idf_main.dir" / (source + ".su")
    if not usage.exists() or usage.stat().st_mtime < (ROOT / "main" / source).stat().st_mtime:
        pytest.skip("Build the current firmware before checking compiler stack budgets")
    records = usage.read_text().splitlines()
    # A nested lambda record contains its parent's signature too; its tiny
    # frame must not accidentally satisfy the parent's stack budget.
    matches = [line for line in records if symbol in line and "::<lambda" not in line]
    if not matches:
        # GCC may print only "constprop(...)" for an optimized local helper.
        # Resolve that record by the original definition's source line, never
        # silently skip a budget when the compiler drops the function name.
        name = symbol.split("(")[0].split("::")[-1]
        definitions = [i for i, line in enumerate((ROOT / "main" / source).read_text().splitlines(), 1)
                       if re.search(r"\b" + re.escape(name) + r"\s*\(", line) and
                       line.startswith("static ")]
        matches = [line for line in records if any(f"/{source}:{i}:" in line for i in definitions)]
    assert len(matches) == 1, f"Missing or ambiguous stack record: {source}: {symbol}"
    record = matches[0]
    assert int(record.split("\t")[1]) <= budget, record
