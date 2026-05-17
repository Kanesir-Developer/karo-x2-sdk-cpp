# proto/ — wire 协议 schema

本目录包含 Karo X2 SDK 客户端与机器人之间的 **wire 协议 `.proto` 文件**, 由
protobuf 序列化经 WebSocket (WSS + mTLS) 传输.

> **大多数客户不需要读 .proto 文件** — `cpp/include/karo/sdk/*.hpp` 和
> Python `from karo_x2_sdk import ...` 已经把所有 proto 类型封装成 plain
> struct / dataclass, 客户工程不需要 `link libprotobuf` 也不需要 include 任何
> `.pb.h`. 本目录主要服务两类场景:
>
> 1. **自定义语言绑定**: 客户用 Go / Rust / Java 等 SDK 未直接支持的语言, 自己
>    跑 `protoc --<lang>_out` 生成代码.
> 2. **wire 抓包调试**: 用 `protoc --decode` 解 binary frame 内容.

## 稳定性承诺

`STABILITY.md` 详细描述本目录 `.proto` 文件在 SDK 主版本内的兼容性保证 —
字段号锁定 / message 名稳定 / 字段不删 / RPC method 名稳定 / enum 取值不重映射.

升级 SDK minor 版本不会破坏 wire 兼容 (e.g. v3.2.x → v3.3.x); 主版本升级
(v3.x.x → v4.x.x) 会在 release note 显式标注 wire 破坏性变更.

## 文件清单 (v3.x 阶段对外暴露)

| 文件 | 用途 |
|---|---|
| `sdk/common.proto` | SDK 通道错误码 / 能力集 / topic 描述符 |
| `sdk/session.proto` | SDK 通道命令 envelope (handshake / subscribe / RPC) |
| `sdk/v1/telemetry.proto` | `robot.state` topic 的 payload (`SdkRobotStateUpdate`) |
| `karo/v1/common.proto` | 业务错误码 + 通用 options |
| `karo/v1/rpc.proto` | RPC Request/Response envelope + `SystemService.{Ping, Authenticate}` |
| `karo/v1/control.proto` | `ControlService.CmdVel` (高频遥控) |
| `karo/v1/safety.proto` | `SafetyService.EmergencyStop` (软急停) |
| `karo/v1/stream.proto` / `telemetry_stream.proto` / `transport.proto` / `tasks.proto` | 内部辅助 schema, 当前 v3.x 客户端不直接使用, 保留以便未来扩展 |

更详细的字段语义和稳定性边界见 `STABILITY.md` + 各 `.proto` 文件内的注释.

## 反馈

发现 wire 协议级问题 (字段号冲突 / 编解码异常 / schema 不一致) 直接发邮件
**developer@kanesir.com**, 附 SDK 版本号 + 复现步骤即可.
