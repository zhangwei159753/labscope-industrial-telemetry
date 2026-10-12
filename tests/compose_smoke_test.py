"""Static Compose and operations-document checks; does not claim Docker runtime coverage."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ComposeSmokeContractTests(unittest.TestCase):
    def test_stack_orders_migrations_before_app_and_proxy_after_health(self):
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        self.assertIn("condition: service_completed_successfully", compose)
        self.assertIn("condition: service_healthy", compose)
        self.assertIn("dockerfile: docker/app.Dockerfile", compose)
        self.assertIn("dockerfile: docker/web.Dockerfile", compose)
        self.assertIn('127.0.0.1:${LABSCOPE_HTTP_PORT:-8080}:80', compose)
        self.assertNotIn('127.0.0.1:${LABSCOPE_HTTP_PORT:-8080}:8080', compose)

    def test_stateful_services_have_persistent_volumes_and_health_checks(self):
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        for volume in ("app_data", "db_data", "mqtt_data", "mqtt_log"):
            self.assertIn(f"{volume}:", compose)
        self.assertGreaterEqual(compose.count("healthcheck:"), 3)

    def test_reverse_proxy_supports_api_websocket_and_frontend_routes(self):
        config = (ROOT / "docker" / "nginx.conf").read_text(encoding="utf-8")
        self.assertIn("location /api/", config)
        self.assertIn("location /ws/", config)
        self.assertIn("proxy_set_header Upgrade", config)
        self.assertIn("try_files $uri $uri/ /index.html", config)

    def test_images_run_app_non_root_and_dev_secrets_are_documented(self):
        app_image = (ROOT / "docker" / "app.Dockerfile").read_text(encoding="utf-8")
        compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("USER 10001:10001", app_image)
        self.assertIn("change-me", compose)
        self.assertIn("不能直接暴露到公网或生产 OT 网络", readme)

    def test_repository_has_only_a_placeholder_environment_template(self):
        self.assertFalse((ROOT / ".env").exists(), "local deployment secrets must not be committed")
        example = ROOT / ".env.example"
        self.assertTrue(example.is_file(), "provide a local configuration template")
        contents = example.read_text(encoding="utf-8")
        self.assertIn("change-this-local-development-password", contents)
        self.assertIn("change-this-local-development-admin-password", contents)

    def test_backup_restore_procedures_are_documented(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("pg_dump", readme)
        self.assertIn("pg_restore", readme)
        self.assertIn("docker compose down --volumes", readme)


if __name__ == "__main__":
    unittest.main(verbosity=2)
