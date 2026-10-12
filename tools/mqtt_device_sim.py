#!/usr/bin/env python3
"""Durable MQTT device simulator for LabScope's post-commit ACK contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import secrets
import socket
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any


TERMINAL_ACKS = {"persisted", "duplicate", "rejected"}
MAX_RETRIES = 10
MAX_BACKOFF_SEC = 30.0


class SQLiteOutbox:
    """WAL-backed device queue: at most one unacknowledged sample per device."""

    def __init__(self, path: str | Path, retry_deadline_sec: float = 300.0):
        self.path = str(path)
        self.retry_deadline_sec = retry_deadline_sec
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(self.path, timeout=30, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        with self.db:
            self.db.execute("PRAGMA journal_mode=WAL")
            self.db.execute("PRAGMA synchronous=FULL")
            self.db.execute("""CREATE TABLE IF NOT EXISTS pending (
                site_id TEXT NOT NULL, device_id TEXT NOT NULL, seq INTEGER NOT NULL,
                payload TEXT NOT NULL, created_at REAL NOT NULL,
                attempts INTEGER NOT NULL DEFAULT 0, next_retry_at REAL NOT NULL,
                PRIMARY KEY(site_id, device_id), UNIQUE(site_id, device_id, seq))""")
            self.db.execute("""CREATE TABLE IF NOT EXISTS dead_letter (
                id INTEGER PRIMARY KEY, site_id TEXT NOT NULL, device_id TEXT NOT NULL,
                seq INTEGER NOT NULL, payload TEXT NOT NULL, reason TEXT NOT NULL,
                attempts INTEGER NOT NULL, failed_at REAL NOT NULL)""")
            self.db.execute("""CREATE TABLE IF NOT EXISTS device_sequence (
                site_id TEXT NOT NULL, device_id TEXT NOT NULL,
                next_seq INTEGER NOT NULL CHECK(next_seq >= 0),
                PRIMARY KEY(site_id, device_id))""")
            self.db.execute("""INSERT INTO device_sequence(site_id, device_id, next_seq)
                SELECT site_id, device_id, MAX(seq) + 1 FROM (
                    SELECT site_id, device_id, seq FROM pending
                    UNION ALL
                    SELECT site_id, device_id, seq FROM dead_letter
                ) history
                GROUP BY site_id, device_id ON CONFLICT(site_id, device_id) DO NOTHING""")

    def close(self) -> None:
        with self.lock:
            self.db.close()

    def enqueue(self, site_id: str, device_id: str, payload: dict[str, Any]) -> bool:
        encoded = json.dumps(payload, separators=(",", ":"), sort_keys=True,
                             allow_nan=False)
        seq = payload.get("seq")
        if not isinstance(seq, int) or seq < 0:
            raise ValueError("payload seq must be a non-negative integer")
        now = time.time()
        with self.lock, self.db:
            try:
                self.db.execute("""INSERT INTO pending
                    (site_id, device_id, seq, payload, created_at, next_retry_at)
                    VALUES (?, ?, ?, ?, ?, ?)""",
                    (site_id, device_id, seq, encoded, now, now))
                return True
            except sqlite3.IntegrityError:
                return False

    def reserve_next_sequence(self, site_id: str, device_id: str) -> int:
        """Persist the next device sequence before using it; crashes may leave gaps."""
        with self.lock, self.db:
            row = self.db.execute("""INSERT INTO device_sequence
                (site_id, device_id, next_seq) VALUES (?, ?, 1)
                ON CONFLICT(site_id, device_id) DO UPDATE
                SET next_seq = device_sequence.next_seq + 1
                RETURNING next_seq - 1 AS reserved_seq""",
                (site_id, device_id)).fetchone()
            return int(row["reserved_seq"])

    def ensure_next_sequence(self, site_id: str, device_id: str,
                             minimum_next_seq: int) -> int:
        if minimum_next_seq < 0:
            raise ValueError("minimum sequence must be non-negative")
        with self.lock, self.db:
            row = self.db.execute("""INSERT INTO device_sequence
                (site_id, device_id, next_seq) VALUES (?, ?, ?)
                ON CONFLICT(site_id, device_id) DO UPDATE
                SET next_seq = MAX(device_sequence.next_seq, excluded.next_seq)
                RETURNING next_seq""",
                (site_id, device_id, minimum_next_seq)).fetchone()
            return int(row["next_seq"])

    def pending(self, site_id: str, device_id: str, now: float | None = None):
        now = time.time() if now is None else now
        with self.lock, self.db:
            self._expire(now)
            row = self.db.execute("""SELECT * FROM pending
                WHERE site_id = ? AND device_id = ?""", (site_id, device_id)).fetchone()
            return dict(row) if row else None

    def due_messages(self, now: float | None = None, limit: int = 1000):
        now = time.time() if now is None else now
        due = []
        with self.lock, self.db:
            self._expire(now)
            rows = self.db.execute("""SELECT * FROM pending
                WHERE next_retry_at <= ? ORDER BY next_retry_at LIMIT ?""",
                (now, limit)).fetchall()
            for row in rows:
                attempts = row["attempts"] + 1
                delay = min(2 ** max(attempts - 1, 0), MAX_BACKOFF_SEC)
                self.db.execute("UPDATE pending SET attempts = ?, next_retry_at = ? "
                                "WHERE site_id = ? AND device_id = ?",
                                (attempts, now + delay, row["site_id"], row["device_id"]))
                due.append(dict(row) | {"attempts": attempts})
        return due

    def handle_ack(self, site_id: str, device_id: str, seq: int, status: str) -> bool:
        if status not in TERMINAL_ACKS:
            return False
        with self.lock, self.db:
            row = self.db.execute("""SELECT * FROM pending
                WHERE site_id = ? AND device_id = ? AND seq = ?""",
                (site_id, device_id, seq)).fetchone()
            if row is None:
                return False
            if status == "rejected":
                self._dead_letter(row, "server rejected telemetry")
            self.db.execute("DELETE FROM pending WHERE site_id = ? AND device_id = ?",
                            (site_id, device_id))
            return True

    def dead_letters(self):
        with self.lock:
            return [dict(row) for row in self.db.execute(
                "SELECT * FROM dead_letter ORDER BY id").fetchall()]

    def counts(self) -> dict[str, int]:
        with self.lock:
            return {
                "pending": self.db.execute("SELECT count(*) FROM pending").fetchone()[0],
                "dead_letter": self.db.execute("SELECT count(*) FROM dead_letter").fetchone()[0],
            }

    def _expire(self, now: float) -> None:
        expired = self.db.execute("SELECT * FROM pending WHERE created_at + ? <= ?",
                                  (self.retry_deadline_sec, now)).fetchall()
        for row in expired:
            self._dead_letter(row, "retry deadline exceeded")
            self.db.execute("DELETE FROM pending WHERE site_id = ? AND device_id = ?",
                            (row["site_id"], row["device_id"]))

    def _dead_letter(self, row: sqlite3.Row, reason: str) -> None:
        self.db.execute("""INSERT INTO dead_letter
            (site_id, device_id, seq, payload, reason, attempts, failed_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (row["site_id"], row["device_id"], row["seq"], row["payload"],
                         reason, row["attempts"], time.time()))


