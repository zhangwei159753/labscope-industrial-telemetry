CREATE TABLE IF NOT EXISTS telemetry_partition_catalog (
    partition_name text PRIMARY KEY,
    partition_start date NOT NULL UNIQUE,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE IF NOT EXISTS telemetry_samples (
    site_id text NOT NULL,
    device_id text NOT NULL,
    received_at timestamptz NOT NULL,
    seq bigint NOT NULL,
    device_time timestamptz NOT NULL,
    metrics jsonb NOT NULL,
    schema_version smallint NOT NULL DEFAULT 1,
    PRIMARY KEY (site_id, device_id, received_at, seq),
    CONSTRAINT telemetry_samples_device_fk
        FOREIGN KEY (site_id, device_id)
        REFERENCES devices (site_id, device_id)
        ON DELETE RESTRICT,
    CONSTRAINT telemetry_samples_seq_nonnegative CHECK (seq >= 0),
    CONSTRAINT telemetry_samples_schema_version CHECK (schema_version > 0),
    CONSTRAINT telemetry_samples_metrics_object CHECK (jsonb_typeof(metrics) = 'object')
) PARTITION BY RANGE (received_at);

CREATE INDEX IF NOT EXISTS telemetry_samples_device_time_idx
    ON telemetry_samples (site_id, device_id, received_at DESC);

CREATE INDEX IF NOT EXISTS telemetry_samples_time_brin_idx
    ON telemetry_samples USING BRIN (received_at);

CREATE TABLE IF NOT EXISTS telemetry_1m (
    site_id text NOT NULL,
    device_id text NOT NULL,
    bucket_start timestamptz NOT NULL,
    metric_name text NOT NULL,
    min_value double precision NOT NULL,
    max_value double precision NOT NULL,
    sum_value double precision NOT NULL,
    sample_count bigint NOT NULL,
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (site_id, device_id, bucket_start, metric_name),
    CONSTRAINT telemetry_1m_device_fk
        FOREIGN KEY (site_id, device_id)
        REFERENCES devices (site_id, device_id)
        ON DELETE RESTRICT,
    CONSTRAINT telemetry_1m_metric_nonempty CHECK (length(metric_name) BETWEEN 1 AND 128),
    CONSTRAINT telemetry_1m_count_positive CHECK (sample_count > 0),
    CONSTRAINT telemetry_1m_values_ordered CHECK (min_value <= max_value)
);

CREATE INDEX IF NOT EXISTS telemetry_1m_device_time_idx
    ON telemetry_1m (site_id, device_id, bucket_start DESC);

CREATE TABLE IF NOT EXISTS telemetry_outbox (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    site_id text NOT NULL,
    device_id text NOT NULL,
    received_at timestamptz NOT NULL,
    seq bigint NOT NULL,
    event_type text NOT NULL,
    payload jsonb NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    processed_at timestamptz,
    attempt_count integer NOT NULL DEFAULT 0,
    last_error text,
    CONSTRAINT telemetry_outbox_event_unique
        UNIQUE (site_id, device_id, received_at, seq, event_type),
    CONSTRAINT telemetry_outbox_seq_nonnegative CHECK (seq >= 0),
    CONSTRAINT telemetry_outbox_attempts_nonnegative CHECK (attempt_count >= 0),
    CONSTRAINT telemetry_outbox_payload_object CHECK (jsonb_typeof(payload) = 'object')
);

CREATE INDEX IF NOT EXISTS telemetry_outbox_pending_idx
    ON telemetry_outbox (created_at, event_id)
    WHERE processed_at IS NULL;

CREATE INDEX IF NOT EXISTS telemetry_outbox_processed_idx
    ON telemetry_outbox (processed_at)
    WHERE processed_at IS NOT NULL;

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
        ON CONFLICT (partition_name) DO NOTHING;
    END LOOP;
END;
$$;

CREATE OR REPLACE FUNCTION labscope_maintain_telemetry(
    p_raw_retention interval DEFAULT interval '24 hours',
    p_aggregate_retention interval DEFAULT interval '30 days',
    p_outbox_retention interval DEFAULT interval '7 days'
) RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    cutoff timestamptz := clock_timestamp() - p_raw_retention;
    expired_partition record;
BEGIN
    PERFORM labscope_ensure_telemetry_partitions((clock_timestamp() AT TIME ZONE 'UTC')::date);

    DELETE FROM telemetry_samples WHERE received_at < cutoff;
    DELETE FROM telemetry_1m
        WHERE bucket_start < clock_timestamp() - p_aggregate_retention;
    DELETE FROM telemetry_outbox
        WHERE processed_at IS NOT NULL
          AND processed_at < clock_timestamp() - p_outbox_retention;

    FOR expired_partition IN
        SELECT partition_name
        FROM telemetry_partition_catalog
        WHERE partition_start + 1 <= (cutoff AT TIME ZONE 'UTC')::date
        ORDER BY partition_start
    LOOP
        EXECUTE format('DROP TABLE IF EXISTS %I', expired_partition.partition_name);
        DELETE FROM telemetry_partition_catalog
        WHERE partition_name = expired_partition.partition_name;
    END LOOP;
END;
$$;
