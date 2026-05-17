#include "karo/sdk/subscription.hpp"

namespace karo::sdk {

const char* to_string(StreamStatusKind k) noexcept {
  switch (k) {
    case StreamStatusKind::Started: return "Started";
    case StreamStatusKind::Paused:  return "Paused";
    case StreamStatusKind::Resumed: return "Resumed";
    case StreamStatusKind::Gap:     return "Gap";
    case StreamStatusKind::Closed:  return "Closed";
  }
  return "Unknown";
}

const char* to_string(SubscriptionState s) noexcept {
  switch (s) {
    case SubscriptionState::Pending: return "Pending";
    case SubscriptionState::Active:  return "Active";
    case SubscriptionState::Paused:  return "Paused";
    case SubscriptionState::Closed:  return "Closed";
  }
  return "Unknown";
}

Subscription::~Subscription() = default;

}