def make_sample(site_id: str, device_id: str, seq: int, now_ms: int,
                device_index: int, fault_profile: str = "nominal") -> dict[str, Any]:
    temperature = 20.0 + (device_index % 170) * 0.1 + (seq % 17) * 0.03
    vibration = 0.2 + (device_index % 13) * 0.01 + (seq % 7) * 0.005
    if fault_profile == "sensor-spike" and seq > 0 and seq % 10 == 0:
        temperature = 120.0
    return {"schema_version": 1, "seq": seq, "timestamp_ms": now_ms,
            "metrics": {"temperature_c": round(temperature, 3),
                        "vibration_mm_s": round(vibration, 3)}}


def build_report(host: str, configuration: dict[str, Any], counters: dict[str, int],
                 ack_latencies_ms: list[float], errors: list[str], started_at: float,
                 finished_at: float,
                 database_verification: dict[str, Any] | None = None,
                 cpu_process_seconds: float | None = None) -> dict[str, Any]:
    latencies = sorted(ack_latencies_ms)

    def percentile(p: float) -> float | None:
        if not latencies:
            return None
        return round(latencies[min(math.ceil(p * len(latencies)) - 1,
                                   len(latencies) - 1)], 3)

    elapsed = max(finished_at - started_at, 0.001)
    acknowledged = sum(counters.get(name, 0)
                       for name in ("persisted", "duplicate", "rejected"))
    return {"schema_version": 2, "host": host, "configuration": configuration,
            "runtime": {"python": sys.version.split()[0],
                        "platform": platform.platform(),
                        "sqlite": sqlite3.sqlite_version,
                        "paho_mqtt": _package_version("paho-mqtt")},
            "started_at_epoch": started_at, "finished_at_epoch": finished_at,
            "elapsed_sec": round(elapsed, 3), "counters": counters,
            "throughput_per_sec": round(counters.get("persisted", 0) / elapsed, 3),
            "ack_latency_ms": {"p50": percentile(0.50), "p95": percentile(0.95),
                               "p99": percentile(0.99)},
            "worker_errors": errors,
            "resource_usage": _resource_usage(configuration.get("outbox", ""), elapsed,
                                               cpu_process_seconds),
            "delivery_accounting": {
                "enqueued": counters.get("enqueued", 0),
                "terminal_acks": acknowledged,
                "unresolved_pending": counters.get("pending_at_end", 0),
                "dead_letter_total": counters.get("dead_letter", 0),
                "drained": counters.get("pending_at_end", 0) == 0},
            "database_verification": database_verification or {
                "verified": False, "reason": "database row verification was not run"}}


