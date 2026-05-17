#ifndef KARO_SDK_DIAGNOSTICS_HPP_
#define KARO_SDK_DIAGNOSTICS_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "karo/sdk/connection_state.hpp"
#include "karo/sdk/error.hpp"
#include "karo/sdk/subscription.hpp"

namespace karo::sdk {

enum class LogLevel {
  Trace = 0,
  Debug = 1,
  Info  = 2,
  Warn  = 3,
  Error = 4,
  Off   = 5,
};

const char* to_string(LogLevel level) noexcept;

using LogHandler = std::function<void(LogLevel level, const std::string& message)>;

struct Diagnostics {
  struct Connection {
    ConnectionState state = ConnectionState::Idle;
    int64_t  last_state_change_ms = 0;
    uint64_t reconnect_count      = 0;
    ErrorCode   last_error_code    = ErrorCode::Ok;
    std::string last_error_message;
    std::string server_protocol_version;
  } connection;

  struct Transport {
    double   rtt_p50_ms       = 0.0;
    double   rtt_p99_ms       = 0.0;
    uint64_t bytes_sent       = 0;
    uint64_t bytes_received   = 0;
    int64_t  last_ping_age_ms = -1;
  } transport;

  struct SubscriptionStat {
    std::string topic;
    SubscriptionState state    = SubscriptionState::Pending;
    double   configured_hz     = 0.0;
    double   received_hz_1min  = 0.0;
    uint64_t drop_count        = 0;
    uint64_t last_seq          = 0;
    int64_t  last_message_age_ms = -1;
  };
  std::vector<SubscriptionStat> subscriptions;

  struct RpcStat {
    std::string name;
    uint64_t count       = 0;
    uint64_t error_count = 0;
    double   p99_latency_ms = 0.0;
  };
  std::vector<RpcStat> rpcs;
};

std::string to_json(const Diagnostics& d);

}

#endif

