CREATE TABLE IF NOT EXISTS audit_log (
    audit_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id bigint REFERENCES web_users(user_id) ON DELETE RESTRICT,
    action text NOT NULL,
    entity_type text NOT NULL,
    entity_id text NOT NULL,
    site_id text,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT audit_action_nonempty CHECK (length(btrim(action)) BETWEEN 1 AND 96),
    CONSTRAINT audit_entity_type_nonempty CHECK (length(btrim(entity_type)) BETWEEN 1 AND 64),
    CONSTRAINT audit_metadata_object CHECK (jsonb_typeof(metadata) = 'object')
);

CREATE INDEX IF NOT EXISTS audit_log_site_time_idx
    ON audit_log (site_id, created_at DESC, audit_id DESC);
CREATE INDEX IF NOT EXISTS audit_log_entity_idx
    ON audit_log (entity_type, entity_id, created_at DESC);

CREATE OR REPLACE FUNCTION labscope_reject_audit_mutation()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'audit_log is append-only';
END;
$$;

DROP TRIGGER IF EXISTS audit_log_immutable ON audit_log;
CREATE TRIGGER audit_log_immutable
    BEFORE UPDATE OR DELETE ON audit_log
    FOR EACH ROW EXECUTE FUNCTION labscope_reject_audit_mutation();
