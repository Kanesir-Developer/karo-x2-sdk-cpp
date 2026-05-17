# karo-x2-sdk-cpp

Karo X2 机器人第三方 C++ SDK. 通过机器人主板 LAN 端口 4434 直连, 在 X.509 证书认证后进行设备状态订阅 / 遥控 / 软急停.

> 本仓库由 Karo 官方在每次发版时统一刷新发布 — 仓库历史会被覆盖, 直接在此仓提 PR 不会被采纳. 反馈请发 **developer@kanesir.com**.

## 系统要求

最低支持: **Ubuntu 20.04** (即 ROS Noetic / ROS 2 Foxy 的常见客户环境).

| 依赖 | 最低版本 | Ubuntu 20.04 默认 apt 包 |
|---|---|---|
| C++17 编译器 | GCC ≥ 9.4 / Clang ≥ 10 | `g++` (默认 9.4) |
| CMake | ≥ 3.16 | `cmake` (默认 3.16.3) |
| Boost (system + Beast) | ≥ 1.71 | `libboost-system-dev` (默认 1.71) |
| OpenSSL | ≥ 1.1.1 | `libssl-dev` (默认 1.1.1f) |
| Protobuf | ≥ 3.6 | `libprotobuf-dev` + `protobuf-compiler` (默认 3.6.1) |

更新版本 (Ubuntu 22.04 / 24.04 / Debian Bookworm) 一律向后兼容.

## 5 分钟快速开始

```bash
# 1. 安装依赖 (Ubuntu 20.04 / 22.04 通用)
sudo apt install -y \
  build-essential cmake \
  libboost-system-dev libssl-dev \
  libprotobuf-dev protobuf-compiler

# 2. 编译 (本仓根目录即源码根)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. (可选) 安装到系统, 后续工程用 find_package(KaroSDK CONFIG) 接入
sudo cmake --install build      # 默认 prefix /usr/local
# 或指定 prefix: cmake --install build --prefix $HOME/.local

# 4. 跑遥控示例
#    creds 目录 = 厂商下发的凭据 zip 解压后, 内含 cert.pem / key.pem / ca.pem
./build/teleop 192.168.10.10 ./creds
```

> **如何获取 SDK 凭据**: 联系机器人服务商索取一份"开发者凭据 zip", 内含 X.509
> 证书 (`cert.pem` / `key.pem` / `ca.pem`) + 该证书绑定的能力授权 (telemetry_read +
> chassis_control 等). **客户没有自助下发权限**, 凭据由服务商按客户身份 + 用途
> 集中签发, 防止凭据泛滥后无法吊销.

> **机器人 IP**: SDK 默认 `192.168.10.10` (有线 USB-RNDIS / 直连 LAN 网卡时
> 出厂固定地址). 跑 example 不传第一个参数也用默认值. 若**通过无线局域网**连
> (机器人通过 WiFi 接入家用/办公网络), IP 由路由器 DHCP 分配, 客户从机器人
> 显示屏 / 路由器后台拿到实际 IP 后传给 `ConnectOptions.host`:
>
> ```cpp
> karo::sdk::ConnectOptions opts;
> opts.host = "10.0.5.27";  // 路由器分配的实际 IP, 不含端口 (固定 4434)
> ```

## 完整示例

`examples/` 下几个完整可跑的程序:

| 文件 | 作用 |
|---|---|
| `teleop.cpp` | 10 Hz 发 cmd_vel 让机器人前进; 演示连接 / 能力检查 / 高频路径 |
| `subscribe_status.cpp` | 订阅 `robot.state` 5 Hz; 演示 topic 推送回调 |
| `emergency_stop.cpp` | 触发 → 等待 → 解除软急停; 演示业务 RPC |
| `estop_cmdvel_e2e.cpp` | 急停 + cmd_vel 端到端集成 |

## API 概览

