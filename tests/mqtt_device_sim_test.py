import json
import threading
import tempfile
import time
import unittest
from pathlib import Path

from tools.mqtt_device_sim import DeviceSimulator, SQLiteOutbox


class SQLiteOutboxTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.path = Path(self.tempdir.name) / "outbox.sqlite3"
        self.outbox = SQLiteOutbox(self.path, retry_deadline_sec=10)

    def tearDown(self):
        self.outbox.close()
        self.tempdir.cleanup()

    def test_pending_message_survives_reopen_and_enforces_one_in_flight(self):
        payload = {"schema_version": 1, "seq": 7, "timestamp_ms": 1700000000000,
                   "metrics": {"temperature": 23.5}}
        self.assertTrue(self.outbox.enqueue("plant-a", "motor-1", payload))
        self.assertFalse(self.outbox.enqueue("plant-a", "motor-1", payload))
        self.outbox.close()
        self.outbox = SQLiteOutbox(self.path, retry_deadline_sec=10)
        pending = self.outbox.pending("plant-a", "motor-1", now=time.time())
        self.assertEqual(pending["seq"], 7)
        self.assertEqual(json.loads(pending["payload"]), payload)

    def test_ack_must_match_identity_and_sequence(self):
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 8})
        self.assertFalse(self.outbox.handle_ack("plant-b", "motor-1", 8, "persisted"))
        self.assertFalse(self.outbox.handle_ack("plant-a", "motor-1", 9, "persisted"))
        self.assertTrue(self.outbox.handle_ack("plant-a", "motor-1", 8, "duplicate"))
        self.assertIsNone(self.outbox.pending("plant-a", "motor-1", now=time.time()))
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 9})
        self.assertTrue(self.outbox.handle_ack("plant-a", "motor-1", 9, "persisted"))

    def test_sequence_watermark_survives_restart_after_previous_ack(self):
        first_seq = self.outbox.reserve_next_sequence("plant-a", "motor-1")
        self.assertEqual(first_seq, 0)
        self.outbox.enqueue("plant-a", "motor-1", {"seq": first_seq})
        self.assertTrue(self.outbox.handle_ack("plant-a", "motor-1", first_seq,
                                               "persisted"))
        self.outbox.close()
        self.outbox = SQLiteOutbox(self.path, retry_deadline_sec=10)

        next_seq = self.outbox.reserve_next_sequence("plant-a", "motor-1")
        self.assertEqual(next_seq, 1)
        self.assertTrue(self.outbox.enqueue("plant-a", "motor-1", {"seq": next_seq}))

    def test_legacy_outbox_seeds_watermark_from_dead_letters(self):
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 7})
        self.assertTrue(self.outbox.handle_ack("plant-a", "motor-1", 7, "rejected"))
        with self.outbox.db:
            self.outbox.db.execute("DROP TABLE device_sequence")
        self.outbox.close()
        self.outbox = SQLiteOutbox(self.path, retry_deadline_sec=10)
        self.assertEqual(self.outbox.reserve_next_sequence("plant-a", "motor-1"), 8)

    def test_database_watermark_can_advance_a_local_sequence(self):
        self.assertEqual(self.outbox.ensure_next_sequence("plant-a", "motor-1", 20), 20)
        self.assertEqual(self.outbox.reserve_next_sequence("plant-a", "motor-1"), 20)

    def test_rejected_ack_moves_message_to_dead_letter(self):
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 9})
        self.assertTrue(self.outbox.handle_ack("plant-a", "motor-1", 9, "rejected"))
        self.assertEqual(len(self.outbox.dead_letters()), 1)

    def test_retry_deadline_moves_message_to_dead_letter(self):
        self.outbox.close()
        self.outbox = SQLiteOutbox(self.path, retry_deadline_sec=0.01)
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 10})
        pending = self.outbox.pending("plant-a", "motor-1", now=time.time() + 1)
        self.assertIsNone(pending)
        self.assertEqual(len(self.outbox.dead_letters()), 1)

    def test_retry_schedule_increases_delay_and_caps_attempt_backoff(self):
        self.outbox.enqueue("plant-a", "motor-1", {"seq": 11})
        base = time.time() + 0.01
        first = self.outbox.due_messages(now=base)[0]
        second = self.outbox.due_messages(now=base + 1)[0]
        third = self.outbox.due_messages(now=base + 3)[0]
        self.assertEqual([first["attempts"], second["attempts"], third["attempts"]],
                         [1, 2, 3])
        self.assertFalse(self.outbox.due_messages(now=base + 6))


class MqttDeviceSimulatorExitCodeTests(unittest.TestCase):
    def test_successful_run_returns_zero_after_writing_report(self):
        source = (Path(__file__).resolve().parents[1] / "tools" / "mqtt_device_sim.py")
        self.assertIn(
            'print(json.dumps(report, indent=2))\n        return 0',
            source.read_text(encoding="utf-8"),
        )

    def test_planned_disconnect_during_cleanup_is_not_counted_as_reconnect(self):
        simulator = DeviceSimulator.__new__(DeviceSimulator)
        simulator.stop_event = threading.Event()
        simulator.counters = {"reconnects": 0}

        class DisconnectingClient:
            def disconnect(self):
                simulator._on_disconnect(self, "device-0001", None, 0, None)

            def loop_stop(self):
                pass

        simulator.clients = {"device-0001": DisconnectingClient()}
        simulator._stop_clients()

        self.assertEqual(simulator.counters["reconnects"], 0)

    def test_unplanned_disconnect_is_counted_as_reconnect(self):
        simulator = DeviceSimulator.__new__(DeviceSimulator)
        simulator.stop_event = threading.Event()
        simulator.counters = {"reconnects": 0}

        simulator._on_disconnect(None, "device-0001", None, 7, None)

        self.assertEqual(simulator.counters["reconnects"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
