# 性能验证记录

> 本页记录的是原有内存 WebSocket 演示链路的历史基线，不是当前 MQTT + PostgreSQL 持久化链路的容量结果。两种负载路径和确认语义不同，不能将下方数字用于声称持久化链路已经达到同等吞吐。新链路的测量使用 `tools/bench_mqtt.py`，结果单独保存在 `docs/benchmarks/run-<UTC>.json`。

## 测试条件

- 平台：Windows 11，Intel 20 逻辑 CPU。
- 服务：本项目 `lab_telemetry.exe`，本机 `127.0.0.1:8848`。
- 负载：2 个模拟设备，每个设备 2 个 WebSocket 监控端。
- 目标间隔：1 ms；持续时间：5 s。
- 口径：`samples_sent` 是模拟器成功写入 WebSocket 的条数；`server_accepted_delta` 是服务端接受的条数；监控延迟是监控端收到事件时的本机时间减事件的 `timestamp_ms`。

## 结果

结果文件：[benchmark-result.json](benchmark-result.json)。

| 指标 | 结果 |
| --- | ---: |
| 模拟器发送条数 | 9,952 |
| 实际发送速率 | 1,992.6 条/秒 |
| 服务端接受条数 | 9,952 |
| 服务端拒绝条数 | 0 |
| 监控端观测帧 | 324 |
| 监控延迟 p50 / p95 / p99 | 2.30 / 3.33 / 44.95 ms |

监控端按 50 ms 合并推送最新值，所以观测帧数不等于服务端接受条数；这正是“高频采集”和“可读的实时监控视图”分层后的预期行为。

## 重复验证

```powershell
python tools/demo_load.py --devices 2 --monitors-per-device 2 --interval-ms 1 --duration-sec 10 --start-seq 100000 --output docs/benchmark-result.json
python -m unittest discover -s tests -p "*_test.py" -v
```

每次换机器、换编译配置或换负载，都应保存新的 JSON。没有新记录时，不应在简历中替换现有数字。
