"""Authentication contracts plus optional live HTTP tests for the Compose stack."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read_source(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required authentication source is missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


class AuthenticationContractTests(unittest.TestCase):
    def test_auth_schema_has_users_roles_and_revocable_expiring_sessions(self):
        migration = ROOT / "migrations" / "V005__web_auth_sessions.sql"
        self.assertTrue(migration.is_file(), "web authentication migration is missing")
        sql = migration.read_text(encoding="utf-8").lower()
        for marker in ("web_users", "password_hash", "admin", "operator", "viewer",
                       "web_sessions", "session_hash", "csrf_token", "expires_at", "revoked_at"):
            self.assertIn(marker, sql)

    def test_password_hashing_uses_argon2id_and_never_stores_plain_password(self):
        source = read_source(ROOT / "src" / "auth" / "PasswordHasher.cc")
        self.assertIn("crypto_pwhash_ALG_ARGON2ID13", source)
        self.assertIn("crypto_pwhash_str_verify", source)

    def test_short_demo_password_policy_is_limited_to_development_environment(self):
        hasher = read_source(ROOT / "src" / "auth" / "PasswordHasher.h")
        controller = read_source(ROOT / "src" / "controllers" / "AuthController.cc")
        compose = read_source(ROOT / "compose.yaml")
        self.assertIn("minimumPasswordLengthForEnvironment", hasher)
        self.assertIn('"development"', controller)
        self.assertIn("minimumPasswordLengthForEnvironment", controller)
        self.assertIn("LABSCOPE_ENV", compose)

    def test_auth_controller_has_login_logout_and_me_routes_with_secure_cookie_policy(self):
        source = read_source(ROOT / "src" / "controllers" / "AuthController.cc")
        for marker in ("/api/v1/auth/login", "/api/v1/auth/logout", "/api/v1/auth/me",
                       "setHttpOnly(true)", "setSameSite", "setSecure"):
            self.assertIn(marker, source)
        filter_source = read_source(ROOT / "src" / "auth" / "SessionFilter.cc")
        self.assertIn("X-CSRF-Token", filter_source)
        self.assertIn("k403Forbidden", filter_source)

    def test_role_checks_fail_closed_when_session_is_missing(self):
        source = read_source(ROOT / "src" / "auth" / "Authorization.cc")
        self.assertIn("requireRole", source)
        self.assertIn("labscope.auth", source)
        self.assertIn("context.userId > 0", source)
        self.assertIn("return user &&", source)

    def test_admin_setup_is_interactive_and_never_accepts_password_as_an_argument(self):
        script = read_source(ROOT / "tools" / "create_admin.py")
        self.assertIn("getpass.getpass", script)
        self.assertNotIn("--password", script)
        self.assertIn("--create-admin", script)

    def test_operator_and_viewer_provisioning_requires_explicit_site_grants(self):
        script = read_source(ROOT / "tools" / "create_web_user.py")
        main = read_source(ROOT / "src" / "main.cc")
        self.assertIn("getpass.getpass", script)
        self.assertIn("--create-user", script)
        self.assertIn("required=True", script)
        self.assertIn("web_user_sites", main)
        self.assertIn("web_user_site.grant", main)
        self.assertIn("'web_user.create'", main)


if __name__ == "__main__":
    unittest.main(verbosity=2)
