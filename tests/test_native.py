"""Run the same platform-independent policies used by firmware."""
import pathlib
import subprocess
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]

@pytest.mark.parametrize("source", ["policy_test.cpp", "service_policy_test.cpp", "key_wake_test.cpp", "ui_policy_test.cpp", "data_protocol_test.cpp", "power_policy_test.cpp", "wifi_policy_test.cpp", "notifications_test.cpp", "log_buffer_test.cpp"])
def test_native_policy(source, tmp_path):
    executable = tmp_path / "policy"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "tests" / source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
