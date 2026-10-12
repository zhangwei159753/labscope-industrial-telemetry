# Benchmark reports

`tools/bench_mqtt.py` writes one JSON report per run here. Schema version 2
reports include host and runtime versions, workload configuration,
accepted/persisted/duplicate/rejected counters, retry and reconnect counts,
ACK latency percentiles, throughput, process CPU time/average cores, peak RSS,
local outbox and host filesystem usage, and worker errors. CPU and RSS refer to
the simulator process; these resource values do not include the app, PostgreSQL,
or Mosquitto containers. It also compares the number of
raw PostgreSQL rows for the run's device sequence ranges with the persisted plus
duplicate application ACK count. A missing Compose database, outstanding
outbox messages, or row-count mismatch is explicitly marked unverified. The
`drained` delivery field means no messages remain pending; it does not mean
there were no dead letters. Delivery accounting and row count are evidence for
that run, not a capacity promise. Keep failed and successful reports so claims
remain tied to the recorded machine and configuration.

The initial acceptance workload is 1,000 devices sending 1 message per second
for 10 minutes. This is a test target, not a field-capacity promise. Run it only
after provisioning device credentials and starting the Compose stack:

```powershell
python -m pip install -r requirements-dev.txt
python tools/bench_mqtt.py --site-id plant-a --credentials data/device-credentials.json
```

The SQLite outbox persists each device's next sequence and synchronizes it with
the database watermark at simulator startup. This lets an older or empty
outbox resume after already acknowledged samples. A crash between reserving a
sequence and enqueueing its sample can create a harmless sequence gap.

Create devices with `tools/provision_device.py create --site-id plant-a
--device-id device-0001 --credentials-file data/device-credentials.json`. The
command prints each generated password once and appends it to the local
credentials file; protect that file as a secret. Rotate/revoke can update the
same file. The simulator uses a durable SQLite outbox in `data/`,
which is ignored by Git.

## Published simulation evidence

The following reports were captured on a single-device local Compose stack.
They validate the application flow and recovery behavior; they do not validate
the 1,000-device capacity target or a physical installation.

| Scenario | Result | Report |
| --- | --- | --- |
| Nominal, 1 device at 1 Hz for 5 minutes | 299/299 samples persisted and verified in PostgreSQL; no retries; ACK p99 61.93 ms | [`run-20261012T031432Z.json`](run-20261012T031432Z.json) |
| Database unavailable during a 60 second run | 44/44 queued samples persisted after recovery; 4 retries; no pending or dead-letter messages | [`run-20261012T032131Z.json`](run-20261012T032131Z.json) |
| MQTT broker interruption during a 60 second run | 30/30 samples persisted; 1 retry and 1 reconnect; no pending or dead-letter messages | [`run-20261012T032358Z.json`](run-20261012T032358Z.json) |
| Sequence-gap injection | 30/30 samples persisted; 3 sequence gaps injected and verified | [`run-20261012T032517Z.json`](run-20261012T032517Z.json) |
| Temperature alarm end-to-end | 12/12 samples persisted; alert flow and database row count verified | [`run-20261012T-alarm-final.json`](run-20261012T-alarm-final.json) |

Reports omit the workstation name, local credential/outbox paths, and host disk
capacity. The unmodified reports remain local and are not needed to reproduce
these summarized results.

New local run reports are ignored by Git by default. Review and sanitize a
report before deliberately adding it as public evidence.
