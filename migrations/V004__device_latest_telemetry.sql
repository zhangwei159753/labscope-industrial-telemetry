ALTER TABLE devices
    ADD COLUMN IF NOT EXISTS last_telemetry jsonb;

ALTER TABLE devices
    ADD COLUMN IF NOT EXISTS last_telemetry_at timestamptz;
