"""Static contracts for persistent alarm processing and its HTTP surface."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file():
        raise AssertionError(f"required alarm implementation missing: {relative}")
    return path.read_text(encoding="utf-8")


class AlarmIntegrationContractTests(unittest.TestCase):
    def test_alarm_schema_tracks_rule_state_event_lifecycle_and_audit(self):
        rules = source("migrations/V006__alarm_rules_events.sql")
        audit = source("migrations/V007__audit_log.sql")
        for fragment in ("alarm_rules", "alarm_rule_state", "alarm_events",
                         "duration_ms", "hysteresis", "cooldown_ms",
                         "acknowledged_by", "acknowledged_at", "recovered_at",
                         "UNIQUE (rule_id, trigger_seq)"):
            self.assertIn(fragment, rules)
        self.assertIn("audit_log", audit)
        self.assertIn("metadata jsonb", audit)

    def test_worker_claims_outbox_in_order_and_marks_after_atomic_evaluation(self):
        processor = source("src/alarms/AlarmProcessor.cc")
        self.assertIn("telemetry_outbox", processor)
        self.assertIn("FOR UPDATE SKIP LOCKED", processor)
        self.assertIn("ORDER BY ob.event_id", processor)
        self.assertIn("earlier.event_id < ob.event_id", processor)
        self.assertIn("processed_at", processor)
        self.assertIn("newTransactionAsync", processor)
        self.assertIn(
            "r.created_at <= (SELECT received_at FROM telemetry_outbox WHERE event_id = $3::bigint)",
            processor,
        )
        self.assertIn("NULLIF($3::bigint, -1)", processor)
        self.assertIn("NULLIF($5::bigint, 0)", processor)

    def test_alarm_routes_require_sessions_roles_and_csrf_for_acknowledgment(self):
        controller = source("src/controllers/AlarmController.cc")
        self.assertIn('"/api/v1/alarms/rules"', controller)
        self.assertIn('"/api/v1/alarms/events"', controller)
        self.assertIn("/api/v1/alarms/events/{1}/ack", controller)
        self.assertIn("SessionFilter::classTypeName()", controller)
        self.assertIn("Authorization::requireRole", controller)
        self.assertIn("audit_log", controller + source("src/alarms/AlarmRepository.cc"))

    def test_rule_list_is_site_scoped_and_enable_mutation_is_admin_audited(self):
        controller = source("src/controllers/AlarmController.cc")
        self.assertIn('"/api/v1/alarms/rules/{1}"', controller)
        self.assertIn("SiteAccessFilter::classTypeName()", controller)
        self.assertIn("ruleJson(rows[i])", controller)
        self.assertIn("alarm_rule.set_enabled", controller)

    def test_audit_json_values_have_explicit_postgres_parameter_types(self):
        repository = source("src/alarms/AlarmRepository.cc")
        for fragment in (
            "'device_id', $4::text",
            "'metric', $5::text",
            "'direction', $6::text",
            "'threshold', $7::double precision",
            "'duration_ms', $8::bigint",
            "'hysteresis', $9::double precision",
            "'cooldown_ms', $10::bigint",
            "'severity', $11::text",
            "'rule_id', $4::bigint",
            "'device_id', $5::text",
            "'remark', $6::text",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, repository)

        controller = source("src/controllers/AlarmController.cc")
        self.assertIn("'device_id', $4::text", controller)
        self.assertIn("'enabled', $5::boolean", controller)

    def test_rule_create_validates_json_types_before_numeric_conversion(self):
        controller = source("src/controllers/AlarmController.cc")
        threshold_validation = controller.index("!thresholdValue.isNumeric()")
        threshold_conversion = controller.index("thresholdValue.asDouble()")
        duration_validation = controller.index("!durationValue.isIntegral()")
        duration_conversion = controller.index("durationValue.asInt64()")
        self.assertLess(threshold_validation, threshold_conversion)
        self.assertLess(duration_validation, duration_conversion)
        self.assertIn("transaction->setCommitCallback", controller)

    def test_outbox_retry_failure_does_not_mark_rows_processed(self):
        processor = source("src/alarms/AlarmProcessor.cc")
        self.assertIn("setCommitCallback", processor)
        self.assertIn("processed_at = clock_timestamp()", processor)
        self.assertIn("last_error", processor)


if __name__ == "__main__":
    unittest.main(verbosity=2)