def _package_version(name: str) -> str | None:
    try:
        from importlib.metadata import version
        return version(name)
    except Exception:
        return None


def _resource_usage(outbox_path: str, elapsed_sec: float,
                    cpu_process_seconds: float | None = None) -> dict[str, int | float | None]:
    cpu_seconds = time.process_time() if cpu_process_seconds is None else cpu_process_seconds
    peak_rss_bytes = None
    try:
        import resource
        peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        peak_rss_bytes = int(peak_rss if sys.platform == "darwin" else peak_rss * 1024)
    except (ImportError, AttributeError, OSError):
        if os.name == "nt":
            try:
                import ctypes
                from ctypes import wintypes

                class MemoryCounters(ctypes.Structure):
                    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                                ("PeakWorkingSetSize", ctypes.c_size_t),
                                ("WorkingSetSize", ctypes.c_size_t),
                                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                                ("PagedPoolUsage", ctypes.c_size_t),
                                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                                ("NonPagedPoolUsage", ctypes.c_size_t),
                                ("PagefileUsage", ctypes.c_size_t),
                                ("PeakPagefileUsage", ctypes.c_size_t)]

                counters = MemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                psapi = ctypes.WinDLL("Psapi.dll")
                process = ctypes.WinDLL("kernel32.dll").GetCurrentProcess()
                if psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
                    peak_rss_bytes = int(counters.PeakWorkingSetSize)
            except (AttributeError, OSError):
                pass
    outbox_bytes = 0
    disk_total_bytes = None
    disk_free_bytes = None
    if outbox_path:
        path = Path(outbox_path)
        for candidate in (path, Path(str(path) + "-wal"), Path(str(path) + "-shm")):
            try:
                outbox_bytes += candidate.stat().st_size
            except OSError:
                pass
        try:
            usage = shutil.disk_usage(path.parent if path.parent.exists() else Path.cwd())
            disk_total_bytes, disk_free_bytes = usage.total, usage.free
        except OSError:
            pass
    return {"cpu_process_seconds": round(cpu_seconds, 3),
            "cpu_average_cores": round(cpu_seconds / max(elapsed_sec, 0.001), 3),
            "peak_rss_bytes": peak_rss_bytes, "outbox_bytes": outbox_bytes,
            "disk_total_bytes": disk_total_bytes, "disk_free_bytes": disk_free_bytes}


