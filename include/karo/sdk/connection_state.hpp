#ifndef KARO_SDK_CONNECTION_STATE_HPP_
#define KARO_SDK_CONNECTION_STATE_HPP_

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "karo/sdk/error.hpp"

namespace karo::sdk {

enum class ConnectionState {
  Idle             = 0,
  Connecting       = 1,
  Ready            = 2,
  TransientFailure = 3,
  Reconnecting     = 4,
  Shutdown         = 5,
  Fatal            = 6,
};

const char* to_string(ConnectionState state) noexcept;

struct StateChangeInfo {

  std::string reason;

  ErrorCode last_error_code = ErrorCode::Ok;
  std::string last_error_message;

  std::chrono::milliseconds next_retry_in_ms{0};

  uint32_t reconnect_attempt = 0;

  std::string last_action;
};

using ConnectionStateHandler = std::function<void(
  ConnectionState old_state,
  ConnectionState new_state,
  const StateChangeInfo& info)>;

}

#endif

