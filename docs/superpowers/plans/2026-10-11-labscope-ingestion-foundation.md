# LabScope Industrial Telemetry Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the in-memory WebSocket ingest prototype with an MQTT-to-PostgreSQL telemetry path that preserves device data across service restarts and is measurable under load.

**Architecture:** Keep the existing Drogon application and CMake source dependency. Add a Mosquitto broker, MQTT 5 consumer, bounded per-device ordered work queues, transactional PostgreSQL persistence, post-commit application acknowledgments, a SQLite-backed simulator outbox, and versioned Compose deployment.

**Tech Stack:** C++20, Drogon 1.9.13 with PostgreSQL ORM driver, PostgreSQL 16, Mosquitto 2.x, Eclipse Paho MQTT C async 1.3.16, CMake, Python 3, SQLite.

**Spec:** `docs/superpowers/specs/2026-10-11-labscope-industrial-telemetry-design.md`

## Global Constraints

- Do not modify Drogon framework source; keep the application in `lab-telemetry-monitor`.
- Each device has at most one unacknowledged telemetry message in flight in the first release.
- MQTT QoS 1 PUBACK is not a database commit; publish a non-retained application ACK only after the PostgreSQL transaction commits.
- `persisted` and `duplicate` application ACKs remove a message from the simulator outbox; `rejected` messages move to its failed queue; missing ACKs use bounded backoff and a configured retry deadline.
- The initial capacity target is 1,000 devices × 1 message/second for 10 minutes; all benchmark claims include the recorded machine and software setup.
- PostgreSQL stores daily raw partitions for 24 hours and one-minute min/max/avg/count aggregates for 30 days by default.
- Compose is a single-node deployment; do not claim HA or safety-critical control capability.

## Review Focus

- A PostgreSQL outage after broker delivery must leave the device message pending; test that no application ACK is published and that retry persists the row once after recovery.
- A crash after database commit but before ACK delivery must cause a replay that returns `duplicate` without inserting a second row.
- Duplicate, stale, and skipped device sequence numbers must update idempotency and gap counters consistently; test same sequence, lower sequence, and a gap.
- A full ingestion queue must remain bounded and stop ACK progress without blocking Drogon request/event-loop threads; test configured capacity and recovery after drain.
- Invalid payloads and unknown device credentials must not create telemetry rows or cause unbounded retries; test permanent rejection and simulator dead-letter behavior.

---

### Task 1: Build, dependency, and local service foundation

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `vcpkg.json`
- Create: `compose.yaml`
- Create: `config/mosquitto/mosquitto.conf`
- Create: `docker/mosquitto-entrypoint.sh`
- Modify: `.gitignore`
- Test: `tests/build_contract_test.py`

**Interfaces:**
- Consumes: Existing Drogon source path `DROGON_SOURCE_DIR` and existing CMake build.
- Produces: CMake targets `lab_telemetry` and `lab_telemetry_tests`; Compose services `app`, `mqtt`, `db`, `migrate` with PostgreSQL health checks. Container builds fetch the pinned Drogon 1.9.13 source; local CMake continues to allow `DROGON_SOURCE_DIR` override.

- [x] **Step 1: Write a build-contract test** that asserts CMake enables Drogon ORM/PostgreSQL, declares the Paho async target, and Compose declares durable `db` and `mqtt` volumes.
- [x] **Step 2: Run the focused test** with `python -m unittest discover -s tests -p "build_contract_test.py" -v`; confirm it fails against the existing CMake and absent Compose file.
- [x] **Step 3: Configure dependencies** for Drogon ORM/PostgreSQL and Paho MQTT C 1.3.16 using pinned CMake FetchContent (`v1.3.16`, async TLS target); use vcpkg for libpq/OpenSSL and existing utility libraries. Pin Mosquitto and PostgreSQL image tags in Compose; add Mosquitto dynamic-security configuration for per-device publish/subscribe ACL; keep generated build, vcpkg, local benchmark output, and secrets ignored while allowing committed benchmark evidence. The application Docker build fetches Drogon tag `v1.9.13`; local CMake retains the existing `DROGON_SOURCE_DIR` override.
- [x] **Step 4: Run configuration and static contract checks** with the existing Visual Studio/vcpkg configure command from README (including its toolchain file) and `python -m unittest discover -s tests -p "build_contract_test.py" -v`; expected result is a successful CMake configure and passing contract test.
- [x] **Step 5: Commit** as `build: add mqtt and postgres service foundation`.

