#ifndef KARO_SDK_SUBSCRIPTION_HPP_
#define KARO_SDK_SUBSCRIPTION_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "karo/sdk/error.hpp"

namespace karo::sdk {

enum class StreamStatusKind {
  Started  = 0,
  Paused   = 1,
  Resumed  = 2,
  Gap      = 3,
  Closed   = 4,
};

const char* to_string(StreamStatusKind kind) noexcept;

struct StreamStatus {
  StreamStatusKind kind = StreamStatusKind::Started;

  std::string reason;

  std::optional<uint64_t> gap_count;

  ErrorCode error_code = ErrorCode::Ok;
};

enum class SubscriptionState {
  Pending = 0,
  Active  = 1,
  Paused  = 2,
  Closed  = 3,
};

const char* to_string(SubscriptionState state) noexcept;

enum class DropPolicy {
  DropOldest = 0,
  DropNewest = 1,

};

enum class CallbackMode {
  Dispatched = 0,
  Inline     = 1,
};

using StatusCallback = std::function<void(const StreamStatus&)>;

class Subscription {
 public:
  virtual ~Subscription();

  virtual std::string topic() const = 0;

  virtual double desired_hz() const = 0;

  virtual SubscriptionState state() const = 0;

  virtual uint64_t last_seq() const = 0;

  virtual uint64_t drop_count() const = 0;

  virtual ErrorCode error() const = 0;

  virtual ErrorCode Unsubscribe() = 0;

 protected:
  Subscription() = default;
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
};

}

#endif

