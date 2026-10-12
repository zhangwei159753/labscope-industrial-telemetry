ALTER TABLE devices
    ADD COLUMN IF NOT EXISTS next_source_seq bigint NOT NULL DEFAULT 0;

ALTER TABLE devices
    DROP CONSTRAINT IF EXISTS devices_next_source_seq_nonnegative;

ALTER TABLE devices
    ADD CONSTRAINT devices_next_source_seq_nonnegative
    CHECK (next_source_seq >= 0);
