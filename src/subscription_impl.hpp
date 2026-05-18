#ifndef KARO_SDK_INTERNAL_SUBSCRIPTION_IMPL_HPP_
#define KARO_SDK_INTERNAL_SUBSCRIPTION_IMPL_HPP_

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "karo/sdk/error.hpp"
#include "karo/sdk/subscription.hpp"
#include "karo/sdk/topic.hpp"

#include "proto_codec.hpp"

namespace karo::sdk::detail {

class RobotImpl;

using FrameDispatcher = std::function<void(const DecodedTopicPush& push)>;

struct SubSlot {

  std::string       topic;
  double            desired_hz;
  DeliverySemantics semantics    = DeliverySemantics::Telemetry;
  CallbackMode      callback_mode = CallbackMode::Dispatched;
  DropPolicy        drop_policy   = DropPolicy::DropOldest;
  uint32_t          queue_size    = 100;

  FrameDispatcher   frame_dispatcher;

  StatusCallback    status_callback;

  std::atomic<SubscriptionState> state{SubscriptionState::Pending};
  std::atomic<uint64_t>          last_seq{0};
  std::atomic<uint64_t>          drop_count{0};

  std::atomic<int>               error_code{static_cast<int>(ErrorCode::Ok)};

  std::deque<DecodedTopicPush>   pending_buffer;

  uint64_t                       inflight_subscribe_request_id = 0;

  bool                           fresh_retry_done = false;

  uint64_t                       expected_resume_seq = 0;

  bool                           ever_active = false;

  std::atomic<bool>              user_closed{false};

  bool                           is_reconnect_resume = false;

  bool                           unsubscribe_sent = false;

  std::chrono::steady_clock::time_point last_message_at;

  std::atomic<uint64_t>          messages_received{0};
  std::array<std::atomic<uint32_t>, 60> hz_buckets{};
  std::atomic<int64_t>           hz_bucket_epoch_ms{0};
};

class SubscriptionFacade : public Subscription {
 public:
  SubscriptionFacade(std::shared_ptr<SubSlot> slot,
                     std::weak_ptr<RobotImpl> robot)
    : slot_(std::move(slot)), robot_(std::move(robot)) {}

  ~SubscriptionFacade() override;

  std::string topic() const override { return slot_->topic; }
  double desired_hz() const override { return slot_->desired_hz; }
  ::karo::sdk::SubscriptionState state() const override {
    return slot_->state.load();
  }
  uint64_t last_seq() const override { return slot_->last_seq.load(); }
  uint64_t drop_count() const override { return slot_->drop_count.load(); }
  ErrorCode error() const override {
    return static_cast<ErrorCode>(slot_->error_code.load());
  }

  ErrorCode Unsubscribe() override;

 private:
  std::shared_ptr<SubSlot>  slot_;
  std::weak_ptr<RobotImpl>  robot_;
};

}

#endif

