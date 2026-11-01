/* =============================================================================
   Useful queries for System_full_of_sensors (run in SSMS, database: sensors)
   ============================================================================= */
USE sensors;
GO

-- 1. Latest 50 readings (readable, with Baku local time)
SELECT TOP (50) *
FROM   iot.vw_Reading
ORDER BY MeasuredAtUtc DESC, ReadingId DESC;

-- 2. Current status: the newest value of every sensor
SELECT DeviceCode, SensorCode, [Value], Unit, Label, MeasuredAtBaku
FROM   iot.vw_LatestReading
ORDER BY DeviceCode, SensorCode;

-- 3. Is the device alive? (no data for more than 30 seconds = offline)
SELECT  DeviceCode,
        LastSeenUtc,
        DATEDIFF(SECOND, LastSeenUtc, SYSUTCDATETIME()) AS SecondsAgo,
        CASE WHEN DATEDIFF(SECOND, LastSeenUtc, SYSUTCDATETIME()) <= 30
             THEN 'online' ELSE 'offline' END AS Status
FROM    iot.Device;

-- 4. Statistics per sensor over the last hour
SELECT  SensorCode, Unit, COUNT(*) AS Samples,
        AVG([Value]) AS AvgValue, MIN([Value]) AS MinValue, MAX([Value]) AS MaxValue
FROM    iot.vw_Reading
WHERE   MeasuredAtUtc >= DATEADD(HOUR, -1, SYSUTCDATETIME())
GROUP BY SensorCode, Unit
ORDER BY SensorCode;

-- 5. Hourly summary for the last 24 hours (ready for a chart)
SELECT *
FROM   iot.vw_HourlySummary
WHERE  HourUtc >= DATEADD(HOUR, -24, SYSUTCDATETIME())
ORDER BY SensorCode, HourUtc;

-- 6. How was the light during the last day? (share of each label)
SELECT  Label, COUNT(*) AS Samples
FROM    iot.vw_Reading
WHERE   SensorCode = 'light_level'
        AND MeasuredAtUtc >= DATEADD(DAY, -1, SYSUTCDATETIME())
GROUP BY Label
ORDER BY Samples DESC;

-- 7. Rows per sensor and size of the table
SELECT  s.SensorCode, COUNT_BIG(*) AS RowCnt
FROM    iot.Reading AS r
JOIN    iot.SensorType AS s ON s.SensorTypeId = r.SensorTypeId
GROUP BY s.SensorCode
ORDER BY RowCnt DESC;

EXEC sp_spaceused N'iot.Reading';

-- 8. Maintenance: delete data older than 90 days (run manually or from a scheduler)
-- EXEC iot.usp_PurgeOldReadings @KeepDays = 90;
GO
