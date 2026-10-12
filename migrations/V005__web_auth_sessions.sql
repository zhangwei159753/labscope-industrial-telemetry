CREATE TABLE IF NOT EXISTS web_users (
    user_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username text NOT NULL UNIQUE,
    password_hash text NOT NULL,
    role text NOT NULL DEFAULT 'viewer',
    enabled boolean NOT NULL DEFAULT true,
    failed_login_count integer NOT NULL DEFAULT 0,
    locked_until timestamptz,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CONSTRAINT web_users_username_format CHECK (username ~ '^[A-Za-z0-9][A-Za-z0-9_.@-]{2,63}$'),
    CONSTRAINT web_users_password_hash_nonempty CHECK (length(password_hash) > 20),
    CONSTRAINT web_users_role_allowed CHECK (role IN ('admin', 'operator', 'viewer')),
    CONSTRAINT web_users_failed_login_nonnegative CHECK (failed_login_count >= 0)
);

CREATE TABLE IF NOT EXISTS web_sessions (
    session_hash char(64) PRIMARY KEY,
    user_id bigint NOT NULL REFERENCES web_users(user_id) ON DELETE CASCADE,
    csrf_token char(64) NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    expires_at timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    revoked_at timestamptz,
    CONSTRAINT web_sessions_expiry_after_creation CHECK (expires_at > created_at)
);

CREATE INDEX IF NOT EXISTS web_sessions_user_expiry_idx
    ON web_sessions (user_id, expires_at DESC);
CREATE INDEX IF NOT EXISTS web_sessions_expiry_idx
    ON web_sessions (expires_at)
    WHERE revoked_at IS NULL;

CREATE TABLE IF NOT EXISTS web_user_sites (
    user_id bigint NOT NULL REFERENCES web_users(user_id) ON DELETE CASCADE,
    site_id text NOT NULL REFERENCES sites(site_id) ON DELETE CASCADE,
    granted_by bigint REFERENCES web_users(user_id) ON DELETE SET NULL,
    can_manage_devices boolean NOT NULL DEFAULT false,
    can_manage_alarm_rules boolean NOT NULL DEFAULT false,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (user_id, site_id)
);

CREATE INDEX IF NOT EXISTS web_user_sites_site_idx ON web_user_sites (site_id, user_id);