### Task 2: Database schema and migration lifecycle

**Files:**
- Create: `migrations/V001__identity_and_devices.sql`
- Create: `migrations/V002__telemetry_and_aggregates.sql`
- Create: `src/storage/PartitionMaintenance.h`
- Create: `src/storage/PartitionMaintenance.cc`
- Modify: `compose.yaml`
- Test: `tests/migrations_test.py`

**Interfaces:**
- Consumes: Task 1 Compose PostgreSQL service and Flyway migration service.
- Produces: Tables `sites`, `devices`, `telemetry_samples`, `telemetry_1m`, `telemetry_outbox`, and Flyway's `flyway_schema_history`; daily partition creation and retention maintenance APIs. The outbox stores committed sample references for restart-safe alarm processing in the operations plan. Auth/alarm tables are added there as well.

- [x] **Step 1: Write migration contract tests** for required site/device columns, indexes, per-device sequence watermark, daily partition key, aggregate uniqueness, and telemetry outbox uniqueness/processing fields.
- [ ] **Step 2: Run the migration tests** against a fresh Compose PostgreSQL instance; confirm expected tables and retention metadata are absent. *(Blocked locally: Docker Desktop engine pipe is inaccessible.)*
- [x] **Step 3: Add V001 site/device and V002 telemetry/aggregate/outbox SQL migrations** and a Flyway Compose service that completes before `app` starts; create current and next-day raw partitions at startup, and provide an idempotent daily maintenance function to create future partitions and drop data older than 24 hours/30 days. Retention cleanup also removes processed outbox rows after their configured audit window.
- [ ] **Step 4: Run migration tests** twice against the same database and once against a fresh database; expected result is idempotent startup and all schema/partition assertions pass. *(Blocked locally: Docker Desktop engine pipe is inaccessible.)*
- [x] **Step 5: Commit** as `feat(storage): add telemetry schema and retention lifecycle`.

### Task 3: Telemetry envelope validation and domain types

