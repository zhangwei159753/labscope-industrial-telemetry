# LabScope Operations Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver an authenticated industrial operations console for device fleet monitoring, live trends, alarm handling, and ingestion health.

**Architecture:** Extend the Drogon modular monolith with PostgreSQL-backed users, sessions, device access, alarm rules/events, and Prometheus metrics. Build a Vue 3/TypeScript frontend served through a same-origin reverse proxy; use bounded REST queries for history and authenticated WebSockets for coalesced live state.

**Tech Stack:** C++20, Drogon 1.9.13, PostgreSQL 16, Vue 3, TypeScript, Vite, ECharts, Vitest, Playwright, Prometheus exposition format.

**Spec:** `docs/superpowers/specs/2026-10-11-labscope-industrial-telemetry-design.md`

## Global Constraints

- Keep the Drogon framework as an unchanged dependency; business and UI code stays in `lab-telemetry-monitor`.
- The browser uses a revocable server-side session in an HttpOnly, SameSite cookie; production uses Secure cookies and CSRF protection for mutations.
- Roles are `admin`, `operator`, and `viewer`; all management actions write an audit record.
- Every REST history request has bounded time range, pagination, and maximum rows.
- WebSocket subscriptions are authorized by site/device and emit coalesced latest values, not one frame per sample.
- The console displays `unknown`, `online`, `stale`, or `disabled` distinctly; it does not issue control commands.

## Review Focus

- Anonymous users and lower-privilege roles must not access management/alarm mutations; test every role against each write route.
- Expired/revoked cookies and missing/invalid CSRF tokens must fail closed; test expired session, logout, revocation, and cross-site mutation.
- A user cannot subscribe to another site's device by changing a WebSocket query/topic; test unauthorized site/device subscriptions.
- Alarm thresholds near boundaries must honor duration, hysteresis, and cooldown rather than flap; test just-below, crossing, recovery, and repeated crossing.
- Slow or disconnected browsers must not accumulate queued telemetry or leak subscriptions; test bounded coalescing and disconnect cleanup.

---

### Task 1: Frontend build scaffold and API client

**Files:**
- Create: `web/package.json`
- Create: `web/package-lock.json`
- Create: `web/tsconfig.json`
- Create: `web/vite.config.ts`
- Create: `web/src/main.ts`
- Create: `web/src/api/client.ts`
- Create: `web/src/router.ts`
- Modify: `docker/web.Dockerfile`
- Modify: `.gitignore`
- Test: `web/src/api/client.test.ts`

**Interfaces:**
- Consumes: REST route contracts from the ingestion foundation plan.
- Produces: typed `ApiClient` methods for `login`, `logout`, `getOverview`, `listDevices`, `getDeviceHistory`, `listAlerts`, `acknowledgeAlert`, and `getSystemHealth`.

- [x] **Step 1: Write API client tests** for JSON response parsing, 400/401/403/5xx mapping, cookie credentials, CSRF header on writes, and URL-encoded pagination/filter parameters.
- [x] **Step 2: Run `cd web; npm test -- --run src/api/client.test.ts`; confirm methods are missing.** RED observed: login/me/listAlerts methods were missing (6 failing cases).
- [x] **Step 3: Reuse the existing Vite/Vue/TypeScript scaffold and implement** typed same-origin auth, site overview, alert list/acknowledgment, and system-health API methods. CSRF stays in module memory and session cookies remain browser-managed; server errors retain HTTP status through `ApiError`.
- [x] **Step 4: Run the focused Vitest suite and `npm run build`; expected result is passing tests and emitted static assets.** Focused suite 9/9 passed; TypeScript and Vite production build passed.
- [x] **Step 5: Commit** as `feat(web): add auth and alarm API client contracts` (`8f84a90`; the scaffold was delivered in foundation Task 8).

### Task 2: User login, server-side sessions, roles, and CSRF

**Files:**
- Create: `migrations/V005__web_auth_sessions.sql`
- Create: `src/auth/PasswordHasher.h`
- Create: `src/auth/PasswordHasher.cc`
- Create: `src/auth/SessionStore.h`
- Create: `src/auth/SessionStore.cc`
- Create: `src/auth/Authorization.h`
- Create: `src/auth/Authorization.cc`
- Create: `src/controllers/AuthController.h`
- Create: `src/controllers/AuthController.cc`
- Create: `tools/create_admin.py`
- Modify: `CMakeLists.txt`
- Modify: `vcpkg.json`
- Test: `tests/auth_integration_test.py`

**Interfaces:**
- Consumes: Phase 1 `sites`/`devices` tables and Drogon PostgreSQL client.
- Produces: `POST /api/v1/auth/login`, `POST /api/v1/auth/logout`, `GET /api/v1/auth/me`; session cookie is HttpOnly/SameSite, Secure outside development, and mutation requests require a session-bound CSRF token. First admin is explicitly provisioned by CLI/environment; never seed a public default password.
- Produces: `Role { Admin, Operator, Viewer }`, `SessionStore::create(userId)`, `SessionStore::resolve(cookie)`, `SessionStore::revoke(cookie)`, `Authorization::requireRole(request, minimumRole)`.

