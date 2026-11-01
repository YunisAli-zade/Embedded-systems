/* =============================================================================
   System_full_of_sensors - SQL Server installer (idempotent: safe to re-run)

   How to run: open this file in SSMS and press Execute (F5).
   Requires:   SQL Server 2016 SP1 or newer (CREATE OR ALTER, OPENJSON, AT TIME ZONE).

   Creates:
     database  sensors
     schema    iot
     tables    iot.Device, iot.SensorType, iot.Reading
     views     iot.vw_Reading, iot.vw_LatestReading, iot.vw_HourlySummary
     procs     iot.usp_InsertReadings, iot.usp_PurgeOldReadings
     role      iot_api  (least privilege for the Flask API)

   Conventions: PascalCase names, singular table names, UTC timestamps (DATETIME2),
                every constraint has an explicit name (PK_, FK_, UQ_, CK_, DF_, IX_).
   ============================================================================= */
SET NOCOUNT ON;
GO

IF DB_ID(N'sensors') IS NULL
    CREATE DATABASE sensors;
GO

USE sensors;
GO

/* ---------------------------------------------------------------------------
   1. Schema
   --------------------------------------------------------------------------- */
IF SCHEMA_ID(N'iot') IS NULL
    EXEC (N'CREATE SCHEMA iot AUTHORIZATION dbo;');
GO

/* ---------------------------------------------------------------------------
   2. Tables
   --------------------------------------------------------------------------- */

-- One row per physical device (an ESP32). Registered automatically on first data.
IF OBJECT_ID(N'iot.Device', N'U') IS NULL
BEGIN
    CREATE TABLE iot.Device
    (
        DeviceId     INT           IDENTITY(1,1) NOT NULL,
        DeviceCode   VARCHAR(64)   NOT NULL,          -- value of GW_DEVICE_ID, e.g. esp32s3-01
        DisplayName  NVARCHAR(100) NULL,
        Location     NVARCHAR(100) NULL,
        IsActive     BIT           NOT NULL CONSTRAINT DF_Device_IsActive     DEFAULT (1),
        FirstSeenUtc DATETIME2(0)  NOT NULL CONSTRAINT DF_Device_FirstSeenUtc DEFAULT (SYSUTCDATETIME()),
        LastSeenUtc  DATETIME2(0)  NULL,
        CONSTRAINT PK_Device            PRIMARY KEY CLUSTERED (DeviceId),
        CONSTRAINT UQ_Device_DeviceCode UNIQUE (DeviceCode)
    );
END
GO

-- Catalogue of measured quantities (light_level, air_temperature, ...).
IF OBJECT_ID(N'iot.SensorType', N'U') IS NULL
BEGIN
    CREATE TABLE iot.SensorType
    (
        SensorTypeId SMALLINT      IDENTITY(1,1) NOT NULL,
        SensorCode   VARCHAR(64)   NOT NULL,          -- value of "sensor" in the JSON
        Unit         VARCHAR(16)   NOT NULL,
        Description  NVARCHAR(200) NULL,
        CONSTRAINT PK_SensorType            PRIMARY KEY CLUSTERED (SensorTypeId),
        CONSTRAINT UQ_SensorType_SensorCode UNIQUE (SensorCode)
    );
END
GO

