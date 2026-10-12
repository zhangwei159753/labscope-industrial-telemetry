"""Static route wiring checks plus optional Compose authorization scenarios."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read_source(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required authorization source is missing: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


class LiveMonitorAuthorizationTests(unittest.TestCase):
    def test_database_read_routes_require_a_session_and_site_scope(self):
        for name in ("DeviceController.cc", "TelemetryController.cc"):
            source = read_source(ROOT / "src" / "controllers" / name)
            self.assertIn("SessionFilter::classTypeName()", source, name)
            self.assertIn("SiteAccessFilter::classTypeName()", source, name)

    def test_monitor_websocket_uses_authenticated_versioned_route(self):
        main = read_source(ROOT / "src" / "main.cc")
        source = read_source(ROOT / "src" / "controllers" / "LiveMonitorSocket.cc")
        header = read_source(ROOT / "src" / "controllers" / "LiveMonitorSocket.h")
        self.assertNotIn('WS_PATH_ADD("/ws/monitor"', main)
        self.assertIn('WS_PATH_ADD("/ws/v1/monitor"', header)
        self.assertIn("getCookie", source)
        self.assertIn("sessions_->resolve", source)
        self.assertIn("web_user_sites", source)

    def test_site_scope_is_bound_to_user_and_unknown_roles_are_denied(self):
        migration = read_source(ROOT / "migrations" / "V005__web_auth_sessions.sql")
        source = read_source(ROOT / "src" / "auth" / "SiteAccessFilter.cc")
        self.assertIn("web_user_sites", migration)
        self.assertIn("user_id = $1", source)
        self.assertIn("k403Forbidden", source)

    def test_site_directory_is_session_protected_and_limits_sites_to_assignments(self):
        source = read_source(ROOT / "src" / "controllers" / "SiteController.cc")
        self.assertIn('"/api/v1/sites"', source)
        self.assertIn("SessionFilter::classTypeName()", source)
        self.assertIn("web_user_sites", source)
        self.assertIn("context->userId", source)
        self.assertIn("Role::Admin", source)

    def test_site_overview_and_browser_monitor_use_authorized_routes(self):
        telemetry = read_source(ROOT / "src" / "controllers" / "TelemetryController.cc")
        frontend = read_source(ROOT / "web" / "src" / "views" / "DeviceDetailView.vue")
        router = read_source(ROOT / "web" / "src" / "router.ts")
        self.assertIn('"/api/v1/overview"', telemetry)
        self.assertIn("SiteAccessFilter::classTypeName()", telemetry)
        self.assertIn("/ws/v1/monitor", frontend)
        self.assertIn("api.me()", router)
        self.assertIn("/login", router)


if __name__ == "__main__":
    unittest.main(verbosity=2)
