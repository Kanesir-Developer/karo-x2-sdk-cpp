#ifndef KARO_SDK_ROBOT_HPP_
#define KARO_SDK_ROBOT_HPP_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "karo/sdk/capabilities.hpp"
#include "karo/sdk/connection_state.hpp"
#include "karo/sdk/diagnostics.hpp"
#include "karo/sdk/error.hpp"
#include "karo/sdk/messages/robot_status.hpp"
#include "karo/sdk/subscription.hpp"
#include "karo/sdk/topic.hpp"

namespace karo::sdk {

struct CertCredentials {
  std::string cert_pem;
  std::string key_pem;
  std::string ca_pem;

  bool insecure_skip_verify = false;
};

struct AccessTokenCredentials {
  std::vector<uint8_t> token;
};

struct ConnectOptions {

  std::string host = "192.168.10.10";

  CertCredentials cert;
  AccessTokenCredentials access_token;

  std::string client_id = "karo-sdk-cpp";
  std::string sdk_version;

  std::chrono::milliseconds connect_timeout = std::chrono::seconds(5);

  std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(10);
  std::chrono::milliseconds ping_timeout = std::chrono::seconds(3);

  uint32_t max_missed_pings = 3;

  int32_t reconnect_max_attempts = -1;
  std::chrono::milliseconds reconnect_base_delay = std::chrono::milliseconds(500);
  std::chrono::milliseconds reconnect_max_delay  = std::chrono::seconds(30);
  double reconnect_jitter_ratio = 0.3;

  bool reconnect_replay_history = true;
  uint32_t history_limit = 1000;

  std::chrono::milliseconds subscribe_ack_timeout = std::chrono::seconds(5);

  uint32_t subscription_queue_size = 100;
  DropPolicy drop_policy = DropPolicy::DropOldest;

  CallbackMode callback_mode = CallbackMode::Dispatched;

  std::chrono::milliseconds cmdvel_rpc_timeout = std::chrono::milliseconds(200);
  std::chrono::milliseconds estop_rpc_timeout  = std::chrono::seconds(2);
};

struct RobotInfo {
  std::string sn;
  std::string model;
  std::string protocol_version;
  Capabilities granted_capabilities;
  std::vector<TopicDescriptor> available_topics;
};

struct CmdVelResult {
  bool accepted = false;
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  std::chrono::milliseconds rtt{0};
};

struct EmergencyStopResult {
  bool ok = false;
  ErrorCode code = ErrorCode::Ok;
  std::string message;
};

class Robot {
 public:

  explicit Robot(const ConnectOptions& opts);
  ~Robot();

  Robot(const Robot&) = delete;
  Robot& operator=(const Robot&) = delete;

  void SetConnectionStateHandler(ConnectionStateHandler handler);
  void SetLogHandler(LogHandler handler);

  void Connect();

  void Close();

  ConnectionState state() const noexcept;
  bool IsConnected() const noexcept;

  std::optional<RobotInfo> info() const;

  using RobotStatusCallback = std::function<void(const RobotStatus&)>;

  std::unique_ptr<Subscription> SubscribeRobotStatus(
    double desired_hz,
    RobotStatusCallback on_data,
    StatusCallback on_status = {});

  using TopicDataCallback = std::function<void(const TopicMessage&)>;
  std::unique_ptr<Subscription> Subscribe(
    const std::string& topic,
    double desired_hz,
    TopicDataCallback on_data,
    StatusCallback on_status = {});

  CmdVelResult CmdVel(double linear_x, double linear_y, double angular_z);

  EmergencyStopResult EmergencyStop(bool engage, const std::string& reason = "");

  std::chrono::milliseconds Ping();

  Diagnostics GetDiagnostics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* sdk_version();

}

#endif

