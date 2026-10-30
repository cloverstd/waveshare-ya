import hashlib, importlib.util, pathlib, struct, json, shutil, subprocess, os
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release", ROOT / "tools/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def image():
    b = bytearray(512)
    b[0] = 0xE9
    struct.pack_into("<I", b, 32, 0xABCD5432)
    b[48:53] = b"0.1.0"
    b[80:92] = b"waveshare-ya"
    struct.pack_into("<I", b, 288, 0x59414142)
    b[292:316] = b"esp32-s3-touch-amoled-2.16"
    b[332:338] = b"ab6-v1"
    return b


def test_valid_metadata():
    assert release.image_metadata(image()) == "0.1.0"


@pytest.mark.parametrize("offset", [0, 32, 80, 288, 292, 332])
def test_wrong_headers(offset):
    b = image()
    b[offset] = 0
    with pytest.raises(ValueError):
        release.image_metadata(b)


def test_oversize():
    with pytest.raises(ValueError):
        release.image_metadata(bytes(0x600001))


def test_partitions():
    partitions = []
    for line in (ROOT / "partitions.csv").read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        fields = [x.strip() for x in line.split(",")]
        partitions.append((fields[0], int(fields[3], 0), int(fields[4], 0)))
    for a, b in zip(partitions, partitions[1:]):
        assert a[1] + a[2] <= b[1]
    assert partitions[-1][1] + partitions[-1][2] == 0x1000000
    for name, start, size in partitions:
        if name.startswith("ota_"):
            assert size == 0x600000 and start % 0x10000 == 0


def test_real_signed_image_header():
    p = ROOT / os.environ.get("YA_BUILD_DIR", "build-release") / "waveshare-ya.bin"
    if not p.exists():
        pytest.skip("Build first")
    assert release.image_metadata(p.read_bytes())


def test_release_rejects_insecure_transport(tmp_path):
    p = tmp_path / "app.bin"
    p.write_bytes(image())
    with pytest.raises(ValueError, match="HTTPS"):
        release.package(
            p, tmp_path / "out", "http://example.com/fw.bin", tmp_path / "missing-key"
        )
    assert not (tmp_path / "out").exists()


def test_release_rejects_fault_firmware(tmp_path):
    b = image()
    b[48:80] = b"0.1.0-test-abort".ljust(32, b"\0")
    p = tmp_path / "app.bin"
    p.write_bytes(b)
    with pytest.raises(ValueError, match="Test firmware"):
        release.package(
            p, tmp_path / "out", "https://example.com/fw.bin", tmp_path / "missing-key"
        )
    assert not (tmp_path / "out").exists()


@pytest.mark.parametrize("version", ["", "../escape", "a" * 32, "版本", "1.0\n"])
def test_rejects_versions_unsafe_for_contract_or_output_path(version):
    b = image()
    b[48:80] = version.encode().ljust(32, b"\0")
    with pytest.raises(ValueError, match="version"):
        release.image_metadata(b)


@pytest.mark.parametrize("url", [
    "http://example.com/fw.bin", "https://", "https:///fw.bin",
    "https://name:secret@example.com/fw.bin", "https://example.com/fw.bin#fragment",
    "https://example.com/fw.bin#", "https://example.com/a b.bin",
    "https://example.com:99999/fw.bin", "https://example.com/固件.bin",
    "https://example.com/" + "a" * 384,
])
def test_release_rejects_unusable_download_url(url):
    with pytest.raises(ValueError, match="HTTPS"):
        release.validate_url(url)


def test_default_package_matches_device_contract(tmp_path, monkeypatch):
    image_path = tmp_path / "app.bin"
    raw = image()
    image_path.write_bytes(raw)
    calls = []
    monkeypatch.setattr(release.subprocess, "run", lambda args, **kwargs: calls.append((args, kwargs)))
    output = tmp_path / "out"
    result = release.package(image_path, output, None, tmp_path / "signing.pem", notes="更新说明")
    assert result == json.loads((output / "manifest.json").read_text())
    assert result["url"] == "https://ya.cloverstd.com/firmware/waveshare-ya/0.1.0/waveshare-ya-0.1.0.bin"
    assert result["product"] == "waveshare-ya"
    assert result["board"] == "esp32-s3-touch-amoled-2.16"
    assert result["layout"] == "ab6-v1"
    assert result["version"] == "0.1.0"
    assert result["size"] == len(raw)
    assert result["sha256"] == hashlib.sha256(raw).hexdigest()
    assert (output / "waveshare-ya-0.1.0.bin").read_bytes() == raw
    assert len((output / "manifest.json").read_bytes()) < 4096
    assert calls and "verify_signature" in calls[0][0] and calls[0][1]["check"]


@pytest.mark.parametrize("notes", ["字" * 86, "bad\0note"])
def test_release_rejects_notes_firmware_cannot_read(tmp_path, notes):
    p = tmp_path / "app.bin"
    p.write_bytes(image())
    with pytest.raises(ValueError, match="notes"):
        release.package(p, tmp_path / "out", None, tmp_path / "key", notes=notes)
    assert not (tmp_path / "out").exists()


def test_signature_failure_does_not_publish_manifest_or_binary(tmp_path, monkeypatch):
    p = tmp_path / "app.bin"
    p.write_bytes(image())
    def reject(args, **kwargs):
        raise subprocess.CalledProcessError(1, args)
    monkeypatch.setattr(release.subprocess, "run", reject)
    with pytest.raises(subprocess.CalledProcessError):
        release.package(p, tmp_path / "out", None, tmp_path / "key")
    assert not (tmp_path / "out").exists()


def test_firmware_manifest_parser_on_host(tmp_path):
    cjson = ROOT / ".tools/esp-idf/components/json/cJSON"
    cc, cxx = shutil.which("cc"), shutil.which("c++")
    if not cc or not cxx or not (cjson / "cJSON.c").exists():
        pytest.skip("Native compiler and pinned ESP-IDF cJSON source are required")
    obj = tmp_path / "cJSON.o"
    binary = tmp_path / "ota-contract-test"
    subprocess.run([cc, "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(cjson),
                    str(ROOT / "tests/ota_contract_test.cpp"), str(obj), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
