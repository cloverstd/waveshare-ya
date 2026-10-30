import os
import threading, time, pathlib, sys, requests
from device import Device
from ota_smoke import session

base, token = session()
d = Device(os.environ["YA_DEVICE_PORT"])
data = bytearray(pathlib.Path("build/waveshare-ya.bin").read_bytes())
data[80] ^= 1
result = {}


def send():
    try:
        s = requests.Session()
        s.trust_env = False
        r = s.post(
            base + "/api/ota",
            data=data,
            headers={"Authorization": "Bearer " + token},
            timeout=(60, 180),
        )
        result.update(http=r.status_code)
    except Exception as e:
        result.update(error=type(e).__name__)


t = threading.Thread(target=send)
t.start()
log = bytearray()
end = time.monotonic() + 60
while (t.is_alive() or not result) and time.monotonic() < end:
    try:
        log.extend(d.serial.read(4096))
    except Exception:
        break
extra = time.monotonic() + 5
while time.monotonic() < extra:
    try:
        log.extend(d.serial.read(4096))
    except Exception:
        break
pathlib.Path("private/upload-debug.log").write_bytes(log)
try:
    print("Result", result, "status", d.request({"op": "status"}))
finally:
    d.close()
