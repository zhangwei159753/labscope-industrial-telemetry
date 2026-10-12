"""Schema and partition lifecycle contracts for PostgreSQL migrations."""

from pathlib import Path
from datetime import datetime, timedelta, timezone
import os
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MigrationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v001 = (ROOT / "migrations" / "V001__identity_and_devices.sql").read_text(
            encoding="utf-8"
        )
        cls.v002 = (ROOT / "migrations" / "V002__telemetry_and_aggregates.sql").read_text(
            encoding="utf-8"
        )

    def test_identity_schema_tracks_device_watermarks_and_credentials(self):
        self.assertRegex(self.v001, r"(?is)CREATE TABLE IF NOT EXISTS sites\s*\(")
        self.assertRegex(self.v001, r"(?is)CREATE TABLE IF NOT EXISTS devices\s*\(")
        for column in ("credential_hash", "last_seq", "sequence_gap_count", "last_seen_at"):
            with self.subTest(column=column):
                self.assertIn(column, self.v001)
        self.assertRegex(self.v001, r"(?is)UNIQUE\s*\(\s*site_id\s*,\s*device_id\s*\)")

    def test_telemetry_is_partitioned_and_idempotently_indexed(self):
        self.assertRegex(
            self.v002,
            r"(?is)CREATE TABLE IF NOT EXISTS telemetry_samples.*?PARTITION BY RANGE\s*\(\s*received_at\s*\)",
        )
        self.assertRegex(
            self.v002,
            r"(?is)PRIMARY KEY\s*\(\s*site_id\s*,\s*device_id\s*,\s*received_at\s*,\s*seq\s*\)",
        )
        self.assertRegex(self.v002, r"(?is)CREATE INDEX IF NOT EXISTS .*telemetry_samples")

    def test_aggregates_and_alarm_outbox_have_stable_deduplication_keys(self):
        self.assertIn("telemetry_1m", self.v002)
        for column in ("bucket_start", "metric_name", "min_value", "max_value", "sum_value", "sample_count"):
            with self.subTest(column=column):
                self.assertIn(column, self.v002)
        self.assertIn("telemetry_outbox", self.v002)
        self.assertRegex(self.v002, r"(?is)UNIQUE\s*\(\s*site_id\s*,\s*device_id\s*,\s*received_at\s*,\s*seq\s*,\s*event_type\s*\)")

    def test_partition_lifecycle_covers_future_partitions_and_retention(self):
        lifecycle = self.v002.lower()
        for marker in ("labscope_ensure_telemetry_partitions", "labscope_maintain_telemetry", "interval '24 hours'", "interval '30 days'"):
            with self.subTest(marker=marker):
                self.assertIn(marker, lifecycle)
        maintenance = ROOT / "src" / "storage" / "PartitionMaintenance.cc"
        self.assertTrue(maintenance.is_file(), "startup partition maintenance helper is missing")
        implementation = maintenance.read_text(encoding="utf-8")
        self.assertIn("labscope_ensure_telemetry_partitions", implementation)
        self.assertIn("labscope_maintain_telemetry", implementation)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("src/storage/PartitionMaintenance.cc", cmake)

    @unittest.skipUnless(
        os.environ.get("LABSCOPE_RUN_DB_TESTS") == "1" and shutil.which("docker"),
        "set LABSCOPE_RUN_DB_TESTS=1 and start the Compose PostgreSQL service",
    )
    def test_flyway_migrations_and_partition_functions_are_repeatable(self):
        def compose(*args):
            result = subprocess.run(
                ["docker", "compose", *args],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout.strip()

        compose("run", "--rm", "migrate")
        compose("run", "--rm", "migrate")

        db_user = os.environ.get("POSTGRES_USER", "labscope")
        db_name = os.environ.get("POSTGRES_DB", "labscope")
        query = (
            "SELECT string_agg(table_name, ',' ORDER BY table_name) "
            "FROM information_schema.tables WHERE table_schema='public' "
            "AND table_name IN ('sites','devices','telemetry_samples','telemetry_1m',"
            "'telemetry_outbox','telemetry_partition_catalog')"
        )
        tables = compose(
            "exec", "-T", "db", "psql", "-XAt", "-U", db_user, "-d", db_name, "-c", query
        )
        self.assertEqual(
            tables,
            "devices,sites,telemetry_1m,telemetry_outbox,telemetry_partition_catalog,telemetry_samples",
        )

        compose(
            "exec", "-T", "db", "psql", "-XAt", "-U", db_user, "-d", db_name,
            "-c", "SELECT labscope_ensure_telemetry_partitions(); SELECT labscope_ensure_telemetry_partitions();",
        )
        today = datetime.now(timezone.utc).date()
        names = [
            f"telemetry_samples_{today:%Y%m%d}",
            f"telemetry_samples_{today + timedelta(days=1):%Y%m%d}",
        ]
        partition_query = (
            "SELECT string_agg(child.relname, ',' ORDER BY child.relname) "
            "FROM pg_inherits i JOIN pg_class parent ON parent.oid=i.inhparent "
            "JOIN pg_class child ON child.oid=i.inhrelid "
            "WHERE parent.relname='telemetry_samples' AND child.relname IN ('"
            + "','".join(names)
            + "')"
        )
        partitions = compose(
            "exec", "-T", "db", "psql", "-XAt", "-U", db_user, "-d", db_name,
            "-c", partition_query,
        )
        self.assertEqual(partitions, ",".join(sorted(names)))


if __name__ == "__main__":
    unittest.main()
