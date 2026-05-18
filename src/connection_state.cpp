#include "karo/sdk/connection_state.hpp"

namespace karo::sdk {

const char* to_string(ConnectionState state) noexcept {
  switch (state) {
    case ConnectionState::Idle:             return "Idle";
    case ConnectionState::Connecting:       return "Connecting";
    case ConnectionState::Ready:            return "Ready";
    case ConnectionState::TransientFailure: return "TransientFailure";
    case ConnectionState::Reconnecting:     return "Reconnecting";
    case ConnectionState::Shutdown:         return "Shutdown";
    case ConnectionState::Fatal:            return "Fatal";
  }
  return "Unknown";
}

}

