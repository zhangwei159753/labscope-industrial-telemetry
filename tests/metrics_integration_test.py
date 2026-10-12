"""Static contracts for operational metrics and dependency health."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file():
        raise AssertionError(f"required metrics implementation missing: {relative}")
    return path.read_text(encoding="utf-8")


class MetricsIntegrationContractTests(unittest.TestCase):
    def test_prometheus_endpoint_exposes_low_cardinality_runtime_signals(self):
        controller = source("src/controllers/MetricsController.cc")
        for metric in ("labscope_telemetry_received_total", "labscope_telemetry_persisted_total",
                       "labscope_telemetry_duplicate_total", "labscope_telemetry_rejected_total",
                       "labscope_ingestion_queue_depth", "labscope_mqtt_connected",
                       "labscope_websocket_clients", "labscope_device_sequence_gaps_total",
                       "labscope_alarm_events_active"):
            self.assertIn(metric, controller)
        self.assertIn('"/metrics"', controller)
        self.assertNotIn("device_id=", controller)
        self.assertNotIn("site_id=", controller)

    def test_system_health_checks_database_and_reports_real_dependencies(self):
        controller = source("src/controllers/MetricsController.cc")
        self.assertIn('"/api/v1/system/health"', controller)
        self.assertIn('"SELECT 1"', controller)
        self.assertIn('"database"', controller)
        self.assertIn('"mqtt"', controller)
        self.assertIn('"alarm_processor"', controller)
        self.assertIn('"ingestion_queue_depth"', controller)
        self.assertIn('SessionFilter::classTypeName()', controller)

    def test_metrics_track_database_outcomes_and_broker_transitions(self):
        consumer = source("src/ingest/MqttConsumer.cc")
        metrics = source("src/metrics/TelemetryMetrics.cc")
        for signal in ("recordReceived", "recordInvalid", "recordQueueFull", "recordResult",
                       "setBrokerConnected"):
            self.assertIn(signal, consumer)
        self.assertIn("brokerReconnects_", metrics)
        self.assertIn("latencyMaxMs_", metrics)

    def test_retryable_persistence_failures_log_the_database_reason(self):
        consumer = source("src/ingest/MqttConsumer.cc")
        self.assertIn("Telemetry persistence will be retried:", consumer)
        self.assertIn("result.reason", consumer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
