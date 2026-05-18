# Changelog

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/).

## [3.3.1] - 2026-05-18

### Fixed
- 修正导航任务的 wire 协议对齐 — 3.3.0 用了不存在的 RPC 名与消息结构,
  无法与机器人通信。3.3.1 改为机器人实际协议:
  - 创建任务走 `move.create_task`,取消/暂停/恢复走 `task_actions.*_task`。
  - `tasks.events` 按 `TaskEventEnvelope` 解码;安全事件 topic 为 `robot.safety`。
- 行为与 3.3.0 文档一致,客户代码无需改动 —— 仅底层 wire 修正。

**3.3.0 已撤回,请直接使用 3.3.1。**

## [3.3.0] - 2026-05-18

### Added
- 导航任务接口: `CreateNavigationTaskToMarker` / `CreateNavigationTaskToPose` /
  `CancelTask` / `PauseTask` / `ResumeTask`.
- 事件订阅: `SubscribeTaskEvents`(`TaskEvent`)/ `SubscribeSafetyEvents`(`SafetyEvent`).
- 新数据类型 `CreateTaskResult` / `TaskCommandResult` / `TaskEvent` / `SafetyEvent`,
  新枚举 `TaskType` / `TaskState` / `TaskCancelReason` / `TaskPauseReason`.
- 新错误码 9400 段(`TaskRunning` / `TaskNotFound` / `TaskInvalidState` /
  `NavMarkerNotFound` / `NavNoActiveMap` / `NavGoalUnreachable` / `NavLowBattery`).

### Removed
- 移除通用 `Subscribe(topic, ...)` / `TopicMessage` —— 每个对外 topic 均已有
  typed 订阅方法,通用订阅返回原始字节与 SDK 设计相悖.

## [3.2.16] - 2026-05-17

### Added
- 新增接口文档 `API.md` (C++ / Python 各一份) — 方法 / 配置项 / 数据类型 / 错误码逐项说明.

API 与 wire 协议无变更, 与 3.2.15 二进制兼容.

## [3.2.15] - 2026-05-16

### Changed
- 全仓注释精简 — 仅保留必要的歧义说明.
- proto / README / 公开头文件措辞清理, 移除非客户向引用.

API 与 wire 协议无变更, 与 3.2.13 / 3.2.14 二进制兼容.

## [3.2.14] - 2026-05-16

### Changed
- 删除 cpp/README 内"协议版本兼容"说明 (实现细节, 不需要客户关心).

## [3.2.13] - 2026-05-15

### Added
- C++ SDK: `karo::sdk::Robot::Connect / Subscribe / CmdVel / EmergencyStop` 公开接口.
- Python SDK: `karo_x2_sdk` 包 (PyPI `karo-x2-sdk`), 同等 4 个接口.
- 示例工程: teleop / subscribe_status / emergency_stop.

## 版本号约定

- **MAJOR**: wire 协议主版本变更, 不向后兼容.
- **MINOR**: 新增接口或 capability, 向后兼容.
- **PATCH**: 修复 / 优化, API 不变.