def verify_database_rows(compose_file: str, site_id: str,
                         sequence_ranges: dict[str, tuple[int, int]],
                         expected_rows: int) -> dict[str, Any]:
    """Compare committed/duplicate ACKs with retained raw rows for this run's sequences."""
    if not sequence_ranges:
        return {"verified": False, "expected_rows": expected_rows,
                "persisted_rows": None, "reason": "no sequence range was recorded"}
    values = ", ".join(
        f"({_sql_literal(device_id)}, {int(bounds[0])}, {int(bounds[1])})"
        for device_id, bounds in sorted(sequence_ranges.items()))
    sql = ("WITH seq_ranges(device_id, first_seq, last_seq) AS (VALUES " + values + ") "
           "SELECT current_setting('server_version'), count(*) FROM telemetry_samples t JOIN seq_ranges r "
           "ON r.device_id = t.device_id WHERE t.site_id = " + _sql_literal(site_id) +
           " AND t.seq BETWEEN r.first_seq AND r.last_seq")
    env_values = {"POSTGRES_USER": "labscope", "POSTGRES_DB": "labscope"}
    dotenv = Path(compose_file).resolve().parent / ".env"
    if dotenv.is_file():
        for line in dotenv.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key.strip() in env_values:
                env_values[key.strip()] = value.strip().strip('"').strip("'")
    command = ["docker", "compose", "-f", compose_file, "exec", "-T", "db", "psql",
               "-U", env_values["POSTGRES_USER"], "-d", env_values["POSTGRES_DB"],
               "-At", "-F", "|", "-c", sql]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True,
                                timeout=30)
        postgres_version, persisted_count = result.stdout.strip().split("|", 1)
        persisted_rows = int(persisted_count)
        return {"verified": persisted_rows == expected_rows,
                "expected_rows": expected_rows, "persisted_rows": persisted_rows,
                "postgres_version": postgres_version,
                "reason": "row count matches application ACK outcomes"
                if persisted_rows == expected_rows else "database row count mismatch"}
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        return {"verified": False, "expected_rows": expected_rows,
                "persisted_rows": None,
                "reason": f"database verification unavailable: {type(exc).__name__}"}


def load_database_watermarks(compose_file: str, site_id: str,
                             device_ids: list[str]) -> dict[str, int]:
    """Read server sequence state so legacy/empty outboxes cannot restart at zero."""
    if not device_ids:
        return {}
    device_list = ", ".join(_sql_literal(device_id) for device_id in device_ids)
    sql = ("SELECT device_id || '|' || (last_seq + 1)::text FROM devices WHERE site_id = " +
           _sql_literal(site_id) + " AND device_id IN (" + device_list + ") ORDER BY device_id")
    env_values = {"POSTGRES_USER": "labscope", "POSTGRES_DB": "labscope"}
    dotenv = Path(compose_file).resolve().parent / ".env"
    if dotenv.is_file():
        for line in dotenv.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key.strip() in env_values:
                env_values[key.strip()] = value.strip().strip('"').strip("'")
    command = ["docker", "compose", "-f", compose_file, "exec", "-T", "db", "psql",
               "-U", env_values["POSTGRES_USER"], "-d", env_values["POSTGRES_DB"],
               "-At", "-c", sql]
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    watermarks: dict[str, int] = {}
    for line in result.stdout.splitlines():
        device_id, next_seq = line.split("|", 1)
        watermarks[device_id] = int(next_seq)
    missing = set(device_ids) - watermarks.keys()
    if missing:
        raise RuntimeError("server sequence watermark is missing for configured devices")
    return watermarks


def topic(site_id: str, device_id: str, suffix: str) -> str:
    return f"labscope/v1/sites/{site_id}/devices/{device_id}/{suffix}"


