# Sadə HTTP API + SQLite baza. ESP32 bura POST edir.
# İşə salmaq:  pip install flask   ->   python server.py
import sqlite3
from flask import Flask, request, jsonify

app = Flask(__name__)
DB = "data.db"


def get_db():
    conn = sqlite3.connect(DB)
    conn.row_factory = sqlite3.Row
    return conn


with get_db() as c:
    c.execute(
        """CREATE TABLE IF NOT EXISTS readings (
               id INTEGER PRIMARY KEY AUTOINCREMENT,
               device TEXT,
               distance_cm REAL,
               created_at TEXT DEFAULT CURRENT_TIMESTAMP
           )"""
    )


@app.post("/api/readings")
def add_reading():
    data = request.get_json(silent=True)
    if not data or "distance_cm" not in data:
        return jsonify(error="distance_cm lazımdır"), 400
    with get_db() as c:
        c.execute(
            "INSERT INTO readings (device, distance_cm) VALUES (?, ?)",
            (data.get("device", "unknown"), float(data["distance_cm"])),
        )
    return jsonify(ok=True), 201


@app.get("/api/readings")
def list_readings():
    with get_db() as c:
        rows = c.execute(
            "SELECT * FROM readings ORDER BY id DESC LIMIT 50"
        ).fetchall()
    return jsonify([dict(r) for r in rows])


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
