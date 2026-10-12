"""Persistence contract and optional PostgreSQL integration checks."""

from __future__ import annotations

import os
import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class TelemetryPersistenceContractTests(unittest.TestCase):
    def test_repository_exposes_async_transactional_batch_api(self):
        header = ROOT / "src" / "storage" / "TelemetryRepository.h"
        source = ROOT / "src" / "storage" / "TelemetryRepository.cc"
        self.assertTrue(header.exists(), "TelemetryRepository.h has not been implemented")
        self.assertTrue(source.exists(), "TelemetryRepository.cc has not been implemented")
        text = header.read_text(encoding="utf-8")
        self.assertIn("insertBatch", text)
        self.assertIn("BatchCompletionCallback", text)
        self.assertIn("DbClientPtr", text)
        implementation = source.read_text(encoding="utf-8")
        self.assertIn("newTransactionAsync", implementation)
        query = implementation.split("constexpr char kPersistSql[] = R\"SQL(", 1)[1].split(")SQL\";", 1)[0]
        self.assertNotIn("?", query, "PostgreSQL SQL must use numbered $n parameters")
        for parameter in range(1, 8):
            self.assertIn(f"${parameter}", query)
        self.assertIn("telemetry_samples", implementation)
        self.assertIn("telemetry_1m", implementation)
        self.assertIn("telemetry_outbox", implementation)
        self.assertIn("setCommitCallback", implementation)
        self.assertIn("sequence_gap_count", implementation)
        self.assertIn("next_source_seq", implementation)
        self.assertIn("RetryableError", implementation)

    def test_source_sequence_has_a_dedicated_allocator(self):
        header = ROOT / "src" / "storage" / "DeviceRepository.h"
        source = ROOT / "src" / "storage" / "DeviceRepository.cc"
        migration = ROOT / "migrations" / "V003__source_sequence.sql"
        self.assertTrue(header.exists(), "DeviceRepository.h has not been implemented")
        self.assertTrue(source.exists(), "DeviceRepository.cc has not been implemented")
        self.assertTrue(migration.exists(), "source sequence migration is missing")
        self.assertIn("reserveSourceSequence", header.read_text(encoding="utf-8"))
        self.assertIn("next_source_seq", migration.read_text(encoding="utf-8"))

    @unittest.skipUnless(
        os.environ.get("LABSCOPE_RUN_DB_TESTS") == "1",
        "set LABSCOPE_RUN_DB_TESTS=1 when Docker Compose PostgreSQL is available",
    )
    def test_postgres_commit_replay_and_gap_scenarios(self):
        binary = os.environ.get("LABSCOPE_PERSISTENCE_TEST_BINARY")
        if not binary:
            candidates = (
                ROOT / "build" / "Debug" / "telemetry_persistence_integration_test.exe",
                ROOT / "build" / "telemetry_persistence_integration_test",
            )
            binary = next((str(path) for path in candidates if path.is_file()), "")
        self.assertTrue(binary, "build telemetry_persistence_integration_test first")
        self.assertTrue(
            os.environ.get("LABSCOPE_TEST_DB_CONNINFO"),
            "LABSCOPE_TEST_DB_CONNINFO must point at a migrated test database",
        )
        executable = pathlib.Path(binary)
        self.assertTrue(executable.is_file(), f"integration test binary not found: {executable}")
        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=90,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
