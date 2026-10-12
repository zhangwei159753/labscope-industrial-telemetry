#!/usr/bin/env python3
"""Run the MQTT load simulator and save a timestamped benchmark report."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site-id", required=True)
    parser.add_argument("--credentials", required=True)
    parser.add_argument("--devices", type=int, default=1000)
    parser.add_argument("--rate-hz", type=float, default=1.0)
    parser.add_argument("--duration-sec", type=int, default=600)
    parser.add_argument("--drain-timeout-sec", type=float, default=30.0)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--outbox", default="data/device-outbox.sqlite3")
    parser.add_argument("--compose-file", default="compose.yaml")
    parser.add_argument("--fault-profile", choices=("nominal", "sensor-spike", "sequence-gap"),
                        default="nominal")
    args = parser.parse_args()
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = Path("docs/benchmarks") / f"run-{run_id}.json"
    command = [sys.executable, "tools/mqtt_device_sim.py", "--site-id", args.site_id,
               "--credentials", args.credentials, "--devices", str(args.devices),
               "--rate-hz", str(args.rate_hz), "--duration-sec", str(args.duration_sec),
               "--drain-timeout-sec", str(args.drain_timeout_sec),
               "--host", args.host, "--port", str(args.port), "--outbox", args.outbox,
               "--compose-file", args.compose_file,
               "--fault-profile", args.fault_profile, "--report", str(output)]
    subprocess.run(command, check=True)
    report = json.loads(output.read_text(encoding="utf-8"))
    report["run_id"] = run_id
    report["configuration"]["target"] = "1000 devices x 1 msg/s for 10 minutes"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
