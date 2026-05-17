#ifndef KARO_SDK_INTERNAL_ROBOT_IMPL_HPP_
#define KARO_SDK_INTERNAL_ROBOT_IMPL_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "karo/sdk/connection_state.hpp"
#include "karo/sdk/diagnostics.hpp"
#include "karo/sdk/error.hpp"
#include "karo/sdk/robot.hpp"
#include "karo/sdk/subscription.hpp"

#include "dispatcher.hpp"
#include "log.hpp"
#include "proto_codec.hpp"
#include "rate_limiter.hpp"
#include "subscription_impl.hpp"
#include "ws_client.hpp"

namespace karo::sdk::detail {

struct InflightRpc {
  uint64_t                                  request_id;
  std::string                               method;
  std::chrono::steady_clock::time_point     enqueued_at;

  std::function<void(const DecodedResponse&, ErrorCode timeout_or_disc)> on_complete;

  std::shared_ptr<boost::asio::steady_timer> timeout_timer;
};

struct HeartbeatState {
  std::chrono::steady_clock::time_point last_inbound_at;
  std::chrono::steady_clock::time_point last_ping_sent_at;
  uint64_t                              last_ping_request_id = 0;
  bool                                  ping_deadline_active = false;
  uint32_t                              miss_count = 0;
};

class RttHistogram {
 public:
  void record(int64_t rtt_ms);
  double p50_ms() const;
  double p99_ms() const;
 private:
  mutable std::mutex mu_;
  std::deque<int64_t> samples_;
  static constexpr size_t kCap = 1024;
};

class RobotImpl : public std::enable_shared_from_this<RobotImpl> {
 public:

  static std::shared_ptr<RobotImpl> Create(const ConnectOptions& opts);

  ~RobotImpl();

  void SetConnectionStateHandler(ConnectionStateHandler h);
  void SetLogHandler(LogHandler h);

  void Connect();
  void Close();

  ConnectionState state() const noexcept { return state_.load(); }
  bool IsConnected() const noexcept { return state_.load() == ConnectionState::Ready; }

  std::optional<RobotInfo> info() const;

  std::unique_ptr<Subscription> SubscribeRobotStatus(
    double desired_hz,
    Robot::RobotStatusCallback on_data,
    StatusCallback on_status);

  std::unique_ptr<Subscription> SubscribeGeneric(
    const std::string& topic,
    double desired_hz,
    Robot::TopicDataCallback on_data,
    StatusCallback on_status);

  CmdVelResult CmdVel(double vx, double vy, double wz);
  EmergencyStopResult EmergencyStopRpc(bool engage, const std::string& reason);
  std::chrono::milliseconds Ping();

  Diagnostics GetDiagnostics() const;

  ErrorCode UnsubscribeRequest(const std::string& topic, bool sync_wait);

 private:
  explicit RobotImpl(const ConnectOptions& opts);

  void ValidateOpts() const;

  void RunControlLoop();

  void EstablishConnection(bool is_reconnect);
  void DoReconnect(uint32_t attempt);
  void TransitionTo(ConnectionState new_state, StateChangeInfo info);

  void AbortToFatal(ErrorCode code, const std::string& message);
  void OnInboundMessage(std::vector<uint8_t> bytes);
  void OnTransportClose(ErrorCode reason, std::string msg);
  void HandleSdkResponse(const DecodedResponse& resp);
  void HandleSdkTopicPush(const DecodedTopicPush& push);

  void StartHeartbeat();
  void OnIdleTimerExpired();
  void OnPingDeadlineExpired();
  void ResetLivenessOnInbound();

  void SendSubscribe(const std::shared_ptr<SubSlot>& slot, uint64_t since_seq);
  void ResubscribeAllAfterReconnect();
  void HandleSubscribeAck(const DecodedResponse& resp);
  void DeliverPushToSub(const std::shared_ptr<SubSlot>& slot,
                       const DecodedTopicPush& push);
  void DrainPendingBuffer(const std::shared_ptr<SubSlot>& slot, uint64_t next_seq);
  void FireStatus(const std::shared_ptr<SubSlot>& slot, StreamStatusKind k,
                  std::optional<uint64_t> gap_count, ErrorCode err = ErrorCode::Ok,
                  const std::string& reason = {});

  void DoEStopReplay();

  int64_t SlideHzBuckets(const std::shared_ptr<SubSlot>& slot);

  bool SendOrFail(std::vector<uint8_t> bytes) noexcept;

  std::chrono::milliseconds NextBackoff(uint32_t attempt);

  void PostStateCallback(ConnectionState old_s, ConnectionState new_s,
                         StateChangeInfo info);
  void PostLog(LogLevel level, std::string msg);

  uint64_t NextRequestId() noexcept { return next_request_id_.fetch_add(1); }

  const ConnectOptions opts_;

  std::atomic<ConnectionState> state_{ConnectionState::Idle};

  boost::asio::io_context  control_ioc_;
  std::optional<boost::asio::executor_work_guard<
    boost::asio::io_context::executor_type>> control_work_;
  std::thread              control_thread_;
  std::atomic<bool>        shutdown_requested_{false};

  Dispatcher               dispatcher_;

  LogSink                  log_sink_;

  std::unique_ptr<WsClient> ws_;

  mutable std::mutex       handler_mu_;
  ConnectionStateHandler   state_handler_;

  mutable std::mutex       info_mu_;
  std::optional<RobotInfo> robot_info_;

  std::atomic<uint64_t>    next_request_id_{1};

  std::unordered_map<uint64_t, InflightRpc> inflight_rpc_;

  std::map<std::string, std::shared_ptr<SubSlot>> active_subs_;

  std::unordered_map<uint64_t, std::string> inflight_sub_request_;

  std::unordered_map<uint64_t, std::shared_ptr<boost::asio::steady_timer>>
    sub_ack_timers_;

  HeartbeatState           heartbeat_;
  boost::asio::steady_timer idle_timer_;
  boost::asio::steady_timer ping_deadline_timer_;

  uint32_t                 reconnect_attempt_ = 0;
  uint64_t                 reconnect_total_count_ = 0;
  std::mt19937             rng_;
  boost::asio::steady_timer reconnect_timer_;

  enum class EStopIntent { None, Engaged };

  EStopIntent              last_estop_intent_ = EStopIntent::None;
  std::string              last_estop_reason_;

  size_t                   reconnect_subs_pending_ = 0;
  bool                     reconnect_estop_pending_ = false;
  ErrorCode                reconnect_failure_code_ = ErrorCode::Ok;
  std::string              reconnect_failure_message_;

  TokenBucket              cmdvel_rate_limiter_;

  std::atomic<int64_t>     last_state_change_ms_{0};

  std::atomic<int>         last_error_code_{static_cast<int>(ErrorCode::Ok)};
  mutable std::mutex       last_error_msg_mu_;
  std::string              last_error_msg_;
  std::atomic<uint64_t>    bytes_sent_{0};
  std::atomic<uint64_t>    bytes_received_{0};
  std::atomic<int64_t>     last_ping_sent_ms_{-1};
  RttHistogram             rtt_hist_;
  struct RpcStat {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> error_count{0};
    RttHistogram          latency_hist;
  };
  RpcStat                  rpc_stat_cmdvel_;
  RpcStat                  rpc_stat_estop_;
  RpcStat                  rpc_stat_ping_;
};

}

#endif

