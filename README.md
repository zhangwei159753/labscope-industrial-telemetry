# LabScope

LabScope 是面向工业设备遥测采集与运行监控的 C++ 服务端和 Web 控制台。设备或边缘网关通过 MQTT 上报测点，系统负责数据校验、持久化、历史查询、实时展示和告警处置。

项目基于 Drogon 构建，配套 PostgreSQL、Mosquitto、Vue 3、TypeScript 和 ECharts。服务端按采集、存储、告警、权限和观测等职责组织模块，使用 Docker Compose 启动本地开发环境。

## 工程能力

- MQTT QoS 1 遥测接入，按站点和设备隔离主题及设备凭据。
- 有界采集队列、按设备顺序处理、数据库事务批量写入和背压控制。
- 设备序号幂等处理、重复消息识别和序号缺口统计。
- PostgreSQL 日分区原始数据、分钟级 min/max/avg/count 聚合和历史查询。
- 数据库提交后发送设备专属应用层 ACK；模拟设备以 SQLite outbox 保存待确认消息并支持重试。
- 基于事务 outbox 的告警规则处理，支持阈值、持续时间、恢复回差、冷却时间和人工确认。
- Vue 运维控制台提供设备总览、台账、趋势、告警中心、规则管理和系统健康页面。
- 服务端会话、角色权限、站点授权、CSRF 校验和管理操作审计。
- Prometheus 格式运行指标及健康接口。

## 架构与数据流

~~~mermaid
flowchart LR
    D[设备或边缘网关] -->|MQTT QoS 1 遥测| B[Mosquitto]
    S[设备模拟器 SQLite outbox] -->|MQTT QoS 1 遥测| B
    B --> C[Drogon MQTT Consumer]
    C --> Q[有界队列与设备顺序处理]
    Q --> W[Telemetry Writer]
    W -->|PostgreSQL 事务| DB[(PostgreSQL)]
    DB -->|提交后应用 ACK| A[设备 ACK Topic]
    A --> S
    DB --> O[遥测事务 Outbox]
    O --> P[告警处理器与规则状态机]
    P --> DB
    DB --> R[REST API]
    DB --> H[实时状态与 WebSocket]
    R --> UI[Vue Web 控制台]
    H --> UI
~~~

设备遥测主题为：

~~~text
labscope/v1/sites/{site_id}/devices/{device_id}/telemetry
~~~

设备应用层 ACK 主题为：

~~~text
labscope/v1/sites/{site_id}/devices/{device_id}/acks
~~~

遥测消息示例：

~~~json
{
  "schema_version": 1,
  "seq": 12345,
  "timestamp_ms": 1791710400000,
  "metrics": {
    "temperature_c": 38.4,
    "vibration_mm_s": 2.1
  }
}
~~~

MQTT PUBACK 表示 broker 与服务端之间的传输确认。LabScope 在 PostgreSQL 事务提交后发布应用层 ACK。设备端收到 persisted 或 duplicate 后可从待发队列移除消息；未收到 ACK 时使用原序号和载荷重发。

## 组件

| 组件 | 技术 | 职责 |
| --- | --- | --- |
| 应用服务 | C++20、Drogon | HTTP/REST、WebSocket、MQTT 消费、权限和业务编排 |
| MQTT Broker | Eclipse Mosquitto | 遥测分发、动态设备身份和主题 ACL |
| 数据库 | PostgreSQL 16 | 设备、用户、遥测、聚合、告警和审计数据 |
| Web 控制台 | Vue 3、TypeScript、ECharts | 设备运行监控和告警操作 |
| 模拟与压测工具 | Python、Paho MQTT、SQLite | 设备模拟、持久化 outbox、故障注入和运行报告 |
| 本地部署 | Docker Compose | 应用、Web、Mosquitto、PostgreSQL 和数据库迁移 |

## 快速启动

### 环境要求

本地 Compose 运行需要 Docker Desktop 或 Linux Docker Engine，以及 Docker Compose v2。若需单独开发前端，使用 Node.js 24；若需在 Windows 本机构建 C++ 服务，还需要 Visual Studio 2022、CMake、vcpkg、Python 和 Drogon 1.9.13 源码。

### 启动服务

~~~powershell
Copy-Item .env.example .env
docker compose up --build -d
docker compose ps
~~~

打开 http://127.0.0.1:8080。HTTP、PostgreSQL 和 MQTT 端口默认只绑定本机回环地址。

首次启动后创建管理员账号，密码通过交互提示输入：

~~~powershell
python tools/create_admin.py labadmin
~~~

浏览器登录后，管理员可创建和管理告警规则；operator 可确认其授权站点内的活动告警；viewer 具有只读权限。创建站点用户时可重复指定授权站点：

~~~powershell
python tools/create_web_user.py operator-a operator --site-id plant-a
python tools/create_web_user.py observer-a viewer --site-id plant-a --site-id plant-b
~~~