- [x] **Step 1: Write auth tests** for Argon2id password verification, fail-closed role hierarchy, session schema and route/cookie/CSRF/admin CLI contracts. Live session/database cases remain in the Compose integration harness.
- [x] **Step 2: Run `python -m unittest discover -s tests -p "auth_integration_test.py" -v`; confirm routes and migration are absent.** Initial RED observed for missing migration and auth source; static contracts pass 5/5 after implementation.
- [x] **Step 3: Implement** Argon2id password hashes via vcpkg libsodium, random 256-bit opaque session IDs stored only as SHA-256 hashes, eight-hour PostgreSQL sessions with revocation and expiry, HttpOnly/SameSite cookies and production Secure policy, session-bound CSRF checks, centralized role hierarchy, failed-login lockout, and a getpass-based first-admin CLI that does not put the password in argv.
- [ ] **Step 4: Run auth integration tests**; unit/contract checks pass, but PostgreSQL-backed login/session expiry/revocation/cookie/CSRF HTTP cases and complete route role matrix are pending because Docker Engine is inaccessible. Route-level role enforcement continues in Task 3.
- [x] **Step 5: Commit** auth foundation checkpoint; route authorization and DB-backed acceptance remain pending Task 3/runtime verification.

### Task 3: Device registry, site overview, and authenticated live WebSocket

**Files:**
- Create: `src/controllers/SiteController.h`
- Create: `src/controllers/SiteController.cc`
- Modify: `src/controllers/DeviceController.h`
- Modify: `src/controllers/DeviceController.cc`
- Create: `src/controllers/LiveMonitorSocket.h`
- Create: `src/controllers/LiveMonitorSocket.cc`
- Modify: `src/telemetry_hub.h`
- Modify: `src/telemetry_hub.cc`
- Test: `tests/live_monitor_authorization_test.py`

**Interfaces:**
- Consumes: Authenticated `HttpRequest`, `TelemetryRepository`, and site/device rows from Phase 1.
- Produces: `GET /api/v1/overview?site_id=`, `GET /api/v1/sites`, `GET /api/v1/devices` with filters; admin-only `POST/PATCH /api/v1/sites` and `POST/PATCH /api/v1/devices` provision or update devices through the broker ACL service; `/ws/v1/monitor?site_id=&device_id=` requires an authenticated session and authorization for that scope.

- [ ] **Step 1: Write tests** for site/device pagination, last-seen status, admin/operator/viewer device mutation matrix, credential rotation/revocation, anonymous WebSocket rejection, cross-site subscription denial, authorized subscription, coalesced update delivery, and disconnect cleanup.
- [ ] **Step 2: Run the focused live monitor tests**; confirm current public `/ws/monitor` lacks session and site authorization.
- [ ] **Step 3: Implement** site-scoped read endpoints and replace the public query-parameter WebSocket with an authenticated subscription registry that stores weak connections and emits at most one latest state per configured 50 ms interval.
- [ ] **Step 4: Run live monitor tests**; expected result is authorized clients see only their scope, disconnected clients are removed, and update frequency is bounded.
- [ ] **Step 5: Commit** as `feat(monitor): add authorized site and device views`.

### Task 4: Alarm rules, event lifecycle, and audit trail

**Files:**
- Create: `migrations/V006__alarm_rules_events.sql`
- Create: `migrations/V007__audit_log.sql`
- Create: `src/alarms/AlarmRule.h`
- Create: `src/alarms/AlarmEvaluator.h`
- Create: `src/alarms/AlarmEvaluator.cc`
- Create: `src/alarms/AlarmProcessor.h`
- Create: `src/alarms/AlarmProcessor.cc`
- Create: `src/alarms/AlarmRepository.h`
- Create: `src/alarms/AlarmRepository.cc`
- Create: `src/controllers/AlarmController.h`
- Create: `src/controllers/AlarmController.cc`
- Test: `tests/alarm_evaluator_test.cc`
- Test: `tests/alarm_integration_test.py`

**Interfaces:**
- Consumes: `TelemetryEnvelope`, `IngestionService` completion callbacks, and authorization service from Task 2.
- Produces: `AlarmRule { id, siteId, deviceId, metric, direction, threshold, durationMs, hysteresis, cooldownMs, severity, enabled }`; `AlarmEvaluator::onSample(const TelemetryEnvelope&)`; `AlarmProcessor` reads committed samples from `telemetry_outbox`, applies rule/event changes and marks each outbox row processed in the same PostgreSQL transaction; `POST /api/v1/alarms/rules`, `GET /api/v1/alarms/events`, `POST /api/v1/alarms/events/{id}/ack`.

