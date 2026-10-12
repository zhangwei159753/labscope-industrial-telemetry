"""Static build and service contracts for the industrial telemetry app."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BuildContractTest(unittest.TestCase):
    def test_drogon_orm_and_postgresql_are_enabled(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r"set\(BUILD_ORM\s+ON\s+CACHE BOOL")
        self.assertRegex(cmake, r"set\(BUILD_POSTGRESQL\s+ON\s+CACHE BOOL")

    def test_paho_async_tls_target_is_pinned_and_linked(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, r"GIT_TAG\s+v1\.3\.16")
        self.assertIn("PAHO_WITH_SSL", cmake)
        self.assertIn("eclipse-paho-mqtt-c::paho-mqtt3as", cmake)
        self.assertIn("eclipse-paho-mqtt-c::paho-mqtt3a", cmake)

    def test_compose_has_durable_database_and_broker_services(self):
        compose_path = ROOT / "compose.yaml"
        self.assertTrue(compose_path.is_file(), "compose.yaml is missing")
        compose = compose_path.read_text(encoding="utf-8")
        for service in ("mqtt:", "db:"):
            with self.subTest(service=service):
                self.assertRegex(compose, rf"(?m)^\s{{2}}{re.escape(service)}")
        self.assertRegex(compose, r"(?m)^\s{2}db_data:")
        self.assertRegex(compose, r"(?m)^\s{2}mqtt_data:")

    def test_postgresql_target_is_compatible_with_vcpkg_configurations(self):
        find_pg = ROOT / "cmake" / "Findpg.cmake"
        self.assertTrue(find_pg.is_file(), "project PostgreSQL compatibility module is missing")
        module = find_pg.read_text(encoding="utf-8")
        self.assertIn("PostgreSQL::PostgreSQL", module)
        self.assertNotIn('INTERFACE_LINK_LIBRARIES "${PostgreSQL_LIBRARIES}"', module)

    def test_ignore_rules_exclude_secrets_without_hiding_benchmark_evidence(self):
        ignore_rules = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertIn(".env", ignore_rules)
        self.assertNotIn("docs/*.json", ignore_rules)


if __name__ == "__main__":
    unittest.main()