-- The measurements themselves (append-only, one row per sensor value).
IF OBJECT_ID(N'iot.Reading', N'U') IS NULL
BEGIN
    CREATE TABLE iot.Reading
    (
        ReadingId     BIGINT       IDENTITY(1,1) NOT NULL,
        DeviceId      INT          NOT NULL,
        SensorTypeId  SMALLINT     NOT NULL,
        [Value]       FLOAT        NOT NULL,
        Label         VARCHAR(32)  NULL,              -- readable class: bright, comfortable, ...
        MeasuredAtUtc DATETIME2(3) NOT NULL,          -- when the sensor was read (reconstructed)
        ReceivedAtUtc DATETIME2(3) NOT NULL CONSTRAINT DF_Reading_ReceivedAtUtc DEFAULT (SYSUTCDATETIME()),
        CONSTRAINT PK_Reading            PRIMARY KEY CLUSTERED (ReadingId),
        CONSTRAINT FK_Reading_Device     FOREIGN KEY (DeviceId)     REFERENCES iot.Device (DeviceId),
        CONSTRAINT FK_Reading_SensorType FOREIGN KEY (SensorTypeId) REFERENCES iot.SensorType (SensorTypeId),
        CONSTRAINT CK_Reading_MeasuredBeforeReceived CHECK (MeasuredAtUtc <= ReceivedAtUtc)
    );
END
GO

/* ---------------------------------------------------------------------------
   3. Indexes
   --------------------------------------------------------------------------- */

-- Typical query: "latest values of one sensor on one device"
IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_Reading_Device_Sensor_Time' AND object_id = OBJECT_ID(N'iot.Reading'))
    CREATE NONCLUSTERED INDEX IX_Reading_Device_Sensor_Time
        ON iot.Reading (DeviceId, SensorTypeId, MeasuredAtUtc DESC)
        INCLUDE ([Value], Label);
GO

-- "Newest rows overall" and the retention purge
IF NOT EXISTS (SELECT 1 FROM sys.indexes
               WHERE name = N'IX_Reading_MeasuredAtUtc' AND object_id = OBJECT_ID(N'iot.Reading'))
    CREATE NONCLUSTERED INDEX IX_Reading_MeasuredAtUtc
        ON iot.Reading (MeasuredAtUtc DESC);
GO

/* ---------------------------------------------------------------------------
   4. Reference data (sensor catalogue)
   --------------------------------------------------------------------------- */
INSERT iot.SensorType (SensorCode, Unit, Description)
SELECT v.SensorCode, v.Unit, v.Description
FROM (VALUES
    ('vibration_count', 'count', N'Vibration pulses since the previous reading'),
    ('light_level',     '%',     N'Ambient light level, 0 (dark) to 100 (very bright)'),
    ('air_temperature', 'C',     N'Air temperature from the DHT11'),
    ('air_humidity',    '%',     N'Relative humidity from the DHT11'),
    ('tilt_angle',      'deg',   N'Angle between the MPU6050 Z axis and the vertical'),
    ('rotation_rate',   'dps',   N'Total rotation speed from the MPU6050 gyroscope'),
    ('accel_x',         'g',     N'Raw acceleration, X axis (optional)'),
    ('accel_y',         'g',     N'Raw acceleration, Y axis (optional)'),
    ('accel_z',         'g',     N'Raw acceleration, Z axis (optional)'),
    ('gyro_x',          'dps',   N'Raw angular rate, X axis (optional)'),
    ('gyro_y',          'dps',   N'Raw angular rate, Y axis (optional)'),
    ('gyro_z',          'dps',   N'Raw angular rate, Z axis (optional)'),
    ('chip_temp',       'C',     N'ESP32-S3 internal chip temperature (optional)')
) AS v (SensorCode, Unit, Description)
WHERE NOT EXISTS (SELECT 1 FROM iot.SensorType AS s WHERE s.SensorCode = v.SensorCode);
GO

/* ---------------------------------------------------------------------------
   5. Views
   --------------------------------------------------------------------------- */

-- Flat, human-friendly view of every reading (the API reads from this view).
CREATE OR ALTER VIEW iot.vw_Reading
AS
SELECT  r.ReadingId,
        d.DeviceCode,
        s.SensorCode,
        r.[Value],
        s.Unit,
        r.Label,
        r.MeasuredAtUtc,
        r.ReceivedAtUtc,
        CAST(r.MeasuredAtUtc AT TIME ZONE 'UTC' AT TIME ZONE 'Azerbaijan Standard Time' AS DATETIME2(0)) AS MeasuredAtBaku
