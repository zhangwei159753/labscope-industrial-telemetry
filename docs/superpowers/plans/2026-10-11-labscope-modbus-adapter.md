# LabScope Modbus TCP Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add read-only Modbus TCP polling as a second device source that feeds the existing validated, persisted telemetry pipeline.

**Architecture:** Implement a Modbus poller behind the same `TelemetrySource` boundary as MQTT. Site/device register maps live in PostgreSQL, polling runs on bounded worker threads with timeouts and reconnect backoff, and every poll result is converted to the same `TelemetryEnvelope` and persistence path used by MQTT.

**Tech Stack:** C++20, Drogon 1.9.13, libmodbus 3.1.x, PostgreSQL 16, CMake, Docker Compose, existing simulator/integration test harness.

**Spec:** `docs/superpowers/specs/2026-10-11-labscope-industrial-telemetry-design.md`

## Global Constraints

- This is a later plan and depends on the ingestion foundation plan's `TelemetryEnvelope`, validation, repository, and site/device model.
- Only read Modbus TCP holding/input registers; no writes, coils, control commands, or safety interlock behavior.
- Poll requests have configured connect/read timeouts, bounded retry/backoff, and bounded worker concurrency.
- Register address, function code, data type, byte order, scale, unit, polling interval, and timeout are explicitly configured per measurement point.
- Bad device responses are counted and surfaced as stale/offline state; they cannot block other device polls or the Drogon event loop.
- Modbus samples use the same DB persistence, one-minute aggregation, alarm, WebSocket, and metrics paths as MQTT samples.

## Review Focus

- Signed/unsigned values, float word order, byte order, scaling, and register boundary errors must decode correctly; test each supported representation and invalid address.
- A slow or nonresponsive device must time out without consuming unbounded workers or delaying healthy devices; test timeout and scheduler isolation.
- Reconnects and repeated poll results must not create duplicate samples or false alarm flapping; test restart, sequence generation, and threshold hysteresis.
- Device configuration changes during active polling must apply atomically between polls; test invalid config rejection and safe reload.
- A malicious/malformed endpoint string or out-of-range unit/register configuration must be rejected before network activity; test input validation.

---

### Task 1: Define source abstraction and Modbus configuration schema

**Files:**
- Create: `migrations/V008__modbus_register_maps.sql`
- Create: `src/modbus/RegisterMap.h`
- Create: `src/modbus/RegisterMap.cc`
- Create: `src/modbus/ModbusProfileRepository.h`
- Create: `src/modbus/ModbusProfileRepository.cc`
- Test: `tests/register_map_test.cc`

**Interfaces:**
- Consumes: `TelemetryEnvelope`, `TelemetrySource`, and `IngestionService::submit(TelemetryEnvelope, CompletionCallback) -> QueueResult` from the ingestion foundation plan.
- Produces: `RegisterSpec { name, functionCode, address, dataType, byteOrder, scale, unit }`; `PollProfile { deviceId, host, port, unitId, intervalMs, timeoutMs, registers }`; `ModbusProfileRepository::listEnabled() -> std::vector<PollProfile>` and `replace(const PollProfile&) -> ProfileUpdateResult` using the `TelemetrySource`/`IngestionService` contracts from the foundation plan. Poll sequence numbers come from `DeviceRepository::reserveSourceSequence(deviceId)` so they continue monotonically after process restart.

- [ ] **Step 1: Write register map tests** for address/unit/interval bounds, allowed function codes 3/4, signed and unsigned 16/32-bit decode, float decode, byte/word order, scale, and duplicate metric names.
- [ ] **Step 2: Run `ctest --test-dir build --output-on-failure -R register_map`; confirm the types and migration are absent.**
- [ ] **Step 3: Implement** validated configuration parsing and register decoding with explicit endian conversion; reject unsupported function codes/data types.
- [ ] **Step 4: Run focused register map tests**; expected result is exact values for each test vector and rejection of invalid map inputs before any socket creation.
- [ ] **Step 5: Commit** as `feat(modbus): define source and validated register maps`.

### Task 2: Bounded Modbus poller and ingestion integration

**Files:**
- Create: `src/modbus/ModbusPoller.h`
- Create: `src/modbus/ModbusPoller.cc`
- Create: `src/modbus/ModbusConnection.h`
- Create: `src/modbus/ModbusConnection.cc`
- Create: `src/controllers/ModbusProfileController.h`
- Create: `src/controllers/ModbusProfileController.cc`
- Modify: `CMakeLists.txt`
- Modify: `compose.yaml`
- Test: `tests/modbus_integration_test.py`

