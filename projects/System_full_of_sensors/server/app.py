"""ESP32-S3 -> HTTP POST (JSON) -> Flask API -> SQL

İşə salmaq:   python app.py
Brauzerdə:    http://<noutbukun-IP>:5000/       (son ölçmələr)
Yoxlama:      http://<noutbukun-IP>:5000/health
API:          POST /api/readings   GET /api/readings   GET /api/latest   (başlıq: X-API-Key)

Mühit dəyişənləri (hamısı ixtiyaridir):
  API_KEY        ESP32 ilə eyni olmalıdır (menuconfig -> API key). Default: change-me
  DATABASE_URL   bax: db.py. Default: SQLite faylı; SQL Server üçün sql/01_install.sql lazımdır
  PORT           default 5000
  ENABLE_WEB_VIEW  "0" yazsan, açıq (açarsız) cədvəl səhifəsi söndürülür
"""
import hmac
import math
import os
from datetime import datetime, timedelta, timezone

from flask import Flask, jsonify, render_template_string, request

import db

API_KEY = os.environ.get("API_KEY", "change-me")
HOST = os.environ.get("HOST", "0.0.0.0")   # 0.0.0.0 = şəbəkədən (ESP32-dən) də qoşulmaq olar
PORT = int(os.environ.get("PORT", "5000"))
ENABLE_WEB_VIEW = os.environ.get("ENABLE_WEB_VIEW", "1") == "1"

MAX_READINGS_PER_REQUEST = 500
MAX_AGE_MS = 30 * 24 * 3600 * 1000   # ölçmə "yaşı" üçün ağlabatan yuxarı hədd

app = Flask(__name__)
db.init_db()


def utcnow():
    """UTC vaxt (timezone-suz) — bütün SQL növlərində eyni işləyir."""
    return datetime.now(timezone.utc).replace(tzinfo=None)


def key_ok():
    sent = request.headers.get("X-API-Key", "")
    return hmac.compare_digest(sent.encode("utf-8"), API_KEY.encode("utf-8"))


def _is_number(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


def parse_payload(data, received_at):
    """Gələn JSON-u yoxlayır və bazaya yazılacaq sətirlərə çevirir.

    Qaytarır: (rows, None) uğurludursa, (None, "xəta mətni") səhvdirsə.

    Vaxt məntiqi: ESP32-nin saatı yoxdur (internet/NTP yoxdur), ona görə
    cihaz öz uptime-ını göndərir. Ölçmənin real vaxtı =
        received_at - (batch_uptime - reading_uptime)
    Beləcə WiFi qopub sonra göndərilən köhnə ölçmələr də düzgün vaxt alır.
    """
    if not isinstance(data, dict):
        return None, "body must be a JSON object"

    device_id = data.get("device_id")
    if not isinstance(device_id, str) or not 1 <= len(device_id) <= 64:
        return None, "device_id must be a string (1-64 chars)"

    items = data.get("readings")
    if not isinstance(items, list) or not 1 <= len(items) <= MAX_READINGS_PER_REQUEST:
        return None, "readings must be a list (1-%d items)" % MAX_READINGS_PER_REQUEST

    batch_uptime = data.get("uptime_ms")
    rows = []
    for i, r in enumerate(items):
        if not isinstance(r, dict):
            return None, "readings[%d] must be an object" % i
        sensor = r.get("sensor")
        value = r.get("value")
        unit = r.get("unit", "")
        if not isinstance(sensor, str) or not 1 <= len(sensor) <= 64:
            return None, "readings[%d].sensor must be a string (1-64 chars)" % i
        if not _is_number(value):
            return None, "readings[%d].value must be a finite number" % i
        if not isinstance(unit, str) or len(unit) > 16:
            return None, "readings[%d].unit must be a string (max 16 chars)" % i
        label = r.get("label")          # ixtiyari: oxunaqlı etiket
        if label is not None and (not isinstance(label, str) or len(label) > 32):
            return None, "readings[%d].label must be a string (max 32 chars)" % i

        measured_at = received_at
        reading_uptime = r.get("uptime_ms")
        if _is_number(batch_uptime) and _is_number(reading_uptime):
            age_ms = min(max(0, batch_uptime - reading_uptime), MAX_AGE_MS)
            measured_at = received_at - timedelta(milliseconds=age_ms)

        rows.append({
            "device_id": device_id,
            "sensor": sensor,
            "value": float(value),
            "unit": unit,
            "label": label or None,
            "measured_at": measured_at,
            "received_at": received_at,
        })
    return rows, None


def serialize(row):
    out = dict(row)
    for k in ("measured_at", "received_at"):
        if isinstance(out.get(k), datetime):
            out[k] = out[k].isoformat() + "Z"
    return out


@app.get("/health")
def health():
    return jsonify(status="ok", time=utcnow().isoformat() + "Z")


@app.post("/api/readings")
def post_readings():
    if not key_ok():
        return jsonify(error="invalid api key"), 401

    data = request.get_json(force=True, silent=True)
    rows, err = parse_payload(data, utcnow())
    if err:
        return jsonify(error=err), 400

    try:
        db.insert_readings(rows)
    except Exception:
        app.logger.exception("Database insert failed")
        return jsonify(error="database error"), 500

    return jsonify(ok=True, inserted=len(rows)), 201


@app.get("/api/readings")
def get_readings():
    if not key_ok():
        return jsonify(error="invalid api key"), 401
    limit = min(max(request.args.get("limit", 50, type=int), 1), 1000)
    rows = db.latest(limit, request.args.get("device_id"), request.args.get("sensor"))
    return jsonify([serialize(r) for r in rows])


@app.get("/api/latest")
def get_latest():
    """Hər cihaz və sensor üçün ən son dəyər (cari vəziyyət)."""
    if not key_ok():
        return jsonify(error="invalid api key"), 401
    rows = db.latest_per_sensor(request.args.get("device_id"))
    return jsonify([serialize(r) for r in rows])


PAGE = """<!doctype html>
<html lang="az"><head><meta charset="utf-8">
<meta http-equiv="refresh" content="5">
<title>Sensor data</title>
<style>
 body{font-family:system-ui,sans-serif;margin:24px}
 table{border-collapse:collapse}
 th,td{border:1px solid #ccc;padding:4px 10px;text-align:left}
 th{background:#f0f0f0}
</style></head><body>
<h2>Son ölçmələr (5 saniyədən bir yenilənir, vaxt UTC)</h2>
<table>
<tr><th>Vaxt (UTC)</th><th>Cihaz</th><th>Sensor</th><th>Dəyər</th><th>Vahid</th><th>Etiket</th></tr>
{% for r in rows %}
<tr><td>{{ r.measured_at.strftime('%Y-%m-%d %H:%M:%S') }}</td><td>{{ r.device_id }}</td>
<td>{{ r.sensor }}</td><td>{{ '%.2f'|format(r.value) }}</td><td>{{ r.unit }}</td><td>{{ r.label or '' }}</td></tr>
{% else %}
<tr><td colspan="6">Hələ data yoxdur</td></tr>
{% endfor %}
</table></body></html>"""


@app.get("/")
def index():
    if not ENABLE_WEB_VIEW:
        return "disabled", 404
    return render_template_string(PAGE, rows=db.latest(50))


if __name__ == "__main__":
    app.run(host=HOST, port=PORT, threaded=True)