- [ ] **Step 1: Write evaluator tests** for below/equal/above threshold, duration crossing, hysteresis recovery, cooldown suppression, disabled rule, and duplicate sample idempotence.
- [ ] **Step 2: Run evaluator and API tests**; confirm rule/evaluator endpoints are absent.
- [ ] **Step 3: Implement** a restart-safe outbox worker that processes committed samples in order and atomically persists rule evaluation state, event transitions `active -> acknowledged -> recovered` (or `active -> recovered`), and the processed marker. Store acknowledgment user/time/remark and its audit event in the same transaction. Replays after a worker restart must not create duplicate events.
- [ ] **Step 4: Run alarm tests**; expected result is deterministic event transitions, no duplicate event on replay, and unauthorized mutation denied.
- [ ] **Step 5: Commit** as `feat(alarms): add hysteresis rules and acknowledged events`.

### Task 5: Operations dashboard and device detail pages

**Files:**
- Create: `web/src/views/OverviewView.vue`
- Create: `web/src/views/DevicesView.vue`
- Create: `web/src/views/DeviceDetailView.vue`
- Create: `web/src/components/DeviceStatusBadge.vue`
- Create: `web/src/components/TrendChart.vue`
- Create: `web/src/components/LiveMetricCards.vue`
- Create: `web/src/stores/liveTelemetry.ts`
- Create: `web/src/views/OverviewView.test.ts`
- Modify: `web/src/router.ts`

**Interfaces:**
- Consumes: Auth and site/device/history/live API clients from Tasks 1–3.
- Produces: responsive Overview, Devices, and Device Detail routes; latest values delivered through a reconnecting `LiveTelemetryStore` with one socket per selected site/device view.

- [ ] **Step 1: Write component tests** for status states, pagination/filter application, no-data state, reconnect status, live values, and history window selection.
- [ ] **Step 2: Run `cd web; npm test -- --run src/views/OverviewView.test.ts`; confirm view components are absent.**
- [ ] **Step 3: Implement** the three pages with Chinese UI copy, trend charts using one-minute aggregates, accessible status labels, loading/error/empty states, and bounded live updates.
- [ ] **Step 4: Run component tests and `npm run build`; expected result is all route states render and the production bundle builds.**
- [ ] **Step 5: Commit** as `feat(web): add live device fleet and trend views`.

### Task 6: Alarm center and role-aware operations flows

**Files:**
- Create: `web/src/views/AlarmsView.vue`
- Create: `web/src/views/AlarmRulesView.vue`
- Create: `web/src/components/AlarmStateBadge.vue`
- Create: `web/src/views/AlarmsView.test.ts`
- Modify: `web/src/router.ts`
- Modify: `web/src/api/client.ts`

**Interfaces:**
- Consumes: Alarm routes and role data from Tasks 2 and 4.
- Produces: alarm filtering by site/device/severity/state, rule creation/editing, event acknowledgment with processing note, and UI permission guards aligned with server authorization.

- [ ] **Step 1: Write UI tests** for alert filters, `active/acknowledged/recovered` states, operator acknowledgment, viewer read-only state, and server-side 403 handling.
- [ ] **Step 2: Run focused alarm view tests**; confirm the views and API methods are absent.
- [ ] **Step 3: Implement** Alarm Center and Rules pages; present server errors in the view and do not rely on client guards for authorization.
- [ ] **Step 4: Run alarm UI tests and `npm run build`; expected result is role-aware display and successful bundle generation.**
- [ ] **Step 5: Commit** as `feat(web): add alarm management and acknowledgment flows`.

### Task 7: Runtime metrics, system status, and end-to-end acceptance

**Files:**
- Create: `src/metrics/TelemetryMetrics.h`
- Create: `src/metrics/TelemetryMetrics.cc`
- Create: `src/controllers/MetricsController.h`
- Create: `src/controllers/MetricsController.cc`
- Create: `web/src/views/SystemHealthView.vue`
- Create: `tests/metrics_integration_test.py`
- Create: `tests/e2e/operations_console.spec.ts`
- Modify: `compose.yaml`
- Modify: `README.md`

**Interfaces:**
- Produces: `GET /metrics` with ingest rate, queue depth, insert latency, committed/rejected/duplicate/gap/retry counts, MQTT state, active WebSocket clients, and alert events; `GET /api/v1/system/health` returns dependency and maintenance status.

- [ ] **Step 1: Write metrics and end-to-end tests** for counter/gauge values, system dependency degradation, login, device live view, alarm firing, authorized acknowledgment, and viewer denial.
- [ ] **Step 2: Run focused metrics/E2E tests**; confirm metrics and health page are absent.
- [ ] **Step 3: Implement** low-cardinality Prometheus metrics and the system status page; prohibit device IDs as metric labels; wire frontend build into Compose.
- [ ] **Step 4: Run full C++/API/UI integration suite and the 10-minute benchmark**; expected result is no authorization failures for valid roles, no cross-site data, latest WebSocket sample displayed within 2 seconds under normal benchmark load, and a complete versioned report with system health metrics.
- [ ] **Step 5: Update README** with role matrix, account setup, TLS/CSRF/session configuration, alarm workflow, and metric catalog; commit as `feat(ops): expose runtime health and document operating flows`.

