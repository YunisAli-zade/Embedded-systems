"""Verilənlər bazası qatı (SQLAlchemy).

İki rejim var, DATABASE_URL-dən asılı olaraq avtomatik seçilir:

1) Microsoft SQL Server  (mssql+pyodbc://...)   <- əsas, peşəkar rejim
   Sxem sql/01_install.sql skripti ilə SSMS-də yaradılır (schema iot, cədvəllər
   Device / SensorType / Reading, view-lar, stored procedure-lar).
   Yazma:   EXEC iot.usp_InsertReadings (bir sorğu = bir tranzaksiya)
   Oxuma:   iot.vw_Reading və iot.vw_LatestReading view-ları

2) Digər bazalar (SQLite - default, MySQL, PostgreSQL)   <- sürətli test üçün
   Sadə düz "readings" cədvəli proqram tərəfindən avtomatik yaradılır.

DATABASE_URL nümunələri:
  sqlite:///sensors.db                         (default)
  mssql+pyodbc://@localhost\\MSSQLSERVER01/sensors?driver=ODBC+Driver+18+for+SQL+Server&trusted_connection=yes&TrustServerCertificate=yes
  mysql+pymysql://USER:PAROL@localhost/sensors
  postgresql+psycopg2://USER:PAROL@localhost/sensors
"""
import json
import os

from sqlalchemy import (Column, DateTime, Float, Index, Integer, MetaData, String,
                        Table, create_engine, insert, select, text)

_default_sqlite = "sqlite:///" + os.path.join(os.path.dirname(os.path.abspath(__file__)), "sensors.db")
DATABASE_URL = os.environ.get("DATABASE_URL", _default_sqlite)

engine = create_engine(DATABASE_URL, pool_pre_ping=True)
IS_MSSQL = engine.dialect.name == "mssql"

# ---------------------------------------------------------------------------
# Düz cədvəl (yalnız SQL Server olmayan sınaq rejimi üçün)
# ---------------------------------------------------------------------------
metadata = MetaData()
readings = Table(
    "readings",
    metadata,
    Column("id", Integer, primary_key=True, autoincrement=True),
    Column("device_id", String(64), nullable=False),
    Column("sensor", String(64), nullable=False),
    Column("value", Float, nullable=False),
    Column("unit", String(16), nullable=False),
    Column("label", String(32), nullable=True),
    Column("measured_at", DateTime, nullable=False),   # UTC
    Column("received_at", DateTime, nullable=False),   # UTC
    Index("ix_readings_dev_sensor_time", "device_id", "sensor", "measured_at"),
)


def init_db():
    """SQL Server-də sxemin qurulduğunu yoxlayır, digər bazalarda cədvəli yaradır."""
    if IS_MSSQL:
        with engine.connect() as conn:
            missing = [name for name in ("iot.usp_InsertReadings", "iot.vw_Reading", "iot.vw_LatestReading")
                       if conn.execute(text("SELECT OBJECT_ID(:n)"), {"n": name}).scalar() is None]
        if missing:
            raise RuntimeError(
                "SQL Server-də bu obyektlər tapılmadı: %s. "
                "Əvvəlcə SSMS-də sql/01_install.sql skriptini işlət." % ", ".join(missing))
    else:
        metadata.create_all(engine)


def insert_readings(rows):
    """rows: [{device_id, sensor, value, unit, label, measured_at, received_at}, ...]
    Bir sorğudakı bütün sətirlər eyni cihaza aiddir (app.py bunu təmin edir)."""
    if not rows:
        return
    if IS_MSSQL:
        payload = json.dumps([
            {
                "sensor": r["sensor"],
                "value": r["value"],
                "unit": r["unit"],
                "label": r.get("label"),
                "measuredAt": r["measured_at"].isoformat(timespec="milliseconds"),
            }
            for r in rows
        ])
        with engine.begin() as conn:
            conn.execute(
                text("EXEC iot.usp_InsertReadings @DeviceCode = :device, "
                     "@ReceivedAtUtc = :received, @ReadingsJson = :payload"),
                {"device": rows[0]["device_id"], "received": rows[0]["received_at"], "payload": payload},
            )
    else:
        with engine.begin() as conn:
            conn.execute(insert(readings), rows)


_MSSQL_COLUMNS = ("ReadingId AS id, DeviceCode AS device_id, SensorCode AS sensor, [Value] AS value, "
                  "Unit AS unit, Label AS label, MeasuredAtUtc AS measured_at, ReceivedAtUtc AS received_at")


def latest(limit=50, device_id=None, sensor=None):
    """Ən son ölçmələri (yeni -> köhnə) dict siyahısı kimi qaytarır."""
    if IS_MSSQL:
        sql = "SELECT TOP (:limit) " + _MSSQL_COLUMNS + " FROM iot.vw_Reading WHERE 1 = 1"
        params = {"limit": int(limit)}
        if device_id:
            sql += " AND DeviceCode = :device"
            params["device"] = device_id
        if sensor:
            sql += " AND SensorCode = :sensor"
            params["sensor"] = sensor
        sql += " ORDER BY MeasuredAtUtc DESC, ReadingId DESC"
        with engine.connect() as conn:
            return [dict(row) for row in conn.execute(text(sql), params).mappings()]

    stmt = select(readings).order_by(readings.c.measured_at.desc(), readings.c.id.desc())
    if device_id:
        stmt = stmt.where(readings.c.device_id == device_id)
    if sensor:
        stmt = stmt.where(readings.c.sensor == sensor)
    stmt = stmt.limit(limit)
    with engine.connect() as conn:
        return [dict(row) for row in conn.execute(stmt).mappings()]


def latest_per_sensor(device_id=None):
    """Hər cihaz və sensor üçün ən son dəyər ("cari vəziyyət")."""
    if IS_MSSQL:
        sql = ("SELECT ReadingId AS id, DeviceCode AS device_id, SensorCode AS sensor, [Value] AS value, "
               "Unit AS unit, Label AS label, MeasuredAtUtc AS measured_at, ReceivedAtUtc AS received_at "
               "FROM iot.vw_LatestReading WHERE 1 = 1")
        params = {}
        if device_id:
            sql += " AND DeviceCode = :device"
            params["device"] = device_id
        sql += " ORDER BY DeviceCode, SensorCode"
        with engine.connect() as conn:
            return [dict(row) for row in conn.execute(text(sql), params).mappings()]

    newest = {}
    for row in latest(1000, device_id):          # yeni -> köhnə: ilk rast gəlinən ən yenisidir
        newest.setdefault((row["device_id"], row["sensor"]), row)
    return sorted(newest.values(), key=lambda r: (r["device_id"], r["sensor"]))
