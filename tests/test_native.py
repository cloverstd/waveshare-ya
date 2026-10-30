"""Run the same platform-independent policies used by firmware."""
import pathlib
import subprocess
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("source", [
    "gallery_policy_test.cpp", "policy_test.cpp", "service_policy_test.cpp", "key_wake_test.cpp",
    "wake_handoff_test.cpp", "ui_policy_test.cpp", "data_protocol_test.cpp",
    "power_policy_test.cpp", "wifi_policy_test.cpp", "notifications_test.cpp",
    "log_buffer_test.cpp", "companion_policy_test.cpp", "rich_codec_test.cpp", "battery_estimator_test.cpp",
    "ota_web_session_test.cpp",
])
def test_native_policy(source, tmp_path):
    executable = tmp_path / "policy"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "tests" / source), *(["-lz"] if source == "rich_codec_test.cpp" else []), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
