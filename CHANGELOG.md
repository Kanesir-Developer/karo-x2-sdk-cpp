# Changelog

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/).

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
