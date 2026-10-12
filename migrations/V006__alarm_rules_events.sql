CREATE TABLE IF NOT EXISTS alarm_rules (
    rule_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    site_id text NOT NULL,
    device_id text NOT NULL,
    metric_name text NOT NULL,
    direction text NOT NULL,
    threshold double precision NOT NULL,
    duration_ms bigint NOT NULL DEFAULT 0,
    hysteresis double precision NOT NULL DEFAULT 0,
    cooldown_ms bigint NOT NULL DEFAULT 0,
    severity text NOT NULL DEFAULT 'warning',
    enabled boolean NOT NULL DEFAULT true,
    created_by bigint REFERENCES web_users(user_id) ON DELETE SET NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT alarm_rules_device_fk FOREIGN KEY (site_id, device_id)
        REFERENCES devices(site_id, device_id) ON DELETE RESTRICT,
    CONSTRAINT alarm_rules_metric_nonempty CHECK (length(metric_name) BETWEEN 1 AND 128),
    CONSTRAINT alarm_rules_direction CHECK (direction IN ('above', 'below')),
    CONSTRAINT alarm_rules_threshold_finite CHECK (threshold NOT IN ('Infinity'::float8, '-Infinity'::float8, 'NaN'::float8)),
    CONSTRAINT alarm_rules_duration_nonnegative CHECK (duration_ms BETWEEN 0 AND 86400000),
    CONSTRAINT alarm_rules_hysteresis_nonnegative CHECK (hysteresis >= 0 AND hysteresis NOT IN ('Infinity'::float8, 'NaN'::float8)),
    CONSTRAINT alarm_rules_cooldown_nonnegative CHECK (cooldown_ms BETWEEN 0 AND 604800000),
    CONSTRAINT alarm_rules_severity CHECK (severity IN ('info', 'warning', 'critical'))
);

CREATE INDEX IF NOT EXISTS alarm_rules_device_enabled_idx
    ON alarm_rules (site_id, device_id, enabled, rule_id);

CREATE TABLE IF NOT EXISTS alarm_events (
    event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    rule_id bigint NOT NULL REFERENCES alarm_rules(rule_id) ON DELETE RESTRICT,
    site_id text NOT NULL,
    device_id text NOT NULL,
    metric_name text NOT NULL,
    severity text NOT NULL,
    state text NOT NULL DEFAULT 'active',
    trigger_seq bigint NOT NULL,
    trigger_value double precision NOT NULL,
    triggered_at timestamptz NOT NULL,
    acknowledged_by bigint REFERENCES web_users(user_id) ON DELETE RESTRICT,
    acknowledged_at timestamptz,
    acknowledgment_remark text,
    recovered_at timestamptz,
    CONSTRAINT alarm_events_rule_seq_unique UNIQUE (rule_id, trigger_seq),
    CONSTRAINT alarm_events_severity CHECK (severity IN ('info', 'warning', 'critical')),
    CONSTRAINT alarm_events_state CHECK (state IN ('active', 'acknowledged', 'recovered')),
    CONSTRAINT alarm_events_ack_consistency CHECK (
        (acknowledged_by IS NULL AND acknowledged_at IS NULL) OR
        (acknowledged_by IS NOT NULL AND acknowledged_at IS NOT NULL)),
    CONSTRAINT alarm_events_recovery_consistency CHECK (
        (state <> 'recovered' AND recovered_at IS NULL) OR
        (state = 'recovered' AND recovered_at IS NOT NULL))
);

CREATE INDEX IF NOT EXISTS alarm_events_site_state_time_idx
    ON alarm_events (site_id, state, triggered_at DESC, event_id DESC);
CREATE INDEX IF NOT EXISTS alarm_events_device_time_idx
    ON alarm_events (site_id, device_id, triggered_at DESC, event_id DESC);

CREATE TABLE IF NOT EXISTS alarm_rule_state (
    rule_id bigint PRIMARY KEY REFERENCES alarm_rules(rule_id) ON DELETE CASCADE,
    last_seq bigint NOT NULL DEFAULT -1,
    threshold_since_ms bigint,
    cooldown_until_ms bigint NOT NULL DEFAULT 0,
    active_event_id bigint REFERENCES alarm_events(event_id) ON DELETE RESTRICT,
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT alarm_rule_state_seq CHECK (last_seq >= -1)
);