FROM    iot.Reading    AS r
JOIN    iot.Device     AS d ON d.DeviceId     = r.DeviceId
JOIN    iot.SensorType AS s ON s.SensorTypeId = r.SensorTypeId;
GO

-- The most recent value of every sensor on every device ("current status").
CREATE OR ALTER VIEW iot.vw_LatestReading
AS
SELECT  x.ReadingId, x.DeviceCode, x.SensorCode, x.[Value], x.Unit, x.Label,
        x.MeasuredAtUtc, x.ReceivedAtUtc, x.MeasuredAtBaku
FROM (
        SELECT  v.ReadingId, v.DeviceCode, v.SensorCode, v.[Value], v.Unit, v.Label,
                v.MeasuredAtUtc, v.ReceivedAtUtc, v.MeasuredAtBaku,
                ROW_NUMBER() OVER (PARTITION BY v.DeviceCode, v.SensorCode
                                   ORDER BY v.MeasuredAtUtc DESC, v.ReadingId DESC) AS rn
        FROM    iot.vw_Reading AS v
     ) AS x
WHERE   x.rn = 1;
GO

-- Hourly aggregates, handy for charts and reports.
CREATE OR ALTER VIEW iot.vw_HourlySummary
AS
SELECT  v.DeviceCode,
        v.SensorCode,
        v.Unit,
        DATEADD(HOUR, DATEDIFF(HOUR, 0, v.MeasuredAtUtc), 0) AS HourUtc,
        COUNT(*)         AS Samples,
        AVG(v.[Value])   AS AvgValue,
        MIN(v.[Value])   AS MinValue,
        MAX(v.[Value])   AS MaxValue
FROM    iot.vw_Reading AS v
GROUP BY v.DeviceCode, v.SensorCode, v.Unit, DATEADD(HOUR, DATEDIFF(HOUR, 0, v.MeasuredAtUtc), 0);
GO

/* ---------------------------------------------------------------------------
   6. Stored procedures
   --------------------------------------------------------------------------- */

-- Inserts one batch coming from the API. Devices and sensor types that are seen
-- for the first time are registered automatically. All-or-nothing (one transaction).
--   @ReadingsJson example:
--   [{"sensor":"light_level","value":62.5,"unit":"%","label":"bright","measuredAt":"2026-10-28T12:00:00.000"}]
CREATE OR ALTER PROCEDURE iot.usp_InsertReadings
    @DeviceCode    VARCHAR(64),
    @ReceivedAtUtc DATETIME2(3),
    @ReadingsJson  NVARCHAR(MAX)
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;

    IF ISJSON(@ReadingsJson) <> 1
        THROW 50001, N'@ReadingsJson is not valid JSON.', 1;

    DECLARE @In TABLE
    (
        SensorCode    VARCHAR(64)  NOT NULL,
        Unit          VARCHAR(16)  NOT NULL,
        [Value]       FLOAT        NOT NULL,
        Label         VARCHAR(32)  NULL,
        MeasuredAtUtc DATETIME2(3) NOT NULL
    );

    INSERT @In (SensorCode, Unit, [Value], Label, MeasuredAtUtc)
    SELECT  j.sensor, ISNULL(j.unit, ''), j.[value], NULLIF(j.label, ''), j.measuredAt
    FROM    OPENJSON(@ReadingsJson)
            WITH (
                sensor     VARCHAR(64)  '$.sensor',
                unit       VARCHAR(16)  '$.unit',
                [value]    FLOAT        '$.value',
                label      VARCHAR(32)  '$.label',
                measuredAt DATETIME2(3) '$.measuredAt'
            ) AS j;

    BEGIN TRY
        BEGIN TRANSACTION;

        DECLARE @DeviceId INT;

        SELECT @DeviceId = DeviceId
        FROM   iot.Device WITH (UPDLOCK, HOLDLOCK)
        WHERE  DeviceCode = @DeviceCode;

        IF @DeviceId IS NULL
        BEGIN
            INSERT iot.Device (DeviceCode, LastSeenUtc) VALUES (@DeviceCode, @ReceivedAtUtc);
            SET @DeviceId = SCOPE_IDENTITY();
        END
        ELSE
        BEGIN
            UPDATE iot.Device SET LastSeenUtc = @ReceivedAtUtc WHERE DeviceId = @DeviceId;
        END

        -- register sensor types that are new
        INSERT iot.SensorType (SensorCode, Unit)
        SELECT  i.SensorCode, MAX(i.Unit)
        FROM    @In AS i
        WHERE   NOT EXISTS (SELECT 1
                            FROM   iot.SensorType AS s WITH (UPDLOCK, HOLDLOCK)
                            WHERE  s.SensorCode = i.SensorCode)
        GROUP BY i.SensorCode;

        INSERT iot.Reading (DeviceId, SensorTypeId, [Value], Label, MeasuredAtUtc, ReceivedAtUtc)
        SELECT  @DeviceId, s.SensorTypeId, i.[Value], i.Label, i.MeasuredAtUtc, @ReceivedAtUtc
        FROM    @In AS i
        JOIN    iot.SensorType AS s ON s.SensorCode = i.SensorCode;

        COMMIT TRANSACTION;
    END TRY
    BEGIN CATCH
        IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION;
        THROW;
    END CATCH
