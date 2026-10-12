from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FrontendBuildContractTests(unittest.TestCase):
    def test_vue_app_has_build_and_test_scripts(self):
        package = ROOT / "web" / "package.json"
        self.assertTrue(package.is_file(), "Vue/Vite frontend scaffold is missing")
        text = package.read_text(encoding="utf-8")
        for marker in ('"build"', '"test"', '"vue"', '"echarts"'):
            self.assertIn(marker, text)

    def test_web_monitor_uses_persisted_api_and_site_scoped_socket(self):
        app = ROOT / "web" / "src" / "App.vue"
        client = ROOT / "web" / "src" / "api" / "client.ts"
        device_detail = ROOT / "web" / "src" / "views" / "DeviceDetailView.vue"
        self.assertTrue(app.is_file())
        self.assertTrue(client.is_file())
        self.assertTrue(device_detail.is_file())
        self.assertIn("/api/v1/devices", client.read_text(encoding="utf-8"))
        self.assertIn("/api/v1/stats", client.read_text(encoding="utf-8"))
        self.assertIn("/ws/v1/monitor", device_detail.read_text(encoding="utf-8"))
        self.assertIn("site_id", device_detail.read_text(encoding="utf-8"))

    def test_compose_builds_frontend_and_matches_backend_environment(self):
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cc").read_text(encoding="utf-8")
        self.assertIn("web:", compose)
        self.assertIn("LABSCOPE_DB_HOST", compose)
        self.assertIn("LABSCOPE_MQTT_URI", compose)
        self.assertIn("LABSCOPE_HTTP_BIND", main)


if __name__ == "__main__":
    unittest.main(verbosity=2)