class DeviceSimulator:
    def __init__(self, args, credentials: dict[str, dict[str, str]]):
        try:
            import paho.mqtt.client as mqtt
        except ImportError as exc:
            raise RuntimeError("install requirements-dev.txt to use the MQTT simulator") from exc
        self.mqtt = mqtt
        self.args = args
        self.credentials = credentials
        self.outbox = SQLiteOutbox(args.outbox, args.retry_deadline_sec)
        self.clients: dict[str, Any] = {}
        self.client_by_user_data: dict[Any, str] = {}
        self.latencies: list[float] = []
        self.errors: list[str] = []
        self.counters = {"sent": 0, "persisted": 0, "duplicate": 0, "rejected": 0,
                         "retries": 0, "reconnects": 0, "worker_errors": 0,
                         "enqueued": 0, "injected_sequence_gaps": 0}
        self.started = time.time()
        self.started_cpu = time.process_time()
        self.stop_event = threading.Event()

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code != 0:
            self._error(f"{userdata}: connect failed: {reason_code}")
            return
        client.subscribe(topic(self.args.site_id, userdata, "acks"), qos=1)

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties):
        if not self.stop_event.is_set():
            self.counters["reconnects"] += 1

    def _on_message(self, client, userdata, message):
        try:
            ack = json.loads(message.payload)
            seq, status = ack.get("seq"), ack.get("status")
            accepted = self.outbox.handle_ack(self.args.site_id, userdata, seq, status)
            if not accepted:
                return
            self.counters[status] += 1
            sent_at = self._sent_at.pop((userdata, seq), None)
            if sent_at:
                self.latencies.append((time.time() - sent_at) * 1000)
        except (ValueError, TypeError, KeyError) as exc:
            self._error(f"{userdata}: invalid ACK: {exc}")

    _sent_at: dict[tuple[str, int], float] = {}

    def _error(self, message: str):
        self.counters["worker_errors"] += 1
        if len(self.errors) < 100:
            self.errors.append(message)

    def _stop_clients(self):
        self.stop_event.set()
        for client in self.clients.values():
            client.disconnect()
            client.loop_stop()

    def start(self):
        self._sent_at = {}
        for device_id in self.args.devices_list:
            cred = self.credentials.get(device_id)
            if not cred:
                raise ValueError(f"missing credentials for device {device_id}")
            client = self.mqtt.Client(self.mqtt.CallbackAPIVersion.VERSION2,
                                      client_id="sim-" + hashlib.sha256(
                                          f"{self.args.site_id}/{device_id}".encode()
                                      ).hexdigest()[:24],
                                      protocol=self.mqtt.MQTTv5)
            client.username_pw_set(cred["username"], cred["password"])
            client.user_data_set(device_id)
            client.on_connect = self._on_connect
            client.on_disconnect = self._on_disconnect
            client.on_message = self._on_message
            client.reconnect_delay_set(min_delay=1, max_delay=30)
            client.connect_async(self.args.host, self.args.port, keepalive=60)
            client.loop_start()
            self.clients[device_id] = client

    def run(self):
        watermarks = load_database_watermarks(self.args.compose_file, self.args.site_id,
                                               self.args.devices_list)
        for device_id, next_seq in watermarks.items():
            self.outbox.ensure_next_sequence(self.args.site_id, device_id, next_seq)
        self.start()
        started = time.time()
        sequence_ranges: dict[str, tuple[int, int]] = {}
        for device_id in self.args.devices_list:
            pending = self.outbox.pending(self.args.site_id, device_id, started)
            if pending:
                sequence_ranges[device_id] = (pending["seq"], pending["seq"])
        load_ends = started + self.args.duration_sec
        drain_ends = load_ends + self.args.drain_timeout_sec
        interval = 1.0 / self.args.rate_hz
        next_emit = started
        device_index = {device_id: index for index, device_id in
                        enumerate(self.args.devices_list)}
        try:
            while time.time() < drain_ends:
                now = time.time()
                if now >= load_ends and self.outbox.counts()["pending"] == 0:
                    break
                for item in self.outbox.due_messages(now):
                    client = self.clients.get(item["device_id"])
                    if client and client.is_connected():
                        info = client.publish(topic(self.args.site_id, item["device_id"],
                                                    "telemetry"), item["payload"], qos=1)
                        if info.rc == self.mqtt.MQTT_ERR_SUCCESS:
                            self.counters["sent"] += 1
                            if item["attempts"] > 1:
                                self.counters["retries"] += 1
                            self._sent_at[(item["device_id"], item["seq"])] = now
                if now < load_ends and now >= next_emit:
                    for device_id in self.args.devices_list:
                        if self.outbox.pending(self.args.site_id, device_id, now):
                            continue
                        current = self.outbox.reserve_next_sequence(
                            self.args.site_id, device_id)
                        # Inject deterministic sequence gaps for recovery-path exercises.
                        if self.args.fault_profile == "sequence-gap" and current > 0 and current % 10 == 0:
                            current = self.outbox.reserve_next_sequence(
                                self.args.site_id, device_id)
                            self.counters["injected_sequence_gaps"] += 1
                        payload = make_sample(self.args.site_id, device_id, current,
                                              int(now * 1000), device_index[device_id],
                                              self.args.fault_profile)
                        if self.outbox.enqueue(self.args.site_id, device_id, payload):
                            self.counters["enqueued"] += 1
                            first, last = sequence_ranges.get(device_id, (current, current))
                            sequence_ranges[device_id] = (min(first, current),
                                                          max(last, current))
                    next_emit = now + interval
                time.sleep(min(0.01, interval / 4))
        finally:
            final_outbox_counts = self.outbox.counts()
            self._stop_clients()
            self.outbox.close()
        finished = time.time()
        counters = dict(self.counters)
        counters["pending_at_end"] = final_outbox_counts["pending"]
        counters["dead_letter"] = final_outbox_counts["dead_letter"]
        database_verification = verify_database_rows(
            self.args.compose_file, self.args.site_id, sequence_ranges,
            counters["persisted"] + counters["duplicate"])
        if counters["pending_at_end"]:
            database_verification["verified"] = False
            database_verification["reason"] = "unacknowledged samples remain in the device outbox"
        report = build_report(socket.gethostname(), vars(self.args), counters,
                              self.latencies, self.errors, self.started, finished,
                              database_verification,
                              max(time.process_time() - self.started_cpu, 0.0))
        Path(self.args.report).parent.mkdir(parents=True, exist_ok=True)
        Path(self.args.report).write_text(json.dumps(report, indent=2) + "\n",
                                          encoding="utf-8")
        print(json.dumps(report, indent=2))
        return 0