```cpp
#include <karo/sdk/robot.hpp>

karo::sdk::ConnectOptions opts;
opts.host = "192.168.10.10";  // 不含端口, 固定 4434
opts.cert.cert_pem = ...;     // X.509 PEM
opts.cert.key_pem  = ...;
opts.cert.ca_pem   = ...;
opts.client_id     = "my-app/1.0";

// 两阶段: 构造 (仅校验参数, 不发起网络) → 注册 handlers → Connect (异步立即返回)
karo::sdk::Robot robot(opts);

robot.SetConnectionStateHandler([](auto old_s, auto new_s, auto info) {
  // 状态转移: Idle → Connecting → Ready (或 Fatal / TransientFailure)
});
robot.SetLogHandler([](auto level, const std::string& msg) {
  // SDK 内部日志路由 (诊断 / 排查)
});

robot.Connect();   // 异步, 立即返回; 真正可用以 ConnectionStateHandler 看 Ready 为准

// ① 设备状态 (typed: 拿 plain C++ struct, 无需 link protobuf)
// robot.state 服务端推 1 Hz, desired_hz 超过会被截断到 max_hz, 这里传 1.0 一致.
auto sub = robot.SubscribeRobotStatus(1.0, [](const karo::sdk::RobotStatus& s) {
  std::cout << "battery=" << s.battery_percent
            << " estop=" << s.is_estop << "\n";
});

// ② 遥控 (内部 ≤20Hz 令牌桶, 0.5s watchdog 自动归零, 断连不重试)
auto r1 = robot.CmdVel(0.2, 0.0, 0.0);

// ③ 软急停 (engage=true 重连后自动重放)
auto r2 = robot.EmergencyStop(true,  "user pressed button");
auto r3 = robot.EmergencyStop(false);

robot.Close();
```

完整接口文档见 [API.md](API.md) — 所有方法 / 配置项 / 数据类型 / 错误码的逐项说明.

## CMake 接入

安装到本地后, 客户 CMakeLists.txt:

```cmake
find_package(KaroSDK CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE KaroSDK::karo_sdk_cpp)
```

源码内构建 (不安装) 也走 alias:

```cmake
add_subdirectory(path/to/karo-x2-sdk-cpp)
target_link_libraries(my_app PRIVATE KaroSDK::karo_sdk_cpp)
```

## 故障排查

**连不上 (ConnectionState 卡 Connecting / 进 TransientFailure)**

1. `ping <host>` 通不通: 不通先查物理连接 (有线网线 / WiFi SSID), 默认 `192.168.10.10`
   是有线直连地址; 无线请用路由器分配的实际 IP.
2. `nc -zv <host> 4434` 端口 4434 通不通: 不通可能机器人 SDK 服务未启动或被
   防火墙拦, 联系厂商.
3. 凭据是否过期: 证书有效期写在 `cert.pem` 里, `openssl x509 -noout -dates -in
   cert.pem` 查; 过期联系服务商换发.
4. ca.pem 是否匹配机器人: 不匹配会握手时 mTLS handshake fail; 同一批凭据 zip
   内的 ca/cert/key 必须一起用, 别跟其他机器人的混.

**Ready 后业务命令被拒**

- `CmdVelResult.code == CapabilityDenied`: 凭据没授 `chassis_control`, 联系
  服务商换发带遥控能力的凭据.
- `code == ControlRejectedEstop`: 机器人当前急停态 (硬急停按下 / 充电中 / 等等),
  这是 server 主动拒绝, 不是 SDK 故障. 先调 `EmergencyStop(false)` 解除或物
  理释放硬急停后重试.

**编译 cmake -B build 报 protoc 缺 `--experimental_allow_proto3_optional`**

- 你的 protobuf < 3.15. SDK 已在 CMakeLists 给 protoc 加这个 flag, 升级到 SDK
  v3.2.3+ 即可; 老版本手动改 CMakeLists protoc 调用加 flag.

## 反馈

- **Issue / Bug 报告**: developer@kanesir.com
- 紧急安全问题: 邮件标题前加 `[SECURITY]`

## 错误处理约定

- **构造非法参数** (host 含冒号 / 凭据全空) → 抛 `karo::sdk::SdkException` (含 `ErrorCode`)
- **运行期故障** (网络断 / 握手失败 / 证书拒绝) → 通过 `ConnectionStateHandler` 暴露状态机, 不抛异常
- **业务命令失败** (cmd_vel / emergency_stop) → 返回值携带 `ErrorCode`, 不抛异常 — 高频路径不要求客户写 try/catch

## 限频与 Watchdog

| 接口 | 客户端限频 | 服务端限频 | Watchdog |
|---|---|---|---|
| `CmdVel` | 20 Hz 令牌桶, 超频本地丢弃 | 100 Hz 兜底 | 客户端 0.5s 自动零速; 服务端独立的 teleop 保持器 |
| `EmergencyStop` | 不限频 | 不限频 | 无 |
| `SubscribeRobotStatus` | 服务端按 `desired_hz` 降采样 | `robot.state` 上限 1 Hz | 无 |
