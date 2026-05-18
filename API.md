# karo-x2-sdk-cpp 接口文档

Karo X2 机器人 C++ SDK 对外接口参考。所有公开符号在命名空间 `karo::sdk` 下，
头文件统一通过 `#include <karo/sdk/robot.hpp>` 引入。

- 适用版本：SDK 3.3.x
- 传输：WSS + mTLS，机器人 LAN 端口固定 4434
- 线程安全：`Robot` 的所有方法（构造 / 析构除外）可从任意线程调用

---

## 目录

- [快速开始](#快速开始)
- [Robot](#robot)
- [连接配置](#连接配置)
- [连接生命周期](#连接生命周期)
- [订阅](#订阅)
- [控制命令](#控制命令)
- [导航任务](#导航任务)
- [安全事件](#安全事件)
- [错误处理](#错误处理)
- [诊断](#诊断)
- [数据类型](#数据类型)

---

## 快速开始

```cpp
#include <karo/sdk/robot.hpp>

karo::sdk::ConnectOptions opts;
opts.host = "192.168.10.10";
opts.cert.cert_pem = /* ... */;
opts.cert.key_pem  = /* ... */;
opts.cert.ca_pem   = /* ... */;
opts.client_id     = "my-app/1.0";

karo::sdk::Robot robot(opts);

robot.SetConnectionStateHandler([](auto old_s, auto new_s, auto& info) {
  // Idle → Connecting → Ready
});

robot.Connect();   // 异步，立即返回

auto sub = robot.SubscribeRobotStatus(1.0, [](const karo::sdk::RobotStatus& s) {
  // 处理状态推送
});

auto r = robot.CmdVel(0.2, 0.0, 0.0);

robot.Close();
```

---

## Robot

SDK 的唯一入口。采用两阶段使用模式：先构造（仅校验参数，不发起网络），注册回调，
再调用 `Connect()` 异步连接。

### 构造与析构

| 签名 | 说明 |
|---|---|
| `explicit Robot(const ConnectOptions& opts)` | 构造。参数非法时抛 `SdkException`。 |
| `~Robot()` | 析构，自动调用 `Close()`。 |

`Robot` 不可拷贝。

### 回调注册

在 `Connect()` 之前注册才能捕获最早的 `Idle → Connecting` 转移。

| 签名 | 说明 |
|---|---|
| `void SetConnectionStateHandler(ConnectionStateHandler handler)` | 注册连接状态变化回调 |
| `void SetLogHandler(LogHandler handler)` | 注册 SDK 日志回调 |

### 连接控制

| 签名 | 说明 |
|---|---|
| `void Connect()` | 异步发起连接，立即返回。非 `Idle` 状态调用为 no-op。 |
| `void Close()` | 终止连接，进入 `Shutdown` 终态。幂等。 |

### 状态查询

| 签名 | 说明 |
|---|---|
| `ConnectionState state() const noexcept` | 当前连接状态 |
| `bool IsConnected() const noexcept` | 等价于 `state() == Ready` |
| `std::optional<RobotInfo> info() const` | 握手成功后的机器人信息；未握手成功返回 `nullopt` |

### 订阅

| 签名 | 说明 |
|---|---|
| `std::unique_ptr<Subscription> SubscribeRobotStatus(double desired_hz, RobotStatusCallback on_data, StatusCallback on_status = {})` | 订阅 `robot.state`，回调收到 typed `RobotStatus` |
| `std::unique_ptr<Subscription> SubscribeTaskEvents(TaskEventCallback on_data, StatusCallback on_status = {})` | 订阅 `tasks.events`，回调收到 `TaskEvent` |
| `std::unique_ptr<Subscription> SubscribeSafetyEvents(SafetyEventCallback on_data, StatusCallback on_status = {})` | 订阅 `robot.safety`，回调收到 `SafetyEvent` |

- 非 `Ready` 状态也可调用：SDK 记录订阅意图，转 `Ready` 后自动建立。
- `desired_hz <= 0` 表示使用服务端默认频率。

### 控制命令

| 签名 | 说明 |
|---|---|
| `CmdVelResult CmdVel(double linear_x, double linear_y, double angular_z)` | 速度控制（单位同 ROS `geometry_msgs::Twist`） |
| `EmergencyStopResult EmergencyStop(bool engage, const std::string& reason = "")` | 软急停：`engage=true` 触发，`false` 解除 |
| `std::chrono::milliseconds Ping()` | 诊断 RTT；非 `Ready` 返回 `-1ms` |

### 任务控制

| 签名 | 说明 |
|---|---|
| `CreateTaskResult CreateNavigationTaskToMarker(const std::string& marker_id)` | 创建导航任务，目标为已存点位 |
| `CreateTaskResult CreateNavigationTaskToPose(double x, double y, double theta)` | 创建导航任务，目标为原始位姿 |
| `TaskCommandResult CancelTask(const std::string& task_id)` | 取消任务 |
| `TaskCommandResult PauseTask(const std::string& task_id)` | 暂停任务 |
| `TaskCommandResult ResumeTask(const std::string& task_id)` | 恢复已暂停的任务 |

### 诊断

| 签名 | 说明 |
|---|---|
| `Diagnostics GetDiagnostics() const` | 当前诊断快照 |

### 全局函数

| 签名 | 说明 |
|---|---|
| `const char* karo::sdk::sdk_version()` | SDK 版本字符串 |

---

## 连接配置

### ConnectOptions

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `host` | `std::string` | `"192.168.10.10"` | 机器人 LAN 地址，不含端口 |
| `cert` | `CertCredentials` | — | X.509 证书认证（优先） |
| `access_token` | `AccessTokenCredentials` | — | Access Token 认证（备用） |
| `client_id` | `std::string` | `"karo-sdk-cpp"` | 客户端标识，建议 `<app>/<version>` |
| `sdk_version` | `std::string` | `""` | 留空时握手用库内置版本，一般无需覆盖 |
| `connect_timeout` | `milliseconds` | `5s` | 单次连接超时 |
| `heartbeat_interval` | `milliseconds` | `10s` | 心跳间隔，`0` = 关闭心跳 |
| `ping_timeout` | `milliseconds` | `3s` | 单次 Ping 响应超时 |
| `max_missed_pings` | `uint32_t` | `3` | 连续未响应判死阈值 |
| `reconnect_max_attempts` | `int32_t` | `-1` | `-1` 无限重连，`0` 不重连，`>0` 上限次数 |
| `reconnect_base_delay` | `milliseconds` | `500ms` | 重连退避基准 |
| `reconnect_max_delay` | `milliseconds` | `30s` | 重连退避上限 |
| `reconnect_jitter_ratio` | `double` | `0.3` | 退避抖动比例 |
| `reconnect_replay_history` | `bool` | `true` | 重连后尝试补发历史 |
| `history_limit` | `uint32_t` | `1000` | 单次补发上限 |
| `subscribe_ack_timeout` | `milliseconds` | `5s` | 订阅确认超时 |
| `subscription_queue_size` | `uint32_t` | `100` | 单订阅回调队列长度 |
| `drop_policy` | `DropPolicy` | `DropOldest` | 队列满时的丢弃策略 |
| `callback_mode` | `CallbackMode` | `Dispatched` | 数据回调派发模式 |
| `cmdvel_rpc_timeout` | `milliseconds` | `200ms` | `CmdVel` RPC 超时 |
| `estop_rpc_timeout` | `milliseconds` | `2s` | `EmergencyStop` RPC 超时 |

### CertCredentials

| 字段 | 类型 | 说明 |
|---|---|---|
| `cert_pem` | `std::string` | 客户端证书（PEM） |
| `key_pem` | `std::string` | 客户端私钥（PEM） |
| `ca_pem` | `std::string` | 校验服务端证书的 CA（PEM） |
| `insecure_skip_verify` | `bool` | 跳过服务端证书校验，仅测试用，生产保持 `false` |

### AccessTokenCredentials

| 字段 | 类型 | 说明 |
|---|---|---|
| `token` | `std::vector<uint8_t>` | 122 字节 Access Token |

---

## 连接生命周期

### ConnectionState

| 取值 | 说明 |
|---|---|
| `Idle` | 初始态，未连接 |
| `Connecting` | 首次连接中 |
| `Ready` | 已连接可用 |
| `TransientFailure` | 临时故障，将自动重连 |
| `Reconnecting` | 重连中（含重订阅恢复） |
| `Shutdown` | 已主动关闭，终态 |
| `Fatal` | 不可恢复错误，终态，不再重连 |

### StateChangeInfo

`ConnectionStateHandler` 回调参数。

| 字段 | 类型 | 说明 |
|---|---|---|
| `reason` | `std::string` | 状态变化原因 |
| `last_error_code` | `ErrorCode` | 最近一次错误码 |
| `last_error_message` | `std::string` | 最近一次错误信息 |
| `next_retry_in_ms` | `milliseconds` | 距下次重连的时间 |
| `reconnect_attempt` | `uint32_t` | 当前重连尝试次数 |
| `last_action` | `std::string` | 最近一次内部动作 |

### ConnectionStateHandler

```cpp
using ConnectionStateHandler = std::function<void(
  ConnectionState old_state,
  ConnectionState new_state,
  const StateChangeInfo& info)>;
```

---

## 订阅

### Subscription

`Subscribe*` 返回的句柄。建议主动调用 `Unsubscribe()`；句柄析构会兜底取消订阅。

| 方法 | 说明 |
|---|---|
| `std::string topic() const` | 订阅的 topic 名 |
| `double desired_hz() const` | 请求频率 |
| `SubscriptionState state() const` | 当前订阅状态 |
| `uint64_t last_seq() const` | 最近收到的序列号 |
| `uint64_t drop_count() const` | 队列丢弃计数 |
| `ErrorCode error() const` | 最近错误码 |
| `ErrorCode Unsubscribe()` | 主动取消订阅 |

### StreamStatus

`StatusCallback` 回调参数。

| 字段 | 类型 | 说明 |
|---|---|---|
| `kind` | `StreamStatusKind` | 事件类型 |
| `reason` | `std::string` | 说明 |
| `gap_count` | `std::optional<uint64_t>` | 丢帧数（`Gap` / `Resumed` 时有意义） |
| `error_code` | `ErrorCode` | 错误码 |

---

## 控制命令

### CmdVel

```cpp
CmdVelResult CmdVel(double linear_x, double linear_y, double angular_z);
```

- 客户端令牌桶限频 ≤ 20 Hz，超频本地丢弃并返回 `RateLimited`。
- 需要凭据授予 `chassis_control` 能力，否则返回 `CapabilityDenied`。
- 服务端 0.5s watchdog：停止发送后机器人自动归零速度。
- 断连时立即返回 `Disconnected`，**不重试、不排队**。

### EmergencyStop

```cpp
EmergencyStopResult EmergencyStop(bool engage, const std::string& reason = "");
```

- `engage=true` 触发的软急停，会在重连成功后**自动重放**；`engage=false` 不重放。

---

## 导航任务

任务相关的全部接口需要凭据授予 `task_control` 能力，否则返回 `CapabilityDenied`。

任务生命周期由服务端维护，客户端创建任务后拿到 `task_id`，后续
`CancelTask` / `PauseTask` / `ResumeTask` 均以 `task_id` 为入参；任务状态变化
通过 `SubscribeTaskEvents` 推送。

### CreateNavigationTaskToMarker / CreateNavigationTaskToPose

```cpp
CreateTaskResult CreateNavigationTaskToMarker(const std::string& marker_id);
CreateTaskResult CreateNavigationTaskToPose(double x, double y, double theta);
```

- `CreateNavigationTaskToMarker`：导航到机器人地图上已标记的点位。
- `CreateNavigationTaskToPose`：导航到原始位姿 `(x, y, theta)`，`x` / `y` 为 map
  坐标系位置（米），`theta` 为朝向（弧度）。
- 成功时 `CreateTaskResult.task_id` 为新任务 ID。
- `marker_id` 为空字符串返回 `NavMarkerNotFound`。
- 机器人当前已有任务在执行时返回 `TaskRunning`（需先取消）。

### CancelTask / PauseTask / ResumeTask

```cpp
TaskCommandResult CancelTask(const std::string& task_id);
TaskCommandResult PauseTask(const std::string& task_id);
TaskCommandResult ResumeTask(const std::string& task_id);
```

- 三者均为幂等：对已处于目标状态的任务重复调用返回成功。
- `PauseTask` 仅对 `Running` 状态的任务有效；`ResumeTask` 仅对 `Paused` 状态有效，
  否则返回 `TaskInvalidState`。`task_id` 不存在时返回 `TaskNotFound`。
- 暂停的任务若长时间未恢复，服务端会以 `TaskCancelReason::PauseTimeout` 自动取消。

### SubscribeTaskEvents

```cpp
std::unique_ptr<Subscription> SubscribeTaskEvents(
  TaskEventCallback on_data,
  StatusCallback on_status = {});
```

- 订阅 `tasks.events`（`Event` 语义 topic）。
- 每次任务状态变化推送一份完整的 `TaskEvent` 快照。
- `using TaskEventCallback = std::function<void(const TaskEvent&)>;`

---

## 安全事件

### SubscribeSafetyEvents

```cpp
std::unique_ptr<Subscription> SubscribeSafetyEvents(
  SafetyEventCallback on_data,
  StatusCallback on_status = {});
```

- 订阅 `robot.safety`，机器人安全状态变化时推送 `SafetyEvent`。
- 需要凭据授予 `telemetry_read` 能力。
- `using SafetyEventCallback = std::function<void(const SafetyEvent&)>;`

---

## 错误处理

- 构造 `Robot` 时参数非法 → 抛 `SdkException`。
- 业务异步路径（`Connect` / `CmdVel` / `EmergencyStop` / 订阅 / 任务控制）
  **不抛异常**，通过返回值或回调携带 `ErrorCode`。

### ErrorCode

| 取值 | 编号 | 分类 | 含义 |
|---|---|---|---|
| `Ok` | 0 | — | 无错误 |
| `AuthFailed` | 1 | Fatal | 认证失败（证书无效 / 过期 / 已吊销） |
| `AuthCnNotAllowed` | 2 | Fatal | 证书未被授权（不在机器人允许的应用列表中） |
| `AuthSerialRevoked` | 3 | Fatal | 证书序列号已被吊销 |
| `AuthTokenInvalid` | 4 | Fatal | Access Token 无效 |
| `AuthTokenExpired` | 5 | Fatal | Access Token 已过期 |
| `SessionTokenInvalid` | 10 | Transient | 会话令牌无效 |
| `SessionTokenExpired` | 11 | Transient | 会话令牌已过期 |
| `HandshakeRequired` | 12 | Transient | 未完成握手就发送命令 |
| `CapabilityDenied` | 20 | Application | 凭据未授予该操作所需能力 |
| `RateLimited` | 21 | Application | 命令被限频丢弃 |
| `UnsupportedVersion` | 22 | Fatal | SDK 协议版本不被机器人支持 |
| `TopicNotFound` | 30 | Application | 订阅的 topic 不存在 |
| `TopicAlreadySubscribed` | 31 | Application | 该 topic 已订阅，不可重复订阅 |
| `TopicNotSubscribed` | 32 | Application | 该 topic 尚未订阅 |
| `ControlEStopActive` | 9300 | Application | 机器人处于急停态，控制命令被拒 |
| `ControlPublisherUnavailable` | 9301 | Application | 机器人控制链路未就绪 |
| `ControlInvalidTwist` | 9302 | Application | 速度参数非法（NaN / Inf 或超物理上限） |
| `TaskRunning` | 9400 | Application | 机器人已有任务在执行，需先取消 |
| `TaskNotFound` | 9401 | Application | 指定的 `task_id` 不存在 |
| `TaskInvalidState` | 9402 | Application | 任务当前状态不允许该操作 |
| `NavMarkerNotFound` | 9403 | Application | 指定的点位不存在 |
| `NavNoActiveMap` | 9404 | Application | 机器人当前无激活地图 |
| `NavGoalUnreachable` | 9405 | Application | 目标不可达（被拒 / 在禁行区 / 无可行路径） |
| `NavLowBattery` | 9406 | Application | 电量过低，无法执行导航任务 |
| `TransportFailure` | 10000 | Transient | WebSocket 连接或读写失败 |
| `Timeout` | 10001 | Transient | 请求超时 |
| `Cancelled` | 10002 | Local | 请求被客户端取消 |
| `Disconnected` | 10003 | Local | 未连接（在非 `Ready` 状态调用） |
| `WouldDeadlock` | 10004 | Local | 在 SDK 回调内调用同步方法（被拦截以避免死锁） |
| `InternalError` | 10999 | Local | SDK 内部错误 |

### ErrorCategory

| 取值 | 含义 |
|---|---|
| `Ok` | 无错误 |
| `Transient` | 临时错误，SDK 自动重连 |
| `Application` | 单次调用失败，不触发重连 |
| `Fatal` | 致命错误，进 `Fatal` 终态 |
| `Local` | 客户端本地错误 |

### 辅助函数

| 签名 | 说明 |
|---|---|
| `const char* to_string(ErrorCode code)` | 错误码转字符串 |
| `ErrorCategory classify(ErrorCode code) noexcept` | 错误码分类 |

### SdkException

| 成员 | 说明 |
|---|---|
| `ErrorCode code() const noexcept` | 错误码 |
| `const char* what() const` | 错误信息（继承自 `std::runtime_error`） |

---

## 诊断

### Diagnostics

`GetDiagnostics()` 返回的快照，含 `connection` / `transport` / `subscriptions` /
`rpcs` 四组。`karo::sdk::to_json(const Diagnostics&)` 可序列化为 JSON。

| 分组 | 字段 |
|---|---|
| `connection` | `state` / `last_state_change_ms` / `reconnect_count` / `last_error_code` / `last_error_message` / `server_protocol_version` |
| `transport` | `rtt_p50_ms` / `rtt_p99_ms` / `bytes_sent` / `bytes_received` / `last_ping_age_ms` |
| `subscriptions[]` | `topic` / `state` / `configured_hz` / `received_hz_1min` / `drop_count` / `last_seq` / `last_message_age_ms` |
| `rpcs[]` | `name` / `count` / `error_count` / `p99_latency_ms` |

### LogLevel / LogHandler

```cpp
enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };
using LogHandler = std::function<void(LogLevel level, const std::string& message)>;
```

---

## 数据类型

### RobotStatus

`SubscribeRobotStatus` 回调收到的 typed 状态。

| 字段 | 类型 | 说明 |
|---|---|---|
| `robot_id` | `std::string` | 机器人 ID |
| `sequence` | `uint64_t` | 心跳序号 |
| `timestamp_ms` | `uint64_t` | 服务端时间戳 |
| `health_score` | `uint32_t` | 健康度 0-100 |
| `battery_percent` | `uint32_t` | 电量 0-100 |
| `charge_type` | `ChargeType` | 充电类型 |
| `service_state` | `ServiceState` | 服务状态 |
| `is_estop` | `bool` | 是否急停（硬或软） |
| `is_hw_estop` | `bool` | 硬急停 |
| `is_sw_estop` | `bool` | 软急停 |
| `active_map_id` | `std::string` | 当前地图 ID |
| `active_map_name` | `std::string` | 当前地图名 |
| `active_floor` | `int32_t` | 当前楼层 |
| `current_task_id` | `std::string` | 当前任务 ID，空为无任务 |
| `uptime_seconds` | `uint64_t` | 运行时长 |
| `idle_seconds` | `uint64_t` | 空闲时长 |
| `charging_seconds` | `uint64_t` | 充电时长 |
| `error_codes` | `std::vector<int32_t>` | 当前活跃错误码 |

### CmdVelResult

| 字段 | 类型 | 说明 |
|---|---|---|
| `accepted` | `bool` | 是否被接受 |
| `code` | `ErrorCode` | 错误码 |
| `message` | `std::string` | 诊断信息 |
| `rtt` | `milliseconds` | 往返时延 |

### EmergencyStopResult

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | `bool` | 是否成功 |
| `code` | `ErrorCode` | 错误码 |
| `message` | `std::string` | 诊断信息 |

### CreateTaskResult

| 字段 | 类型 | 说明 |
|---|---|---|
| `accepted` | `bool` | 任务是否创建成功 |
| `code` | `ErrorCode` | 错误码 |
| `message` | `std::string` | 诊断信息 |
| `task_id` | `std::string` | 创建成功时的新任务 ID |

### TaskCommandResult

`CancelTask` / `PauseTask` / `ResumeTask` 的返回值。

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | `bool` | 是否成功 |
| `code` | `ErrorCode` | 错误码 |
| `message` | `std::string` | 诊断信息 |

### TaskEvent

`SubscribeTaskEvents` 回调收到的任务状态快照。

| 字段 | 类型 | 说明 |
|---|---|---|
| `task_id` | `std::string` | 任务 ID |
| `task_type` | `TaskType` | 任务类型 |
| `state` | `TaskState` | 当前状态 |
| `cancel_reason` | `TaskCancelReason` | 取消原因（`state == Cancelled` 时有意义） |
| `pause_reason` | `TaskPauseReason` | 暂停原因（`state == Paused` 时有意义） |
| `error_code` | `ErrorCode` | 失败错误码（`state == Failed` 时有意义） |
| `error_message` | `std::string` | 失败信息 |
| `marker_id` | `std::string` | 目标点位 ID（按点位导航时） |
| `planned_distance` | `double` | 规划总距离（米） |
| `traveled_distance` | `double` | 已行驶距离（米） |
| `created_at_ms` | `uint64_t` | 任务创建时间戳 |
| `started_at_ms` | `uint64_t` | 任务开始时间戳 |
| `completed_at_ms` | `uint64_t` | 任务结束时间戳 |
| `timestamp_ms` | `uint64_t` | 本次事件时间戳 |

### SafetyEvent

`SubscribeSafetyEvents` 回调收到的安全状态快照。

| 字段 | 类型 | 说明 |
|---|---|---|
| `code` | `uint32_t` | 安全错误码 |
| `level` | `uint32_t` | 安全等级 |
| `severity` | `uint32_t` | 严重度 |
| `mode` | `std::string` | 当前安全模式 |
| `active_source` | `std::string` | 触发源 |
| `timestamp_ms` | `uint64_t` | 事件时间戳 |

### RobotInfo

| 字段 | 类型 | 说明 |
|---|---|---|
| `sn` | `std::string` | 机器人序列号 |
| `model` | `std::string` | 机器人型号 |
| `protocol_version` | `std::string` | 服务端协议版本 |
| `granted_capabilities` | `Capabilities` | 已授权能力 |
| `available_topics` | `std::vector<TopicDescriptor>` | 可订阅 topic 列表 |

### Capabilities

| 字段 | 类型 | 说明 |
|---|---|---|
| `chassis_control` | `bool` | 底盘控制（`CmdVel` / `EmergencyStop`） |
| `telemetry_read` | `bool` | 状态订阅（`SubscribeRobotStatus` / `SubscribeSafetyEvents`） |
| `image_stream` | `bool` | 图像流（后续版本） |
| `map_read` | `bool` | 地图读取（后续版本） |
| `map_write` | `bool` | 地图修改（后续版本） |
| `task_control` | `bool` | 任务控制（创建 / 取消 / 暂停 / 恢复任务，订阅任务事件） |

### TopicDescriptor

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | `std::string` | topic 名 |
| `description` | `std::string` | 说明 |
| `default_hz` | `float` | 默认推送频率 |
| `max_hz` | `float` | 最大推送频率 |
| `delivery_semantics` | `DeliverySemantics` | 投递语义 |

### 枚举

#### ChargeType

| 取值 | 含义 |
|---|---|
| `None` | 未充电 |
| `Wire` | 充电线直连充电 |
| `Dock` | 充电桩对接充电 |

#### ServiceState

| 取值 | 含义 |
|---|---|
| `Idle` | 空闲 |
| `Task` | 执行任务中 |
| `Mapping` | 建图中 |
| `Starting` | 启动中 |
| `ShuttingDown` | 关机中，拒绝新任务 |
| `Upgrading` | OTA 升级中，拒绝新任务 |
| `RemoteControl` | 操作员遥控中 |
| `GotoCharging` | 自主返回充电桩中 |

#### SubscriptionState

| 取值 | 含义 |
|---|---|
| `Pending` | 待生效，已记录订阅意图，尚未与服务端建立 |
| `Active` | 已激活，正常接收数据 |
| `Paused` | 已暂停 |
| `Closed` | 已关闭 |

#### StreamStatusKind

| 取值 | 含义 |
|---|---|
| `Started` | 订阅已建立，开始推送 |
| `Paused` | 推送暂停 |
| `Resumed` | 推送恢复（通常发生在重连后） |
| `Gap` | 检测到丢帧 |
| `Closed` | 订阅已关闭 |

#### DropPolicy

队列满时的丢弃策略。

| 取值 | 含义 |
|---|---|
| `DropOldest` | 丢弃队列中最旧的消息 |
| `DropNewest` | 丢弃最新到达的消息 |

#### CallbackMode

数据回调的派发模式。

| 取值 | 含义 |
|---|---|
| `Dispatched` | 回调在 SDK 内部线程派发，不阻塞 IO 线程 |
| `Inline` | 回调在 IO 线程直接执行，低延迟；回调内不可做耗时操作 |

#### DeliverySemantics

topic 的投递语义。

| 取值 | 含义 |
|---|---|
| `Telemetry` | 遥测流，服务端按频率降采样，缺帧属正常采样 |
| `Event` | 事件流，每帧应送达，SDK 做丢帧检测 |

#### TaskType

| 取值 | 含义 |
|---|---|
| `Unspecified` | 未指定 |
| `Navigation` | 导航任务 |
| `Patrol` | 巡逻任务 |
| `GoBack` | 返回充电桩任务 |

#### TaskState

| 取值 | 含义 |
|---|---|
| `Unspecified` | 未指定 |
| `Pending` | 已创建，等待开始 |
| `Running` | 执行中 |
| `Paused` | 已暂停 |
| `Cancelling` | 取消中 |
| `Succeeded` | 已成功完成 |
| `Failed` | 已失败 |
| `Cancelled` | 已取消 |

#### TaskCancelReason

| 取值 | 含义 |
|---|---|
| `Unspecified` | 未指定 |
| `User` | 客户主动取消 |
| `Estop` | 急停触发取消 |
| `Timeout` | 任务执行超时 |
| `PauseTimeout` | 暂停超时未恢复，自动取消 |
| `ResumeFailed` | 恢复失败 |
| `Error` | 执行错误 |

#### TaskPauseReason

| 取值 | 含义 |
|---|---|
| `Unspecified` | 未指定 |
| `User` | 客户主动暂停 |
| `Estop` | 急停触发暂停 |

---

## 反馈

接口问题请发 **developer@kanesir.com**，附 SDK 版本号 + 复现步骤。