**Interfaces:**
- Consumes: `TelemetrySource`, `PollProfile`, and `IngestionService::submit(TelemetryEnvelope, CompletionCallback)` from Task 1/foundation.
- Produces: `ModbusPoller::start/stop/reloadProfiles`, per-profile bounded workers, admin-only `GET/POST/PATCH/DELETE /api/v1/modbus/profiles`, metrics `modbus_polls_total`, `modbus_poll_errors_total`, `modbus_poll_duration_seconds`, and `modbus_active_workers`.

- [ ] **Step 1: Write integration tests** using a local Modbus simulator for normal register reads, conversion to telemetry, per-device sequence generation, timeout, reconnect, and healthy-device progress while another simulator is offline.
- [ ] **Step 2: Run `python -m unittest discover -s tests -p "modbus_integration_test.py" -v`; confirm the Modbus service and dependencies are absent.**
- [ ] **Step 3: Add libmodbus 3.1.x and implement** a bounded scheduler with explicit connect/read deadlines, cancellation on shutdown, reconnect backoff, DB-reserved per-device sequence numbers, and conversion to `TelemetryEnvelope`; submit through `IngestionService::submit(envelope, callback)`; do not call polling from Drogon event-loop threads.
- [ ] **Step 4: Run Modbus integration tests**; expected result is correct persisted samples, timeout isolation, and no growing worker count across reconnect cycles.
- [ ] **Step 5: Commit** as `feat(modbus): poll tcp registers into telemetry pipeline`.

### Task 3: Modbus configuration UI, simulator, and operating guide

**Files:**
- Create: `web/src/views/ModbusProfilesView.vue`
- Create: `web/src/views/ModbusProfilesView.test.ts`
- Create: `tools/modbus_simulator.py`
- Modify: `web/src/api/client.ts`
- Modify: `web/src/router.ts`
- Modify: `web/src/views/SystemHealthView.vue`
- Modify: `README.md`
- Create: `docs/modbus-device-onboarding.md`

**Interfaces:**
- Produces: admin-only CRUD for poll profiles and register maps; profiles are versioned and take effect between poll cycles; simulator supports holding/input registers, values, delay, timeout, and disconnect injection.

- [ ] **Step 1: Write UI and simulator tests** for profile validation, role restrictions, atomic reload, unit/scale display, and timeout/degraded state.
- [ ] **Step 2: Run focused UI and simulator tests**; confirm routes and simulator are absent.
- [ ] **Step 3: Implement** Modbus profile management, simulator fault injection, operator setup guide, and deployment configuration; display data-source type on the device detail and system health pages.
- [ ] **Step 4: Run complete MQTT + Modbus end-to-end suite and the 10-minute mixed-source benchmark**; expected result is MQTT behavior unchanged, Modbus metrics visible, and all accepted samples pass through the same storage/alarm/WS pipeline.
- [ ] **Step 5: Commit** as `feat(modbus): add profile management and onboarding guide`.

### Task 4: Release hardening and GitHub publication

**Files:**
- Create: `.github/workflows/ci.yml`
- Create: `LICENSE` (select a license compatible with the new application and confirm it does not imply relicensing Drogon)
- Create: `NOTICE` (attribute Drogon and other redistributed third-party components as applicable)
- Modify: `README.md`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: completed MQTT operations console and Modbus adapter, benchmark evidence, dependency versions, and repository history.
- Produces: reproducible Linux CI for configure/build/unit and integration checks; clear project scope, architecture, setup, measured benchmark conditions/results, limitations, and upstream attribution; a clean GitHub repository containing only this application and its own documentation, without build outputs, credentials, or copied Drogon framework source.

- [ ] **Step 1: Write release checks** for secret/build-artifact scans, license and notice presence, clean checkout build, Compose startup, and README commands that reproduce deployment and benchmark.
- [ ] **Step 2: Run release checks** on a clean checkout; record the exact toolchain and any checks that require external services.
- [ ] **Step 3: Add CI and release documentation**; confirm the chosen application license and third-party notices are compatible with the upstream Drogon license and dependency licenses.
- [ ] **Step 4: Review the staged repository contents and Git history**; retain benchmark results with their machine/configuration metadata, remove no source history without an explicit reason, and ensure secrets and generated binaries are absent.
- [ ] **Step 5: Publish the completed repository to the user's GitHub account** using the repository name/visibility they specify, then verify the remote URL and pushed commit. GitHub account/repository destination will be resolved when the implementation is ready to publish.