END
GO

-- Retention: deletes readings older than @KeepDays in small batches
-- (keeps the transaction log small). Example:  EXEC iot.usp_PurgeOldReadings @KeepDays = 90;
CREATE OR ALTER PROCEDURE iot.usp_PurgeOldReadings
    @KeepDays  INT = 90,
    @BatchSize INT = 10000
AS
BEGIN
    SET NOCOUNT ON;

    IF @KeepDays < 1
        THROW 50002, N'@KeepDays must be at least 1.', 1;

    DECLARE @Cutoff  DATETIME2(3) = DATEADD(DAY, -@KeepDays, SYSUTCDATETIME());
    DECLARE @Deleted INT = 1;
    DECLARE @Total   BIGINT = 0;

    WHILE @Deleted > 0
    BEGIN
        DELETE TOP (@BatchSize) FROM iot.Reading WHERE MeasuredAtUtc < @Cutoff;
        SET @Deleted = @@ROWCOUNT;
        SET @Total = @Total + @Deleted;
    END

    SELECT @Total AS DeletedRows, @Cutoff AS CutoffUtc;
END
GO

/* ---------------------------------------------------------------------------
   7. Security: least-privilege role for the Flask API
      (it may only call the insert procedure and read the views)
   --------------------------------------------------------------------------- */
IF DATABASE_PRINCIPAL_ID(N'iot_api') IS NULL
    CREATE ROLE iot_api;
GO

GRANT EXECUTE ON OBJECT::iot.usp_InsertReadings TO iot_api;
GRANT SELECT  ON OBJECT::iot.vw_Reading         TO iot_api;
GRANT SELECT  ON OBJECT::iot.vw_LatestReading   TO iot_api;
GO

-- Optional: a dedicated SQL login for the API instead of Windows authentication.
-- Replace the password, then uncomment and run once.
-- CREATE LOGIN iot_api_login WITH PASSWORD = N'<strong password here>', CHECK_POLICY = ON;
-- CREATE USER  iot_api_user  FOR LOGIN iot_api_login;
-- ALTER ROLE   iot_api ADD MEMBER iot_api_user;

/* ---------------------------------------------------------------------------
   8. Verification: lists every object in the iot schema
   --------------------------------------------------------------------------- */
SELECT  o.type_desc AS ObjectType, o.name AS ObjectName
FROM    sys.objects AS o
WHERE   o.schema_id = SCHEMA_ID(N'iot') AND o.is_ms_shipped = 0
        AND o.type IN ('U', 'V', 'P')
ORDER BY o.type_desc, o.name;
GO
