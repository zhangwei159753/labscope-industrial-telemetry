"""Simulate instruments and measure the same-host monitoring path.

Requires: pip install websocket-client
"""

import argparse
import json
import math
import os
import platform
import threading
import time
import urllib.request
from pathlib import Path

import websocket


def make_sample(seq, timestamp_ms):
    return {
        "seq": seq,
        "timestamp_ms": timestamp_ms,
        "metrics": {
            "temperature_c": round(22 + 0.8 * math.sin(seq / 95), 3),
            "vibration_mm_s": round(0.4 + 0.25 * math.sin(seq / 27), 3),
            "laser_power_mw": round(8 + 0.6 * math.cos(seq / 66), 3),
        },
    }


def percentile_summary(values):
    if not values:
        return {"count": 0, "p50": None, "p95": None, "p99": None}
    ordered = sorted(values)

    def pick(fraction):
        index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
        return round(ordered[index], 2)

    return {"count": len(values), "p50": pick(0.50),
            "p95": pick(0.95), "p99": pick(0.99)}


def read_stats(base_url):
    with urllib.request.urlopen(base_url + "/api/v1/stats", timeout=5) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://127.0.0.1:8848")
    parser.add_argument("--devices", type=int, default=2)
    parser.add_argument("--monitors-per-device", type=int, default=2)
    parser.add_argument("--interval-ms", type=float, default=1.0)
    parser.add_argument("--duration-sec", type=float, default=10.0)
    parser.add_argument("--device-prefix", default="scope-demo")
    parser.add_argument("--start-seq", type=int, default=0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.devices < 1 or args.monitors_per_device < 0 or args.interval_ms <= 0 \
            or args.duration_sec <= 0 or args.start_seq < 0:
        parser.error("devices >= 1, monitors >= 0, interval/duration > 0, start-seq >= 0")
    if args.base.startswith("https://"):
        ws_base = "wss://" + args.base[len("https://"):]
    elif args.base.startswith("http://"):
        ws_base = "ws://" + args.base[len("http://"):]
    else:
        parser.error("base must start with http:// or https://")
    ws_base = ws_base.rstrip("/")
    before = read_stats(args.base)

    monitors = []
    monitor_threads = []
    stop_monitors = threading.Event()
    latencies = [[] for _ in range(args.devices * args.monitors_per_device)]
    errors = []

    def monitor_loop(connection, observed):
        connection.settimeout(0.5)
        while not stop_monitors.is_set():
            try:
                event = json.loads(connection.recv())
            except websocket.WebSocketTimeoutException:
                continue
            except (websocket.WebSocketException, OSError, ValueError):
                break
            if event.get("type") == "telemetry":
                observed.append(time.time_ns() / 1_000_000 - event["timestamp_ms"])

    for device_index in range(args.devices):
        device_id = f"{args.device_prefix}-{device_index + 1:02d}"
        for monitor_index in range(args.monitors_per_device):
            connection = websocket.create_connection(
                f"{ws_base}/ws/monitor?device_id={device_id}", timeout=3,
                http_proxy_host=None)
            subscribed = json.loads(connection.recv())
            if subscribed.get("type") != "subscribed":
                raise RuntimeError(f"monitor handshake failed: {subscribed}")
            monitors.append(connection)
            observed = latencies[device_index * args.monitors_per_device + monitor_index]
            worker = threading.Thread(target=monitor_loop,
                                      args=(connection, observed), daemon=True)
            worker.start()
            monitor_threads.append(worker)

    start_gate = threading.Barrier(args.devices + 1)
    sent_counts = [0] * args.devices

    def device_loop(device_index):
        device_id = f"{args.device_prefix}-{device_index + 1:02d}"
        try:
            connection = websocket.create_connection(
                f"{ws_base}/ws/ingest?device_id={device_id}", timeout=3,
                http_proxy_host=None)
            try:
                ready = json.loads(connection.recv())
                if ready.get("type") != "ready":
                    raise RuntimeError(f"ingest handshake failed: {ready}")
                start_gate.wait(timeout=10)
                next_due = time.perf_counter_ns()
                stop_at = next_due + int(args.duration_sec * 1_000_000_000)
                interval_ns = int(args.interval_ms * 1_000_000)
                seq = args.start_seq
                while time.perf_counter_ns() < stop_at:
                    remaining = next_due - time.perf_counter_ns()
                    if remaining > 0:
                        time.sleep(remaining / 1_000_000_000)
                    sample = make_sample(seq, time.time_ns() // 1_000_000)
                    connection.send(json.dumps(sample, separators=(",", ":")))
                    sent_counts[device_index] += 1
                    seq += 1
                    next_due = max(next_due + interval_ns, time.perf_counter_ns())
            finally:
                connection.close()
        except Exception as error:  # Keep the report truthful when a worker fails.
            errors.append(f"{device_id}: {error}")
            try:
                start_gate.abort()
            except Exception:
                pass

    device_threads = [threading.Thread(target=device_loop, args=(index,), daemon=True)
                      for index in range(args.devices)]
    for worker in device_threads:
        worker.start()
    started = time.perf_counter()
    try:
        start_gate.wait(timeout=10)
    except threading.BrokenBarrierError:
        pass
    for worker in device_threads:
        worker.join()
    elapsed = time.perf_counter() - started
    time.sleep(0.2)
    after = read_stats(args.base)
    stop_monitors.set()
    for connection in monitors:
        connection.close()
    for worker in monitor_threads:
        worker.join(timeout=1)

    flattened = [value for monitor in latencies for value in monitor]
    report = {
        "mode": "same-host simulated instruments; no real hardware",
        "host": {"platform": platform.platform(), "processor": platform.processor(),
                 "logical_cpus": os.cpu_count()},
        "configuration": {"devices": args.devices,
                          "monitors_per_device": args.monitors_per_device,
                          "target_interval_ms": args.interval_ms,
                          "duration_sec": args.duration_sec},
        "results": {"elapsed_sec": round(elapsed, 3),
                    "samples_sent": sum(sent_counts),
                    "actual_send_rate_per_sec": round(sum(sent_counts) / elapsed, 1),
                    "server_accepted_delta": after["accepted"] - before["accepted"],
                    "server_rejected_delta": after["rejected"] - before["rejected"],
                    "monitor_frames_observed": len(flattened),
                    "observed_monitor_latency_ms": percentile_summary(flattened)},
        "worker_errors": errors,
    }
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    if errors or report["results"]["server_accepted_delta"] != sum(sent_counts):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