**Files:**
- Create: `src/telemetry/TelemetryEnvelope.h`
- Create: `src/telemetry/TelemetryValidator.h`
- Create: `src/telemetry/TelemetryValidator.cc`
- Modify: `src/telemetry_hub.h`
- Test: `tests/telemetry_validator_test.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Existing JsonCpp dependency and IDs/topics from the spec.
- Produces: `TelemetryEnvelope { std::string siteId, deviceId; int64_t seq, deviceTimeMs, receivedTimeMs; Json::Value metrics; }`, `ValidationResult { ValidationCode code; std::optional<TelemetryEnvelope> envelope; std::string reason; }`, and `validateTelemetry(std::string_view topic, std::string_view payload, int64_t nowMs)`.

- [x] **Step 1: Write validator unit tests** for valid payload, bad topic/site/device, missing schema version, non-integer/negative sequence, out-of-range timestamp, empty/33-value metrics, non-finite values, and payload over 16 KiB.
- [x] **Step 2: Run `ctest --test-dir build --output-on-failure -R telemetry_validator`; confirm the target is missing or the new cases fail.**
- [x] **Step 3: Implement** topic parsing for `labscope/v1/sites/{site}/devices/{device}/telemetry`, schema version `1`, 1–32 finite numeric metrics, a 16 KiB payload maximum, and configurable timestamp skew.
- [x] **Step 4: Run the focused validator tests**; expected result is all listed valid/invalid cases pass without mutating any device state.
- [x] **Step 5: Commit** as `feat(telemetry): validate versioned mqtt envelopes`.

### Task 4: MQTT consumer, ordered queues, and post-commit ACK contract

**Files:**
- Create: `src/ingest/TelemetrySource.h`
- Create: `src/ingest/IngestionService.h`
- Create: `src/ingest/IngestionService.cc`
- Create: `src/ingest/MqttConsumer.h`
- Create: `src/ingest/MqttConsumer.cc`
- Create: `src/ingest/BoundedDeviceQueue.h`
- Create: `src/ingest/BoundedDeviceQueue.cc`
- Create: `src/ingest/AckPublisher.h`
- Create: `src/ingest/AckPublisher.cc`
- Modify: `src/main.cc`
- Test: `tests/mqtt_ingestion_integration_test.py`

**Interfaces:**
- Consumes: `TelemetryEnvelope` and `ValidationResult` from Task 3; broker configuration from Task 1.
- Produces: `class TelemetrySource { virtual ~TelemetrySource() = default; virtual void start() = 0; virtual void stop() = 0; }`; `enum class IngestionStatus { Persisted, Duplicate, Rejected, RetryableError }; struct IngestionResult { IngestionStatus status; std::string reason; }; using CompletionCallback = std::function<void(IngestionResult)>; enum class QueueResult { Accepted, Full, Stopping }; IngestionService::submit(TelemetryEnvelope, CompletionCallback) -> QueueResult`; `MqttConsumer : public TelemetrySource`; `BoundedDeviceQueue::tryPush(QueuedTelemetry) -> bool`; `DeviceAck { siteId, deviceId, seq, status, confirmedMs }`; `AckPublisher::publish(const DeviceAck&)`. ACK topic `labscope/v1/sites/{site}/devices/{device}/acks`, payload `{schema_version,seq,status,confirmed_ms}`. MQTT attaches a completion callback that publishes only `Persisted`, `Duplicate`, or permanent `Rejected`; `RetryableError` produces no application ACK. Future Modbus sources submit through the same ingestion service.

- [ ] **Step 1: Write broker integration tests** for QoS 1 ingest, authenticated topic scope, per-device in-order dispatch, queue capacity, and ACK status/topic shape. (Deferred to the Compose integration stage; Docker Engine is unavailable in this environment.)
- [ ] **Step 2: Run the tests** with `python -m unittest discover -s tests -p "mqtt_ingestion_integration_test.py" -v`; confirm the current WebSocket-only service has no MQTT consumer.
- [x] **Step 3: Implement Paho MQTT C async v1.3.16 client** using MQTT 5, persistent service session, bounded inflight messages, one ordered queue lane per device, exponential reconnect, and a queue capacity from configuration. The consumer submits through `IngestionService`; MQTT PUBACK remains transport-level; only a successful storage completion may request application `persisted`/`duplicate` ACKs. Publish permanent `rejected` for parseable authenticated messages; never application-ACK transient database failures.
- [ ] **Step 4: Run broker integration tests**; expected result is ordered delivery, bounded queue behavior, and no application `persisted` ACK before the storage completion callback. (C++ build and three focused CTest cases pass; broker-level verification remains blocked by unavailable Docker Engine.)
- [x] **Step 5: Commit** as `feat(ingest): receive mqtt telemetry with ordered backpressure` (`8f6bf1b`; C++ tests/build passed, broker integration still pending).

### Task 5: Transactional PostgreSQL repository and persistence recovery

**Files:**
- Create: `src/storage/TelemetryRepository.h`
- Create: `src/storage/TelemetryRepository.cc`
- Create: `src/storage/TelemetryWriter.h`
- Create: `src/storage/TelemetryWriter.cc`
- Create: `src/storage/DeviceRepository.h`
- Create: `src/storage/DeviceRepository.cc`
- Test: `tests/telemetry_persistence_integration_test.py`

**Interfaces:**
- Consumes: Ordered `TelemetryEnvelope` queue and `AckPublisher` from Task 4; Drogon PostgreSQL `DbClientPtr`.
- Produces: `struct QueuedTelemetry { TelemetryEnvelope envelope; CompletionCallback completion; }; using BatchCompletionCallback = std::function<void(std::vector<IngestionResult>)>; TelemetryRepository::insertBatch(std::vector<QueuedTelemetry>, BatchCompletionCallback)` using Drogon asynchronous DB callbacks; results preserve input order and each item's completion runs exactly once after transaction outcome. A successful transaction writes raw samples, updates device sequence watermarks and aggregates, and appends corresponding durable alarm-processing rows to `telemetry_outbox`; `TelemetryWriter` invokes completion callbacks after commit and never publishes MQTT ACKs itself. `DeviceRepository::reserveSourceSequence(deviceId)` atomically allocates a monotonic sequence for sources such as Modbus.

- [x] **Step 1: Write PostgreSQL integration tests** for atomic batch commit, per-device sequence watermark, duplicate replay, gap count, forced SQL rollback and source sequence allocation. The harness is ready to run when a migrated test database is available.
- [ ] **Step 2: Run the persistence integration tests** against Compose; confirm repository and tables are absent. *(Blocked locally: Docker Desktop engine pipe is inaccessible.)*
- [x] **Step 3: Implement** batch writes using a transaction and `devices.last_seq`; a committed new sequence writes one sample plus its alarm outbox row, equal/lower sequence returns `duplicate`, a gap commits with a gap counter, missing/disabled devices return `rejected`, and transient SQL failure produces no application ACK. On replay after commit, return `duplicate` and let the MQTT completion callback republish the application ACK. `TelemetryWriter` is wired into the runtime when database settings are provided.
- [ ] **Step 4: Run persistence integration tests**; expected result is one stored row after replay, no ACK during SQL failure, and `persisted`/`duplicate` ACK after commit. *(C++ integration harness builds; database execution remains blocked because Docker is unavailable.)*
- [ ] **Step 5: Commit** as `feat(storage): persist telemetry transactionally and replay safely`.

### Task 6: Device and history HTTP APIs plus existing monitor compatibility

**Files:**
- Create: `src/controllers/DeviceController.h`
- Create: `src/controllers/DeviceController.cc`
- Create: `src/controllers/TelemetryController.h`
- Create: `src/controllers/TelemetryController.cc`
- Modify: `src/main.cc`
- Modify: `src/telemetry_hub.h`
- Modify: `src/telemetry_hub.cc`
- Modify: `tests/integration_test.py`

**Interfaces:**
- Consumes: `DeviceRepository` and `TelemetryRepository` from Task 5.
- Produces: `GET /api/v1/devices?site_id=&status=&cursor=&limit=`, `GET /api/v1/devices/{id}`, `GET /api/v1/devices/{id}/latest`, `GET /api/v1/devices/{id}/history?from_ms=&to_ms=&cursor=&limit=`, `GET /api/v1/stats`; default history limit 100, maximum 1,000, maximum time range 24 hours for raw and 30 days for aggregate.

- [x] **Step 1: Write API contract tests** for bounded device pagination, raw/aggregate history limits, persisted latest/history routes, optional DB-mode registration, and committed site-scoped monitor samples. Full HTTP/PostgreSQL integration cases are still pending a runnable Compose database.
- [ ] **Step 2: Run the API tests** against PostgreSQL; runtime cases remain blocked because Docker Engine is unavailable. Static contract tests were run before and after implementation.
- [x] **Step 3: Implement** asynchronous Drogon HTTP handlers with repository-backed responses; retain monitor subscription behavior but source latest committed samples from the persistence service.
- [x] **Step 4: Run build and static API checks**; controller target builds, API contracts pass 4/4, and existing C++ CTest cases pass 5/5. Live HTTP/database behavior remains unverified.
- [ ] **Step 5: Commit** as `feat(api): expose device registry and persisted history`.

### Task 7: Durable MQTT simulator and benchmark report

**Files:**
- Create: `tools/mqtt_device_sim.py`
- Create: `tools/bench_mqtt.py`
- Create: `tools/provision_device.py`
- Create: `requirements-dev.txt`
- Create: `tests/mqtt_device_sim_test.py`
- Create: `tests/benchmark_report_test.py`
- Modify: `.gitignore`
- Create: `docs/benchmarks/README.md`

**Interfaces:**
- Consumes: Telemetry and ACK topic contracts from Task 4; Mosquitto dev credentials from Compose.
- Produces: `provision_device.py create|rotate|revoke --site-id --device-id` provisions device metadata and per-device MQTT credentials/ACL; simulator options `--devices`, `--rate-hz`, `--duration-sec`, `--outbox`, `--fault-profile`; versioned JSON report with host, configuration, sent/persisted/duplicate/rejected, throughput, ACK latency p50/p95/p99, reconnects, retries, and worker errors.

- [x] **Step 1: Write simulator tests** for SQLite persistence across process restart, one outstanding message per device, matching ACK by device/sequence, bounded retry/dead-letter behavior, and deterministic fault injection.
- [x] **Step 2: Run simulator tests** with `python -m unittest discover -s tests -p "mqtt_device_sim_test.py" -v`; confirmed RED before the module existed.
- [x] **Step 3: Add pinned Python MQTT/WebSocket dev dependencies** and implement the device provisioning CLI plus durable SQLite outbox, MQTT QoS 1 publisher/subscriber, deterministic metrics, configurable load, exponential retry, `persisted`/`duplicate` removal, `rejected`/expired retry dead-letter, and JSON benchmark output under `docs/benchmarks/<run-id>.json`.
- [ ] **Step 4: Run simulator tests and a short broker smoke load**; local outbox and report tests pass, CLI/import checks pass, but broker smoke is blocked because Docker Engine is inaccessible. No broker or throughput behavior is claimed as runtime-verified.
- [ ] **Step 5: Commit** as `feat(tools): add durable mqtt simulator and benchmark reports`.

### Task 8: Compose service wiring, operations docs, and capacity acceptance

**Files:**
- Modify: `compose.yaml`
- Create: `docker/app.Dockerfile`
- Create: `docker/web.Dockerfile`
- Create: `config/app.example.yaml`
- Modify: `README.md`
- Modify: `docs/benchmark.md`
- Modify: `docs/benchmarks/README.md`
- Include: `docs/benchmark-result.json` (existing, currently ignored baseline evidence)
- Test: `tests/compose_smoke_test.py`

**Interfaces:**
- Consumes: App, database, broker, migration, frontend placeholder, and simulator from Tasks 1–7.
- Produces: `docker compose up --build` starts a localhost-only development stack, `/healthz` reports dependency readiness, and README documents Linux single-node deployment, configuration, data backup/restore, log rotation, and benchmark reproduction.

- [x] **Step 1: Write Compose smoke checks** for service dependencies, health checks, persistent volumes, no committed secrets, and documented backup/restore commands.
- [x] **Step 2: Run `python -m unittest discover -s tests -p "compose_smoke_test.py" -v`; confirm missing Docker files/configuration fail.** Static checks pass 6/6. (The initial implementation was already present when this execution resumed, so this continuation did not observe a RED run.)
- [x] **Step 3: Wire** app, web UI, Mosquitto, PostgreSQL, migrations, and named volumes; add non-root app and web images, health checks, and a placeholder-only environment template. Preserve the existing benchmark baseline and label it as the legacy in-memory WebSocket path, separate from durable MQTT measurements.
- [ ] **Step 4: Run Compose smoke, including backup/restore persistence, all ingestion integration tests, then the 10-minute 1,000-device × 1 Hz benchmark**; static Compose validation and application tests pass, but runtime/database/broker/backup/restore and capacity checks are blocked because Docker Engine is inaccessible. Do not claim these acceptance results.
- [x] **Step 5: Commit** the deploy wiring checkpoint; runtime acceptance remains outstanding.

**Task 8 configuration ruling:** the plan listed `config/app.example.yaml`, but the application reads deployment settings from environment variables and has no YAML application-config loader. Use `.env.example` as the actual Compose configuration template rather than add an unused YAML file; cost if wrong: a future YAML loader will need a separately specified and tested schema.

