# Wire 协议稳定性承诺

本目录的 `.proto` 文件是 Karo SDK 与机器人之间的 **wire 协议规范**, 客户工程可直接 `protoc` 生成自己语言的代码, 或用 `protoc --decode` 调试抓包.

本文档定义 v1 阶段的稳定性边界 — 哪些保证不变, 哪些保留演进空间.

---

## 我们承诺 (在同一 v1.x 主版本内)

### 1. 字段号永不复用

任何字段一旦发布, 其编号 (= 1 / = 2 / ...) 不会被赋给另一个语义不同的字段. 字段被弃用时改用 `[deprecated = true]` 标注, 但编号保留.

### 2. message / enum / RPC 名永不重命名

`SdkCommand` / `RobotStatus` / `ControlService.CmdVel` 等公开符号一旦发布即锁定. 内部重构由 SDK 客户端封装, 不传染到 wire 层.

### 3. 现有字段不删

只追加新字段, 老字段保留. 客户老代码不会因为升级机器人固件而 break.

### 4. enum 取值不重新映射

`SdkErrorCode::SDK_AUTH_FAILED = 1` 永远是 1, `karo.v1.ErrorCode::ERROR_CODE_OK = 0` 永远是 0. 新增取值用新编号, 老编号语义不变.

### 5. RPC method 名稳定

`control.cmd_vel` / `safety.emergency_stop` / `system.ping` / `system.authenticate` 这些 method 字符串一旦发布即锁定, 客户硬编码安全.

---

## 我们保留权利 (会向客户提前公告)

### 1. 新增字段 / 新增 message / 新增 RPC

向后兼容, 客户老代码继续工作. 新功能要靠客户主动升级 SDK 才能用.

### 2. 标记 `[deprecated = true]`

字段或 message 标弃用; 至少保留 **2 个 minor 版本** 才会进入"可能在主版本升级时清理"状态.

### 3. 调整服务端默认值 / 限频

例如 `cmd_vel` 服务端兜底从 100 Hz 调成 200 Hz; wire 字段不变, 客户代码无需改, 但行为感观可能变.

### 4. 引入 v2 协议 (主版本升级)

破坏性变更必须用主版本号区隔, 通过 `SdkHandshakeResponse.protocol_version` 协商. SDK 1.x 客户端连接到只支持 2.x 的机器人会拿到 `SDK_UNSUPPORTED_VERSION` 错误码, 不会静默跑出错误结果.

---

## 当前 v1 暴露范围

只包含 SDK 客户端实际能调用 / 接收的 proto 子集, 并非机器人内部全部协议:

| 文件 | 用途 | 客户怎么用 |
|---|---|---|
| `sdk/common.proto` | `SdkErrorCode` / `SdkCapabilitySet` / `SdkTopicDescriptor` | 错误码识别 |
| `sdk/session.proto` | `SdkCommand` / `SdkResponse` / 握手 / 订阅 envelope | wire 抓包调试; SDK 内部已封装 |
| `karo/v1/common.proto` | `karo.v1.ErrorCode`, options | 业务命令错误码 |
| `karo/v1/rpc.proto` | `Request` / `Response` envelope, `SystemService` | wire 抓包; 自定义语言绑定 |
| `karo/v1/control.proto` | `ControlService.CmdVel`, `Twist`, `Vector3` | 自定义语言客户端 |
| `karo/v1/safety.proto` | `SafetyService.EmergencyStop` | 同上 |

v1 阶段 SDK 聚焦于"状态订阅 + 控制 + 安全"三类最常用能力. 未来主版本会按业务面板 (地图 / 导航 / 任务 / 视频流等) 逐步扩展并加入本目录, 加入时遵循上述字段号永不复用 / 名称永不重命名等承诺.

---

## 不属于 wire 协议稳定性范围 (单独说明)

- **C++ SDK API surface** (`include/karo/sdk/*.hpp`) 在 SDK semver 内单独承诺. 详见 cpp/README.md.
- **Python SDK API surface** (`from karo_x2_sdk import ...`) 在 SDK semver 内单独承诺. 详见 python/README.md.
- **TLS 证书 / mTLS 握手细节**: 由 SDK 内部封装, 不视为 wire 公开协议.

---

## 反馈

发现协议级问题 (字段号冲突 / 编码不一致 / 文档错误) 直接发 **developer@kanesir.com**, 请附:

1. SDK 版本号 + 机器人型号
2. 抓包 / 错误码 / 复现步骤