def _identifier(value: str, label: str) -> str:
    if not value or len(value) > 63 or not value[0].isalnum() or any(
            not (char.isalnum() or char in "-_") for char in value):
        raise argparse.ArgumentTypeError(f"invalid {label}: use letters, digits, _ or -")
    return value


def _sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _compose_exec(service: str, command: list[str], compose_file: str):
    return ["docker", "compose", "-f", compose_file, "exec", "-T", service, *command]


def provision(args) -> int:
    env_path = Path(args.compose_file).resolve().parent / ".env"
    if env_path.is_file():
        for line in env_path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))
    site = _identifier(args.site_id, "site id")
    device = _identifier(args.device_id, "device id")
    username = f"dev-{site}-{device}"
    role = f"role-{site}-{device}"
    admin = os.environ.get("MOSQUITTO_ADMIN_USER", "labscope-admin")
    admin_password = os.environ.get("MOSQUITTO_ADMIN_PASSWORD")
    if not admin_password:
        raise RuntimeError("set MOSQUITTO_ADMIN_PASSWORD before provisioning")
    db_user = os.environ.get("POSTGRES_USER", "labscope")
    db_password = os.environ.get("POSTGRES_PASSWORD")
    db_name = os.environ.get("POSTGRES_DB", "labscope")
    if not db_password:
        raise RuntimeError("set POSTGRES_PASSWORD before provisioning")
    broker = ["mosquitto_ctrl", "-h", "127.0.0.1", "-p", "1883", "-u", admin,
              "-P", admin_password, "dynsec"]
    psql = ["psql", "-v", "ON_ERROR_STOP=1", "-U", db_user, "-d", db_name]
    password = secrets.token_urlsafe(32)
    broker_run = lambda *args_: subprocess.run(
        _compose_exec("mqtt", broker + list(args_), args.compose_file), check=True,
        capture_output=True, text=True)
    def create_broker_client():
        # dynsec createClient prompts for the new credential. Supplying both
        # lines keeps batch provisioning deterministic and non-interactive.
        return subprocess.run(
            _compose_exec("mqtt", broker + ["createClient", username], args.compose_file),
            check=True, capture_output=True, text=True,
            input=f"{password}\n{password}\n")
    db_run = lambda sql: subprocess.run(
        _compose_exec("db", psql + ["-c", sql], args.compose_file), check=True,
        capture_output=True, text=True,
        env={**os.environ, "PGPASSWORD": db_password})

    if args.command == "create":
        sql = ("INSERT INTO sites(site_id,display_name) VALUES (" + _sql_literal(site) + "," +
               _sql_literal(args.site_name or site) + ") ON CONFLICT(site_id) DO NOTHING; "
               "INSERT INTO devices(site_id,device_id,display_name,model,mqtt_username,credential_hash) VALUES (" +
               ",".join((_sql_literal(site), _sql_literal(device),
                          _sql_literal(args.display_name or device),
                          _sql_literal(args.model or ""), _sql_literal(username),
                          "decode(" + _sql_literal(hashlib.sha256(password.encode()).hexdigest()) + ",'hex')")) +
               ");")
        db_run(sql)
        try:
            broker_run("createRole", role)
            broker_run("addRoleACL", role, "publishClientSend",
                       topic(site, device, "telemetry"), "allow", "5")
            broker_run("addRoleACL", role, "publishClientReceive",
                       topic(site, device, "acks"), "allow", "5")
            broker_run("addRoleACL", role, "subscribeLiteral",
                       topic(site, device, "acks"), "allow", "5")
            create_broker_client()
            broker_run("addClientRole", username, role, "5")
        except subprocess.CalledProcessError:
            db_run("DELETE FROM devices WHERE site_id=" + _sql_literal(site) +
                   " AND device_id=" + _sql_literal(device) + ";")
            raise
        print(json.dumps({"site_id": site, "device_id": device, "username": username,
                          "password": password}, indent=2))
        if args.credentials_file:
            _write_credentials(args.credentials_file, device, username, password)
        return 0

    query = "SELECT mqtt_username FROM devices WHERE site_id=" + _sql_literal(site) + \
            " AND device_id=" + _sql_literal(device) + ";"
    result = db_run(query)
    if username not in result.stdout:
        raise RuntimeError("device is not provisioned or has a different MQTT username")
    if args.command == "rotate":
        broker_run("setClientPassword", username, password)
        db_run("UPDATE devices SET credential_hash=decode(" +
               _sql_literal(hashlib.sha256(password.encode()).hexdigest()) +
               ",'hex'), credential_version=credential_version+1, updated_at=clock_timestamp() "
               "WHERE site_id=" + _sql_literal(site) + " AND device_id=" + _sql_literal(device) + ";")
        print(json.dumps({"site_id": site, "device_id": device, "username": username,
                          "password": password}, indent=2))
        if args.credentials_file:
            _write_credentials(args.credentials_file, device, username, password)
    else:
        broker_run("disableClient", username)
        db_run("UPDATE devices SET enabled=false, updated_at=clock_timestamp() WHERE site_id=" +
               _sql_literal(site) + " AND device_id=" + _sql_literal(device) + ";")
        print(f"revoked {site}/{device}")
        if args.credentials_file:
            path = Path(args.credentials_file)
            if path.exists():
                credentials = json.loads(path.read_text(encoding="utf-8"))
                credentials.pop(device, None)
                path.write_text(json.dumps(credentials, indent=2) + "\n", encoding="utf-8")
    return 0


