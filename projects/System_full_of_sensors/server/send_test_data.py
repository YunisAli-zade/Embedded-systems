"""ESP32-ni təqlid edir: serverə test ölçmələri göndərir.
Firmware-i yükləməzdən əvvəl serverin işlədiyini yoxlamaq üçündür.

İstifadə:
    python send_test_data.py
    python send_test_data.py http://192.168.1.100:5000 change-me
"""
import json
import sys
import urllib.error
import urllib.request

base = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:5000"
key = sys.argv[2] if len(sys.argv) > 2 else "change-me"

payload = {
    "device_id": "test-device",
    "uptime_ms": 60000,
    "readings": [
        {"sensor": "light_level", "value": 62.5, "unit": "%", "label": "bright", "uptime_ms": 55000},
        {"sensor": "air_temperature", "value": 23.4, "unit": "C", "label": "comfortable", "uptime_ms": 55300},
        {"sensor": "air_humidity", "value": 55, "unit": "%", "label": "comfortable", "uptime_ms": 55300},
        {"sensor": "tilt_angle", "value": 4.2, "unit": "deg", "label": "flat", "uptime_ms": 56000},
        {"sensor": "vibration_count", "value": 0, "unit": "count", "label": "none", "uptime_ms": 59000},
    ],
}

req = urllib.request.Request(
    base + "/api/readings",
    data=json.dumps(payload).encode("utf-8"),
    headers={"Content-Type": "application/json", "X-API-Key": key},
    method="POST",
)
try:
    with urllib.request.urlopen(req, timeout=5) as resp:
        print(resp.status, resp.read().decode())
except urllib.error.HTTPError as e:
    print("HTTP", e.code, e.read().decode())
except Exception as e:
    print("Connection error:", e)
