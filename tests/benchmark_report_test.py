import unittest

from unittest.mock import patch

from tools.mqtt_device_sim import (build_report, load_database_watermarks,
                                   make_sample, verify_database_rows)


class BenchmarkReportTests(unittest.TestCase):
    def test_report_contains_environment_counters_latency_and_throughput(self):
        report = build_report(
            "bench-host", {"devices": 1000, "rate_hz": 1.0},
            {"sent": 100, "persisted": 90, "duplicate": 3, "rejected": 1,
             "retries": 4, "reconnects": 0, "worker_errors": 0,
             "enqueued": 100, "pending_at_end": 3, "dead_letter": 3},
            [10.0, 20.0, 30.0, 40.0], [], 1000.0, 1010.0)
        self.assertEqual(report["schema_version"], 2)
        self.assertEqual(report["host"], "bench-host")
        self.assertEqual(report["counters"]["persisted"], 90)
        self.assertEqual(report["throughput_per_sec"], 9.0)
        self.assertIn("p95", report["ack_latency_ms"])
        self.assertEqual(report["worker_errors"], [])
        self.assertIn("runtime", report)
        self.assertIn("cpu_process_seconds", report["resource_usage"])
        self.assertIn("peak_rss_bytes", report["resource_usage"])
        self.assertIn("outbox_bytes", report["resource_usage"])
        self.assertEqual(report["delivery_accounting"]["enqueued"], 100)
        self.assertEqual(report["delivery_accounting"]["terminal_acks"], 94)
        self.assertFalse(report["delivery_accounting"]["drained"])

    @patch("tools.mqtt_device_sim.subprocess.run")
    def test_database_verification_compares_rows_with_committed_ack_count(self, run):
        run.return_value.stdout = "16.4|2\n"
        result = verify_database_rows("compose.yaml", "plant-a",
                                      {"motor-1": (4, 5)}, expected_rows=2)
        self.assertTrue(result["verified"])
        self.assertEqual(result["persisted_rows"], 2)
        self.assertEqual(result["postgres_version"], "16.4")

    @patch("tools.mqtt_device_sim.subprocess.run")
    def test_database_verification_reports_mismatch(self, run):
        run.return_value.stdout = "16.4|1\n"
        result = verify_database_rows("compose.yaml", "plant-a",
                                      {"motor-1": (4, 5)}, expected_rows=2)
        self.assertFalse(result["verified"])
        self.assertEqual(result["persisted_rows"], 1)

    @patch("tools.mqtt_device_sim.subprocess.run")
    def test_loads_database_sequence_watermarks(self, run):
        run.return_value.stdout = "motor-1|23\nmotor-2|8\n"
        watermarks = load_database_watermarks("compose.yaml", "plant-a",
                                               ["motor-1", "motor-2"])
        self.assertEqual(watermarks, {"motor-1": 23, "motor-2": 8})

    def test_fault_profile_is_repeatable_and_changes_sensor_reading(self):
        sample_a = make_sample("plant-a", "motor-1", 10, 1000, 3, "sensor-spike")
        sample_b = make_sample("plant-a", "motor-1", 10, 1000, 3, "sensor-spike")
        self.assertEqual(sample_a, sample_b)
        self.assertEqual(sample_a["metrics"]["temperature_c"], 120.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
