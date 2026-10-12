CREATE OR REPLACE FUNCTION labscope_ensure_telemetry_partitions(
    p_anchor_date date DEFAULT ((clock_timestamp() AT TIME ZONE 'UTC')::date)
) RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    day_offset integer;
    partition_start date;
    partition_name text;
BEGIN
    FOR day_offset IN 0..1 LOOP
        partition_start := p_anchor_date + day_offset;
        partition_name := 'telemetry_samples_' || to_char(partition_start, 'YYYYMMDD');

        EXECUTE format(
            'CREATE TABLE IF NOT EXISTS %I PARTITION OF telemetry_samples FOR VALUES FROM (%L) TO (%L)',
            partition_name,
            partition_start::timestamp AT TIME ZONE 'UTC',
            (partition_start + 1)::timestamp AT TIME ZONE 'UTC'
        );

        INSERT INTO telemetry_partition_catalog (partition_name, partition_start)
        VALUES (partition_name, partition_start)
        ON CONFLICT ON CONSTRAINT telemetry_partition_catalog_pkey DO NOTHING;
    END LOOP;
END;
$$;