def _write_credentials(path_value: str, device_id: str, username: str, password: str):
    path = Path(path_value)
    path.parent.mkdir(parents=True, exist_ok=True)
    credentials = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    credentials[device_id] = {"username": username, "password": password}
    path.write_text(json.dumps(credentials, indent=2) + "\n", encoding="utf-8")


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="create site/device, broker ACL, and credentials")
    rotate = sub.add_parser("rotate", help="rotate a device password")
    revoke = sub.add_parser("revoke", help="disable a device credential and registry entry")
    for command in (create, rotate, revoke):
        command.add_argument("--site-id", required=True)
        command.add_argument("--device-id", required=True)
        command.add_argument("--compose-file", default="compose.yaml")
        command.add_argument("--credentials-file")
    create.add_argument("--site-name")
    create.add_argument("--display-name")
    create.add_argument("--model")
    return result


def simulator_parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--site-id", required=True)
    result.add_argument("--devices", type=int, required=True)
    result.add_argument("--credentials", required=True,
                        help="JSON mapping device id to {username,password}")
    result.add_argument("--host", default="127.0.0.1")
    result.add_argument("--port", type=int, default=1883)
    result.add_argument("--rate-hz", type=float, default=1.0)
    result.add_argument("--duration-sec", type=int, default=60)
    result.add_argument("--drain-timeout-sec", type=float, default=30.0)
    result.add_argument("--outbox", default="data/device-outbox.sqlite3")
    result.add_argument("--compose-file", default="compose.yaml",
                        help="Compose project used to verify persisted sample counts")
    result.add_argument("--retry-deadline-sec", type=float, default=300)
    result.add_argument("--fault-profile", choices=("nominal", "sensor-spike", "sequence-gap"),
                        default="nominal")
    result.add_argument("--report", default="docs/benchmarks/latest.json")
    return result


def provision_main(argv=None):
    return provision(parser().parse_args(argv))


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv and argv[0] in {"create", "rotate", "revoke"}:
        return provision_main(argv)
    args = simulator_parser().parse_args(argv)
    if (args.devices < 1 or args.devices > 10000 or args.rate_hz <= 0 or
            args.duration_sec < 1 or args.drain_timeout_sec < 0):
        raise SystemExit("devices must be 1..10000, rate-hz > 0, duration-sec >= 1, drain >= 0")
    args.devices_list = [f"device-{index:04d}" for index in range(1, args.devices + 1)]
    credentials = json.loads(Path(args.credentials).read_text(encoding="utf-8"))
    return DeviceSimulator(args, credentials).run()


if __name__ == "__main__":
    raise SystemExit(main())
