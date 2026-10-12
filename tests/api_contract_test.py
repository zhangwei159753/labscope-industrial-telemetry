"""Static contracts for the database-backed device and telemetry API."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ApiContractTests(unittest.TestCase):
    def test_device_controller_exposes_bounded_registry_routes(self):
        header = ROOT / "src" / "controllers" / "DeviceController.h"
        source = ROOT / "src" / "controllers" / "DeviceController.cc"
        self.assertTrue(header.exists(), "DeviceController.h has not been implemented")
        self.assertTrue(source.exists(), "DeviceController.cc has not been implemented")
        text = source.read_text(encoding="utf-8")
        self.assertIn("/api/v1/devices", text)
        self.assertIn("site_id", text)
        self.assertIn("cursor", text)
        self.assertIn("limit", text)
        self.assertIn("execSqlAsync", text)

    def test_telemetry_controller_bounds_history_and_uses_persisted_rows(self):
        header = ROOT / "src" / "controllers" / "TelemetryController.h"
        source = ROOT / "src" / "controllers" / "TelemetryController.cc"
        self.assertTrue(header.exists(), "TelemetryController.h has not been implemented")
        self.assertTrue(source.exists(), "TelemetryController.cc has not been implemented")
        text = source.read_text(encoding="utf-8")
        for route in ("/latest", "/history", "/api/v1/stats"):
            self.assertIn(route, text)
        self.assertIn("telemetry_samples", text)
        self.assertIn("from_ms", text)
        self.assertIn("to_ms", text)
        self.assertIn("1000", text)
        self.assertIn("24", text)

    def test_database_api_is_optional_for_existing_local_monitor(self):
        source = (ROOT / "src" / "main.cc").read_text(encoding="utf-8")
        self.assertIn("telemetryRuntime", source)
        self.assertIn("registerRoutes", source)
        self.assertIn("telemetryHub().stats()", source)

    def test_monitor_cache_receives_only_committed_site_scoped_samples(self):
        main = (ROOT / "src" / "main.cc").read_text(encoding="utf-8")
        hub = (ROOT / "src" / "telemetry_hub.cc").read_text(encoding="utf-8")
        writer = (ROOT / "src" / "storage" / "TelemetryWriter.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("publishCommitted", main)
        self.assertIn('const std::string key = siteId + "/" + deviceId;', hub)
        self.assertIn("catch (...)\n                        {\n                        }", writer)
        self.assertLess(writer.index("committed((*envelopes)[index])"),
                        writer.index("completion(std::move(results))"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
