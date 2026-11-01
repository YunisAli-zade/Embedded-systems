/* =============================================================================
   Optional: moves the rows of the OLD flat table dbo.readings into the new
   normalized tables (iot.Device, iot.SensorType, iot.Reading).

   Run this ONLY ONCE, after 01_install.sql, and only if dbo.readings exists.
   When it finishes, the old table is renamed to dbo.readings_legacy (nothing is
   deleted, you can drop it yourself later).
   ============================================================================= */
USE sensors;
GO

SET NOCOUNT ON;

IF OBJECT_ID(N'dbo.readings', N'U') IS NULL
BEGIN
    PRINT N'dbo.readings was not found - nothing to migrate.';
    RETURN;
END;

BEGIN TRANSACTION;

INSERT iot.Device (DeviceCode, LastSeenUtc)
SELECT  r.device_id, MAX(r.received_at)
FROM    dbo.readings AS r
WHERE   NOT EXISTS (SELECT 1 FROM iot.Device AS d WHERE d.DeviceCode = r.device_id)
GROUP BY r.device_id;

INSERT iot.SensorType (SensorCode, Unit)
SELECT  r.sensor, MAX(r.unit)
FROM    dbo.readings AS r
WHERE   NOT EXISTS (SELECT 1 FROM iot.SensorType AS s WHERE s.SensorCode = r.sensor)
GROUP BY r.sensor;

-- the old table may or may not have the "label" column
DECLARE @labelExpr NVARCHAR(20) =
        CASE WHEN COL_LENGTH(N'dbo.readings', N'label') IS NOT NULL THEN N'r.label' ELSE N'NULL' END;

DECLARE @sql NVARCHAR(MAX) = N'
INSERT iot.Reading (DeviceId, SensorTypeId, [Value], Label, MeasuredAtUtc, ReceivedAtUtc)
SELECT  d.DeviceId, s.SensorTypeId, r.[value], ' + @labelExpr + N', r.measured_at, r.received_at
FROM    dbo.readings AS r
JOIN    iot.Device     AS d ON d.DeviceCode = r.device_id
JOIN    iot.SensorType AS s ON s.SensorCode = r.sensor;';

EXEC sys.sp_executesql @sql;

COMMIT TRANSACTION;

EXEC sys.sp_rename N'dbo.readings', N'readings_legacy';

PRINT N'Migration finished. The old table is now dbo.readings_legacy.';
SELECT COUNT_BIG(*) AS MigratedRows FROM iot.Reading;
GO
