"""End-to-end contract tests against the compiled Drogon service."""

import json
import os
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

import websocket


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "build" / "Release" / "lab_telemetry.exe"


class TelemetryIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get("LAB_SERVER_BIN", DEFAULT_BINARY))
        if not cls.binary.is_file():
            raise RuntimeError(f"Build lab_telemetry first: {cls.binary}")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            cls.port = probe.getsockname()[1]
        cls.base = f"http://127.0.0.1:{cls.port}"
        cls.wsbase = f"ws://127.0.0.1:{cls.port}"
        cls.server = subprocess.Popen(
            [str(cls.binary), str(cls.port)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        for _ in range(60):
            if cls.server.poll() is not None:
                raise RuntimeError("Server exited during startup: " + cls.server.stdout.read())
            try:
                with urllib.request.urlopen(cls.base + "/healthz", timeout=0.2) as response:
                    if response.status == 200:
                        return
            except (urllib.error.URLError, TimeoutError):
                time.sleep(0.1)
        cls.server.terminate()
        cls.server.wait(timeout=3)
        raise RuntimeError("Server did not start on the requested port")

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "server"):
            cls.server.terminate()
            try:
                cls.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait(timeout=3)

    def connect(self, route, device):
        client = websocket.create_connection(
            self.wsbase + route + "?device_id=" + device,
            timeout=2,
            http_proxy_host=None,
        )
        self.addCleanup(client.close)
        return client

    def receive_type(self, client, wanted):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            event = json.loads(client.recv())
            if event.get("type") == wanted:
                return event
        self.fail(f"No {wanted} event received")

    def sample(self, seq, value=22.4):
        return {"seq": seq, "timestamp_ms": time.time_ns() // 1_000_000,
                "metrics": {"temperature_c": value}}

    def get_json(self, path):
        with urllib.request.urlopen(self.base + path, timeout=2) as response:
            return json.load(response)

    @unittest.skip("authenticated /ws/v1/monitor requires a PostgreSQL session; covered by the Compose smoke suite")
    def test_same_device_reaches_two_monitors(self):
        monitors = [self.connect("/ws/monitor", "scope-two") for _ in range(2)]
        for monitor in monitors:
            self.receive_type(monitor, "subscribed")
        device = self.connect("/ws/ingest", "scope-two")
        self.receive_type(device, "ready")
        device.send(json.dumps(self.sample(1)))
        for monitor in monitors:
            event = self.receive_type(monitor, "telemetry")
            self.assertEqual((event["device_id"], event["seq"]), ("scope-two", 1))
        self.assertEqual(self.get_json("/api/v1/devices/scope-two/latest")["seq"], 1)

    def test_duplicate_sequence_is_rejected_without_overwriting_latest(self):
        device = self.connect("/ws/ingest", "scope-duplicate")
        self.receive_type(device, "ready")
        device.send(json.dumps(self.sample(5, 23.1)))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                if self.get_json("/api/v1/devices/scope-duplicate/latest")["seq"] == 5:
                    break
            except urllib.error.HTTPError:
                pass
            time.sleep(0.02)
        device.send(json.dumps(self.sample(5, 99.0)))
        rejected = self.receive_type(device, "rejected")
        self.assertIn("seq", rejected["reason"])
        latest = self.get_json("/api/v1/devices/scope-duplicate/latest")
        self.assertEqual(latest["metrics"]["temperature_c"], 23.1)

    @unittest.skip("site/device authorization requires the database-backed monitor controller")
    def test_device_topics_are_isolated(self):
        monitor_a = self.connect("/ws/monitor", "scope-A")
        monitor_b = self.connect("/ws/monitor", "scope-B")
        self.receive_type(monitor_a, "subscribed")
        self.receive_type(monitor_b, "subscribed")
        device_a = self.connect("/ws/ingest", "scope-A")
        device_b = self.connect("/ws/ingest", "scope-B")
        self.receive_type(device_a, "ready")
        self.receive_type(device_b, "ready")
        device_a.send(json.dumps(self.sample(1, 11)))
        device_b.send(json.dumps(self.sample(1, 22)))
        self.assertEqual(self.receive_type(monitor_a, "telemetry")["metrics"]["temperature_c"], 11)
        self.assertEqual(self.receive_type(monitor_b, "telemetry")["metrics"]["temperature_c"], 22)

    def test_bad_json_is_rejected_and_service_recovers(self):
        device = self.connect("/ws/ingest", "scope-bad")
        self.receive_type(device, "ready")
        device.send("not-json")
        self.receive_type(device, "rejected")
        device.send(json.dumps(self.sample(1)))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                if self.get_json("/api/v1/devices/scope-bad/latest")["seq"] == 1:
                    return
            except urllib.error.HTTPError:
                pass
            time.sleep(0.02)
        self.fail("Service did not accept a valid sample after malformed JSON")

    def test_non_numeric_metric_is_rejected(self):
        device = self.connect("/ws/ingest", "scope-invalid-metric")
        self.receive_type(device, "ready")
        sample = self.sample(1)
        sample["metrics"]["temperature_c"] = "hot"
        device.send(json.dumps(sample))
        rejected = self.receive_type(device, "rejected")
        self.assertIn("metrics", rejected["reason"])
        with self.assertRaises(urllib.error.HTTPError) as missing:
            self.get_json("/api/v1/devices/scope-invalid-metric/latest")
        self.assertEqual(missing.exception.code, 404)

    @unittest.skip("authenticated monitor lifecycle requires the database-backed runtime")
    def test_disconnected_monitor_is_removed_from_stats(self):
        before = self.get_json("/api/v1/stats")["monitors"]
        monitor = self.connect("/ws/monitor", "scope-close")
        self.receive_type(monitor, "subscribed")
        self.assertEqual(self.get_json("/api/v1/stats")["monitors"], before + 1)
        monitor.close()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self.get_json("/api/v1/stats")["monitors"] == before:
                return
            time.sleep(0.02)
        self.fail("Monitor remained registered after close")

    def test_history_paginates_in_sequence_order_and_isolates_devices(self):
        device = self.connect("/ws/ingest", "scope-history")
        self.receive_type(device, "ready")
        other = self.connect("/ws/ingest", "scope-history-other")
        self.receive_type(other, "ready")
        for seq in (2, 4, 6):
            device.send(json.dumps(self.sample(seq, seq)))
        other.send(json.dumps(self.sample(1, 99)))
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                if self.get_json("/api/v1/devices/scope-history/latest")["seq"] == 6:
                    break
            except urllib.error.HTTPError:
                pass
            time.sleep(0.02)
        first = self.get_json("/api/v1/devices/scope-history/history?limit=2")
        self.assertEqual([row["seq"] for row in first["samples"]], [2, 4])
        self.assertEqual(first["next_after_seq"], 4)
        second = self.get_json(
            "/api/v1/devices/scope-history/history?after_seq=4&limit=2"
        )
        self.assertEqual([row["seq"] for row in second["samples"]], [6])
        self.assertEqual(second["next_after_seq"], 6)
        self.assertTrue(all(row["device_id"] == "scope-history" for row in second["samples"]))

    def test_history_rejects_invalid_limit_and_cursor(self):
        for query in ("limit=0", "limit=101", "limit=oops", "limit=",
                      "after_seq=-2", "after_seq=abc", "after_seq="):
            with self.subTest(query=query):
                with self.assertRaises(urllib.error.HTTPError) as error:
                    self.get_json("/api/v1/devices/scope-history/history?" + query)
                self.assertEqual(error.exception.code, 400)

    def test_prometheus_endpoint_reports_low_cardinality_runtime_metrics(self):
        with urllib.request.urlopen(self.base + "/metrics", timeout=2) as response:
            self.assertEqual(response.status, 200)
            self.assertIn("text/plain", response.headers.get("Content-Type", ""))
            metrics = response.read().decode("utf-8")
        for name in ("labscope_telemetry_received_total", "labscope_ingestion_queue_depth",
                     "labscope_mqtt_connected", "labscope_websocket_clients"):
            self.assertIn(name, metrics)
        self.assertNotIn("device_id=", metrics)


if __name__ == "__main__":
    unittest.main()