停止服务并保留数据卷：

~~~powershell
docker compose down
~~~

## 配置

复制 .env.example 为 .env 后，可按本地环境修改以下变量：

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| POSTGRES_DB | labscope | PostgreSQL 数据库名 |
| POSTGRES_USER | labscope | PostgreSQL 用户名 |
| POSTGRES_PASSWORD | 本地开发占位口令 | PostgreSQL 密码 |
| POSTGRES_PORT | 5432 | 本机映射的 PostgreSQL 端口 |
| MQTT_PORT | 1883 | 本机映射的 MQTT 端口 |
| MOSQUITTO_ADMIN_USER | labscope-admin | Broker 管理用户 |
| MOSQUITTO_ADMIN_PASSWORD | 本地开发占位口令 | Broker 管理密码 |
| MOSQUITTO_INGEST_USER | labscope-ingest | 应用采集用户 |
| MOSQUITTO_INGEST_PASSWORD | 本地开发占位口令 | 应用采集密码 |
| LABSCOPE_HTTP_PORT | 8080 | Web 控制台 HTTP 端口 |
| LABSCOPE_ENV | development | 应用运行环境 |
| LABSCOPE_COOKIE_SECURE | 0 | 是否为会话 Cookie 设置 Secure 属性 |
| LABSCOPE_INGEST_QUEUE_CAPACITY | 8192 | 遥测队列容量 |
| LABSCOPE_INGEST_WORKERS | 4 | 遥测写入工作线程数 |
| LABSCOPE_INGEST_BATCH_SIZE | 128 | 单批写入上限 |

.env 包含本地口令，不应提交到版本库。部署环境应设置独立随机凭据，并根据 HTTPS 配置启用 Secure Cookie。

## 设备模拟与压测

先启动 Compose，并在当前 PowerShell 会话中加载 .env 配置，然后创建一台测试设备：

~~~powershell
Get-Content .env | ForEach-Object {
  if ($_ -match '^([^#=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
}
python tools/provision_device.py create --site-id plant-a --device-id device-0001 --credentials-file data/device-credentials.json
~~~

启动模拟设备，以 1 Hz 发送 30 秒：

~~~powershell
python tools/mqtt_device_sim.py --site-id plant-a --credentials data/device-credentials.json --devices 1 --rate-hz 1 --duration-sec 30
~~~

模拟器支持 sensor-spike 和 sequence-gap 故障档案。压测工具可配置设备数、上报频率和运行时长，并生成 JSON 运行报告：

~~~powershell
python tools/bench_mqtt.py --site-id plant-a --credentials data/device-credentials.json --devices 1000 --rate-hz 1 --duration-sec 600
~~~

容量测试需要先创建相应数量的设备凭据。报告字段、运行方式和示例见 docs/benchmarks/README.md。

设备凭据可通过 CLI 轮换或撤销：

~~~powershell
python tools/provision_device.py rotate --site-id plant-a --device-id device-0001 --credentials-file data/device-credentials.json
python tools/provision_device.py revoke --site-id plant-a --device-id device-0001 --credentials-file data/device-credentials.json
~~~

## Web 控制台开发

~~~powershell
cd web
npm ci
npm run dev
~~~

本地 Vite 服务将 /api、/healthz 和 /ws 请求代理到 http://127.0.0.1:8848。构建静态资源并执行前端测试：

~~~powershell
npm test -- --run
npm run build
~~~

生产静态资源由 Nginx 提供，并将 REST、健康检查、指标和 WebSocket 请求转发到 Drogon 服务。

## C++ 本地构建

项目默认从相邻目录引用 Drogon 源码 ../drogon-master/drogon-master。若源码位于其他位置，请通过 DROGON_SOURCE_DIR 指定。Windows 示例：

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DDROGON_SOURCE_DIR="C:/src/drogon" -DBUILD_TESTING=ON
cmake --build build --config Debug --target lab_telemetry --parallel 8
~~~

容器构建会检出 Drogon v1.9.13 及其子模块，并构建服务端镜像。

## 数据与运行观测

遥测原始数据按日分区保存，默认保留 24 小时；系统同时保存每分钟的 min/max/avg/count 聚合，默认保留 30 天。保留维护由应用定期执行。

- GET /healthz：检查 HTTP 服务是否响应。
- GET /api/v1/system/health：读取 PostgreSQL、MQTT、采集队列、实时订阅和维护调度状态，需要登录。
- GET /metrics：以 Prometheus exposition 格式输出接收、持久化、重试、队列、延迟、连接、序号缺口和活动告警指标。

指标避免使用设备、站点和用户标识作为标签。部署时可通过监控网络采集 /metrics。

更多架构决策、遥测契约和运维说明见 docs/superpowers/specs、docs/superpowers/plans 和 docs/benchmarks。
