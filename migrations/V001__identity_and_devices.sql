CREATE TABLE IF NOT EXISTS sites (
    site_id text PRIMARY KEY,
    display_name text NOT NULL,
    timezone text NOT NULL DEFAULT 'UTC',
    enabled boolean NOT NULL DEFAULT true,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT sites_site_id_format CHECK (site_id ~ '^[a-zA-Z0-9][a-zA-Z0-9_-]{0,62}$'),
    CONSTRAINT sites_display_name_nonempty CHECK (length(btrim(display_name)) > 0)
);

CREATE TABLE IF NOT EXISTS devices (
    device_pk bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    site_id text NOT NULL REFERENCES sites(site_id) ON DELETE RESTRICT,
    device_id text NOT NULL,
    display_name text NOT NULL,
    model text NOT NULL DEFAULT '',
    mqtt_username text NOT NULL UNIQUE,
    credential_hash bytea NOT NULL,
    credential_version integer NOT NULL DEFAULT 1,
    enabled boolean NOT NULL DEFAULT true,
    last_seq bigint NOT NULL DEFAULT -1,
    sequence_gap_count bigint NOT NULL DEFAULT 0,
    last_seen_at timestamptz,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT devices_site_device_unique UNIQUE (site_id, device_id),
    CONSTRAINT devices_id_format CHECK (device_id ~ '^[a-zA-Z0-9][a-zA-Z0-9_-]{0,62}$'),
    CONSTRAINT devices_display_name_nonempty CHECK (length(btrim(display_name)) > 0),
    CONSTRAINT devices_last_seq_nonnegative CHECK (last_seq >= -1),
    CONSTRAINT devices_gap_count_nonnegative CHECK (sequence_gap_count >= 0),
    CONSTRAINT devices_credential_version_positive CHECK (credential_version > 0)
);

CREATE INDEX IF NOT EXISTS devices_site_enabled_idx
    ON devices (site_id, enabled, device_id);

CREATE INDEX IF NOT EXISTS devices_last_seen_idx
    ON devices (last_seen_at DESC NULLS LAST)
    WHERE enabled;
