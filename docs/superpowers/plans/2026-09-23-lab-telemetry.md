# 科研设备遥测与实时监控 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付可构建、可演示、可测试的 Drogon 科研设备遥测应用及真实简历证据。

**Architecture:** 独立 C++ 应用依赖本地 Drogon；设备通过 WebSocket 上报，服务端保存有界短时记录并按 50 ms 周期向监控 WebSocket 推送最新值；HTTP 提供查询与统计，浏览器展示。

**Tech Stack:** C++20、Drogon 1.9.13、CMake、Python 3、websocket-client、原生 HTML/CSS/JS。

**Spec:** `docs/superpowers/specs/2026-09-23-lab-telemetry-design.md`

## Global Constraints

- 框架源码位于 `../drogon-master/drogon-master`，业务代码不得改写上游框架功能。
- 设备数据为模拟数据，1 ms 是目标采样间隔，所有性能值实测后才可写进简历。
- 单进程、本机监听、256 条每设备内存记录、最多 1024 设备、16 KiB 单条消息。
- 浏览器端每设备最多 20 次/秒更新，不能将它解释为每个采样点均已展示。

## Review Focus

- 同一设备重复或倒退的序号必须拒绝，且不改变最新值。
- 不同设备相同序号互不影响，订阅消息也不得串台。
- 非法 JSON、非数值指标或过大消息必须拒绝且服务继续运行。
- 监控端断线后订阅数应下降。
- 发送端达不到目标间隔时，报告实际速率而非声称达标。

---

### Task 1: 构建闭环

**Files:** `CMakeLists.txt`, `src/main.cc`, `README.md`

**Interfaces:** CMake 生成 `lab_telemetry`；服务默认监听 `127.0.0.1:8848`。

- [ ] 配置本地 Drogon 与 Trantor 依赖；验证最小服务可编译和启动。
- [ ] 若构建失败，定位缺失依赖并修复构建说明。

### Task 2: 遥测接入与数据语义

**Files:** `src/telemetry_hub.h`, `src/telemetry_hub.cc`, `src/main.cc`, `tests/integration_test.py`

**Interfaces:** `TelemetryHub::ingest(deviceId, Json::Value)` 返回接受/拒绝；`latest(deviceId)` 和 `stats()` 返回 JSON；`/ws/ingest` 接收设备数据。

- [ ] 先写集成测试：有效数据、重复/倒退序号、不同设备同序号、非法指标、超限消息。
- [ ] 运行测试并确认因功能缺失而失败。
- [ ] 实现校验、每设备递增序号、256 条环形记录、统计接口。
- [ ] 运行测试并确认通过。

### Task 3: 多客户端监控

**Files:** `src/telemetry_hub.h`, `src/telemetry_hub.cc`, `src/main.cc`, `tests/integration_test.py`

**Interfaces:** `/ws/monitor?device_id=...` 订阅单设备，连接时发送最新值；分发周期 50 ms。

- [ ] 先写双客户端同设备与不同设备隔离测试，确认失败。
- [ ] 实现订阅注册、周期最新值推送和断线清理。
- [ ] 运行测试并确认通过。

### Task 4: 模拟器与监控页

**Files:** `tools/simulator.py`, `web/index.html`, `web/app.css`, `web/app.js`, `tests/simulator_test.py`

**Interfaces:** 模拟器支持设备数、间隔、持续时间参数并报告实际速率；网页显示设备、指标、趋势、状态与统计。

- [ ] 测试模拟器的消息格式与实际速率统计。
- [ ] 实现模拟器和监控页面，使用本机服务演示多窗口订阅。

### Task 5: 证据与简历材料

**Files:** `README.md`, `docs/benchmark.md`, `导学-科研遥测.md`, `面经-科研遥测.md`

**Interfaces:** README 有构建、启动、演示、测试命令；benchmark 记录机器、命令、原始结果；ASu 文档区分已证实与待测。

- [ ] 运行集成测试与可重复的演示负载，记录真实结果。
- [ ] 基于源码和结果写项目导学、面经及简历候选表述；不编造个人职责或性能数据。
