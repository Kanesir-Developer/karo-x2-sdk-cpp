#include "robot_impl.hpp"

#include <algorithm>
#include <cstring>
#include <future>
#include <utility>
#ifdef _WIN32
#include <process.h>
#define KARO_GETPID _getpid
#else
#include <unistd.h>
#define KARO_GETPID ::getpid
#endif

#include <boost/asio/post.hpp>

#include "karo/sdk/topic.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace karo::sdk::detail {

namespace {

int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
uint64_t now_steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<uint8_t> pem_to_der(const std::string& pem) {
  if (pem.empty()) return {};
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio) {
    throw SdkException(ErrorCode::InternalError, "BIO_new_mem_buf failed");
  }
  X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!x) {
    throw SdkException(ErrorCode::AuthFailed, "PEM_read_bio_X509 failed");
  }
  unsigned char* der_buf = nullptr;
  int der_len = i2d_X509(x, &der_buf);
  X509_free(x);
  if (der_len <= 0 || !der_buf) {
    if (der_buf) OPENSSL_free(der_buf);
    throw SdkException(ErrorCode::InternalError, "i2d_X509 failed");
  }
  std::vector<uint8_t> der(der_buf, der_buf + der_len);
  OPENSSL_free(der_buf);
  return der;
}

ErrorCode reject_reason_to_error(SubscribeRejectReason r) noexcept {
  switch (r) {
    case SubscribeRejectReason::None: return ErrorCode::Ok;
    case SubscribeRejectReason::TopicNotFound: return ErrorCode::TopicNotFound;
    case SubscribeRejectReason::CapabilityDenied: return ErrorCode::CapabilityDenied;
    case SubscribeRejectReason::AlreadySubscribed: return ErrorCode::TopicAlreadySubscribed;
    case SubscribeRejectReason::InvalidHz: return ErrorCode::ControlInvalidTwist;
    case SubscribeRejectReason::HistoryTooOld:
    case SubscribeRejectReason::ReplayNotSupported:

      return ErrorCode::InternalError;
  }
  return ErrorCode::InternalError;
}

}

void RttHistogram::record(int64_t rtt_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  samples_.push_back(rtt_ms);
  if (samples_.size() > kCap) samples_.pop_front();
}
double RttHistogram::p50_ms() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (samples_.empty()) return 0.0;
  std::vector<int64_t> s(samples_.begin(), samples_.end());
  std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
  return static_cast<double>(s[s.size() / 2]);
}
double RttHistogram::p99_ms() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (samples_.empty()) return 0.0;
  std::vector<int64_t> s(samples_.begin(), samples_.end());
  const size_t idx = (s.size() * 99) / 100;
  std::nth_element(s.begin(), s.begin() + idx, s.end());
  return static_cast<double>(s[idx]);
}

SubscriptionFacade::~SubscriptionFacade() {

  slot_->user_closed.store(true);

  if (auto r = robot_.lock()) {
    auto cur_state = slot_->state.load();
    if (cur_state != SubscriptionState::Closed) {
      try {
        (void)r->UnsubscribeRequest(slot_->topic, false);
      } catch (...) {  }
    }
  }
}

ErrorCode SubscriptionFacade::Unsubscribe() {
  auto r = robot_.lock();
  if (!r) {
    slot_->state.store(SubscriptionState::Closed);
    slot_->error_code.store(static_cast<int>(ErrorCode::Disconnected));
    return ErrorCode::Disconnected;
  }
  return r->UnsubscribeRequest(slot_->topic, true);
}

std::shared_ptr<RobotImpl> RobotImpl::Create(const ConnectOptions& opts) {
  auto p = std::shared_ptr<RobotImpl>(new RobotImpl(opts));
  p->ValidateOpts();

  p->control_work_.emplace(boost::asio::make_work_guard(p->control_ioc_));
  p->control_thread_ = std::thread([self = p] {
    self->RunControlLoop();
  });
  return p;
}

RobotImpl::RobotImpl(const ConnectOptions& opts)
  : opts_(opts),
    idle_timer_(control_ioc_),
    ping_deadline_timer_(control_ioc_),
    rng_(static_cast<uint32_t>(
      std::chrono::steady_clock::now().time_since_epoch().count() ^
      static_cast<uint32_t>(KARO_GETPID()))),
    reconnect_timer_(control_ioc_),

    cmdvel_rate_limiter_(20.0, 1.0)
{
  last_state_change_ms_.store(now_epoch_ms());
}

RobotImpl::~RobotImpl() {
  Close();
}

void RobotImpl::ValidateOpts() const {
  if (opts_.host.empty()) {
    throw SdkException(ErrorCode::TransportFailure, "ConnectOptions.host is empty");
  }
  if (opts_.host.find(':') != std::string::npos) {
    throw SdkException(ErrorCode::TransportFailure,
      "ConnectOptions.host must not contain port; SDK uses fixed port 4434");
  }
  if (opts_.cert.cert_pem.empty() && opts_.access_token.token.empty()) {
    throw SdkException(ErrorCode::AuthFailed,
      "neither CertCredentials nor AccessTokenCredentials provided");
  }
  if (!opts_.cert.cert_pem.empty() && opts_.cert.key_pem.empty()) {
    throw SdkException(ErrorCode::AuthFailed,
      "CertCredentials.cert_pem provided but key_pem empty");
  }
  if (opts_.heartbeat_interval.count() < 0) {
    throw SdkException(ErrorCode::InternalError,
      "heartbeat_interval must be >= 0");
  }
  if (opts_.reconnect_max_delay < opts_.reconnect_base_delay) {
    throw SdkException(ErrorCode::InternalError,
      "reconnect_max_delay < reconnect_base_delay");
  }
  if (opts_.reconnect_jitter_ratio < 0.0 || opts_.reconnect_jitter_ratio > 1.0) {
    throw SdkException(ErrorCode::InternalError,
      "reconnect_jitter_ratio out of [0, 1]");
  }
  if (opts_.subscription_queue_size == 0) {
    throw SdkException(ErrorCode::InternalError,
      "subscription_queue_size must be >= 1");
  }

}

void RobotImpl::SetConnectionStateHandler(ConnectionStateHandler h) {
  std::lock_guard<std::mutex> lk(handler_mu_);
  state_handler_ = std::move(h);
}

void RobotImpl::SetLogHandler(LogHandler h) {
  log_sink_.set_handler(std::move(h));
}

void RobotImpl::Connect() {

  boost::asio::post(control_ioc_, [self = shared_from_this()] {
    if (self->state_.load() != ConnectionState::Idle) return;
    self->EstablishConnection(false);
  });
}

void RobotImpl::Close() {
  if (shutdown_requested_.exchange(true)) {

    if (control_thread_.joinable() &&
        std::this_thread::get_id() != control_thread_.get_id()) {
      control_thread_.join();
    }
    return;
  }

  boost::asio::post(control_ioc_, [self = shared_from_this()] {
    auto old = self->state_.load();
    self->TransitionTo(ConnectionState::Shutdown,
      StateChangeInfo{ "user Close()", ErrorCode::Cancelled, {}, {}, self->reconnect_attempt_, {} });
    boost::system::error_code ec;
    self->idle_timer_.cancel();
    self->ping_deadline_timer_.cancel();
    self->reconnect_timer_.cancel();
    for (auto& [rid, t] : self->sub_ack_timers_) {
      if (t) t->cancel();
    }
    self->sub_ack_timers_.clear();
    for (auto& [rid, rpc] : self->inflight_rpc_) {
      if (rpc.on_complete) {
        rpc.on_complete(DecodedResponse{}, ErrorCode::Cancelled);
      }
      if (rpc.timeout_timer) rpc.timeout_timer->cancel();
    }
    self->inflight_rpc_.clear();
    for (auto& [topic, slot] : self->active_subs_) {
      auto prev = slot->state.exchange(SubscriptionState::Closed);
      if (prev != SubscriptionState::Closed) {
        slot->error_code.store(static_cast<int>(ErrorCode::Cancelled));
        self->FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                         ErrorCode::Cancelled, "Robot::Close()");
      }
    }
    self->active_subs_.clear();
    if (self->ws_) self->ws_->Close();
    self->ws_.reset();
    self->control_work_.reset();
    (void)old;
  });

  if (control_thread_.joinable() &&
      std::this_thread::get_id() != control_thread_.get_id()) {
    control_thread_.join();
  }
  dispatcher_.Shutdown();
}

std::optional<RobotInfo> RobotImpl::info() const {
  std::lock_guard<std::mutex> lk(info_mu_);
  return robot_info_;
}

void RobotImpl::RunControlLoop() {
  try {
    control_ioc_.run();
  } catch (...) {

  }
}

void RobotImpl::TransitionTo(ConnectionState new_s, StateChangeInfo info) {
  auto old = state_.exchange(new_s);
  if (old == new_s) return;
  last_state_change_ms_.store(now_epoch_ms());

  if (info.last_error_code != ErrorCode::Ok) {
    last_error_code_.store(static_cast<int>(info.last_error_code));
    std::lock_guard<std::mutex> lk(last_error_msg_mu_);
    last_error_msg_ = info.last_error_message;
  }

  PostLog(LogLevel::Info,
    std::string("state transition ") + to_string(old) + " -> " + to_string(new_s) +
    (info.reason.empty() ? "" : " (" + info.reason + ")"));

  info.reconnect_attempt = reconnect_attempt_;
  PostStateCallback(old, new_s, std::move(info));
}

void RobotImpl::PostStateCallback(ConnectionState old_s, ConnectionState new_s,
                                  StateChangeInfo info) {
  ConnectionStateHandler h;
  {
    std::lock_guard<std::mutex> lk(handler_mu_);
    h = state_handler_;
  }
  if (!h) return;
  dispatcher_.Post([h = std::move(h), old_s, new_s, info = std::move(info)] {
    try { h(old_s, new_s, info); } catch (...) {}
  });
}

void RobotImpl::PostLog(LogLevel level, std::string msg) {
  if (level < log_sink_.min_level()) return;
  auto handler = log_sink_.take_handler();
  if (handler) {
    dispatcher_.Post([handler = std::move(handler), level, msg = std::move(msg)] {
      try { handler(level, msg); } catch (...) {}
    });
  } else {

    log_sink_.default_stderr_write(level, msg);
  }
}

int64_t RobotImpl::SlideHzBuckets(const std::shared_ptr<SubSlot>& slot) {
  auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  int64_t cur_sec = epoch_ms / 1000;
  int64_t last_epoch_ms = slot->hz_bucket_epoch_ms.load();
  int64_t last_sec = last_epoch_ms / 1000;
  if (cur_sec == last_sec && last_epoch_ms > 0) return cur_sec;
  if (last_epoch_ms == 0) {
    slot->hz_bucket_epoch_ms.store(epoch_ms);
    return cur_sec;
  }
  int64_t gap = cur_sec - last_sec;
  if (gap > 60) gap = 60;
  for (int64_t i = 1; i <= gap; ++i) {
    slot->hz_buckets[(last_sec + i) % 60].store(0);
  }
  slot->hz_bucket_epoch_ms.store(epoch_ms);
  return cur_sec;
}

bool RobotImpl::SendOrFail(std::vector<uint8_t> bytes) noexcept {
  if (!ws_) return false;
  size_t n = bytes.size();
  if (!ws_->Post(std::move(bytes))) return false;
  bytes_sent_.fetch_add(n);
  return true;
}

void RobotImpl::AbortToFatal(ErrorCode code, const std::string& message) {

  for (auto& [topic, slot] : active_subs_) {
    auto prev = slot->state.exchange(SubscriptionState::Closed);
    if (prev != SubscriptionState::Closed) {
      slot->error_code.store(static_cast<int>(code));
      FireStatus(slot, StreamStatusKind::Closed, std::nullopt, code,
                 message.empty() ? "Fatal" : message);
    }
  }
  active_subs_.clear();

  for (auto& [rid, rpc] : inflight_rpc_) {
    if (rpc.timeout_timer) rpc.timeout_timer->cancel();
    if (rpc.on_complete) {
      DecodedResponse fake{};
      fake.sdk_code = code;
      fake.message  = message;
      rpc.on_complete(fake, code);
    }
  }
  inflight_rpc_.clear();
  for (auto& [rid, t] : sub_ack_timers_) {
    if (t) t->cancel();
  }
  sub_ack_timers_.clear();
  inflight_sub_request_.clear();

  idle_timer_.cancel();
  ping_deadline_timer_.cancel();
  reconnect_timer_.cancel();
  heartbeat_.ping_deadline_active = false;

  if (ws_) ws_->Close();
  ws_.reset();
  TransitionTo(ConnectionState::Fatal,
    { message, code, message, {}, reconnect_attempt_, {} });
}

void RobotImpl::EstablishConnection(bool is_reconnect) {
  if (!is_reconnect) {
    if (state_.load() != ConnectionState::Idle) return;
    TransitionTo(ConnectionState::Connecting,
      { "Connect()", ErrorCode::Ok, {}, {}, 0, {} });
  } else {

    if (state_.load() != ConnectionState::Reconnecting) return;
  }

  WsClient::Config cfg;
  cfg.endpoint = opts_.host + ":4434";
  cfg.cert_pem = opts_.cert.cert_pem;
  cfg.key_pem  = opts_.cert.key_pem;
  cfg.ca_pem   = opts_.cert.ca_pem;
  cfg.insecure_skip_verify = opts_.cert.insecure_skip_verify;
  cfg.connect_timeout = opts_.connect_timeout;

  std::weak_ptr<RobotImpl> weak = shared_from_this();
  std::unique_ptr<WsClient> ws;
  try {
    ws = std::make_unique<WsClient>(cfg);
  } catch (const SdkException& e) {
    auto cat = classify(e.code());
    if (cat == ErrorCategory::Fatal) {
      AbortToFatal(e.code(), std::string("ws_client setup fatal: ") + e.what());
      return;
    }

    TransitionTo(ConnectionState::TransientFailure,
      { "ws_client setup failed", e.code(), e.what(),
        NextBackoff(reconnect_attempt_), 0, {} });
    DoReconnect(reconnect_attempt_ + 1);
    return;
  }

  ws->SetMessageHandler([weak](std::vector<uint8_t> bytes) {
    if (auto self = weak.lock()) {

      boost::asio::post(self->control_ioc_,
        [self, bytes = std::move(bytes)]() mutable {
          self->OnInboundMessage(std::move(bytes));
        });
    }
  });
  ws->SetCloseHandler([weak](ErrorCode reason, std::string msg) {
    if (auto self = weak.lock()) {
      boost::asio::post(self->control_ioc_,
        [self, reason, msg = std::move(msg)]() mutable {
          self->OnTransportClose(reason, std::move(msg));
        });
    }
  });

  try {
    ws->Connect();
  } catch (const SdkException& e) {
    auto cat = classify(e.code());
    if (cat == ErrorCategory::Fatal) {
      AbortToFatal(e.code(), std::string("ws connect fatal: ") + e.what());
      return;
    }
    TransitionTo(ConnectionState::TransientFailure,
      { "ws connect failed", e.code(), e.what(),
        NextBackoff(reconnect_attempt_), 0, {} });
    DoReconnect(reconnect_attempt_ + 1);
    return;
  }

  ws_ = std::move(ws);

  if (!is_reconnect) {
    bytes_sent_.store(0);
    bytes_received_.store(0);
  }

  uint64_t hs_rid = NextRequestId();
  std::vector<uint8_t> hs_bytes;
  try {
    if (!opts_.cert.cert_pem.empty()) {
      auto der = pem_to_der(opts_.cert.cert_pem);
      hs_bytes = encode_handshake_cert(
        hs_rid,
        opts_.sdk_version.empty() ? ::karo::sdk::sdk_version() : opts_.sdk_version.c_str(),
        opts_.client_id, der);
    } else {
      hs_bytes = encode_handshake_token(
        hs_rid,
        opts_.sdk_version.empty() ? ::karo::sdk::sdk_version() : opts_.sdk_version.c_str(),
        opts_.client_id, opts_.access_token.token);
    }
  } catch (const SdkException& e) {
    AbortToFatal(e.code(), std::string("handshake encode: ") + e.what());
    return;
  }

  InflightRpc rpc;
  rpc.request_id = hs_rid;
  rpc.method = "handshake";
  rpc.enqueued_at = std::chrono::steady_clock::now();
  rpc.on_complete = [self = shared_from_this(), is_reconnect]
                    (const DecodedResponse& resp, ErrorCode err) {

    if (err != ErrorCode::Ok) {

      return;
    }
    if (!resp.success || !resp.has_handshake) {
      auto code = resp.success ? ErrorCode::InternalError : resp.sdk_code;
      auto cat = classify(code);
      if (cat == ErrorCategory::Fatal) {
        self->AbortToFatal(code, "handshake rejected: " + resp.message);
        return;
      }
      self->TransitionTo(ConnectionState::TransientFailure,
        { "handshake failed", code, resp.message,
          self->NextBackoff(self->reconnect_attempt_), 0, {} });
      if (self->ws_) self->ws_->Close();
      self->ws_.reset();
      self->DoReconnect(self->reconnect_attempt_ + 1);
      return;
    }

    RobotInfo ri;
    ri.sn                 = resp.robot_sn;
    ri.model              = resp.robot_model;
    ri.protocol_version   = resp.protocol_version;
    ri.granted_capabilities = resp.granted_capabilities;
    ri.available_topics   = resp.available_topics;
    {
      std::lock_guard<std::mutex> lk(self->info_mu_);
      self->robot_info_ = std::move(ri);
    }

    self->StartHeartbeat();

    if (!is_reconnect) {

      self->TransitionTo(ConnectionState::Ready,
        { "handshake ok", ErrorCode::Ok, {}, {}, 0, {} });

      for (auto& [topic, slot] : self->active_subs_) {
        if (slot->state.load() == SubscriptionState::Pending &&
            slot->inflight_subscribe_request_id == 0) {
          self->SendSubscribe(slot, 0);
        }
      }
      return;
    }

    self->reconnect_subs_pending_ = self->active_subs_.size();
    self->reconnect_estop_pending_ = (self->last_estop_intent_ == EStopIntent::Engaged);
    self->reconnect_failure_code_ = ErrorCode::Ok;
    if (self->reconnect_subs_pending_ == 0 && !self->reconnect_estop_pending_) {
      self->reconnect_attempt_ = 0;
      self->reconnect_total_count_++;
      self->TransitionTo(ConnectionState::Ready,
        { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
      return;
    }
    self->ResubscribeAllAfterReconnect();
    if (self->reconnect_estop_pending_) self->DoEStopReplay();
  };

  auto timer = std::make_shared<boost::asio::steady_timer>(control_ioc_);
  rpc.timeout_timer = timer;
  timer->expires_after(opts_.connect_timeout);
  timer->async_wait([self = shared_from_this(), hs_rid](const boost::system::error_code& ec) {
    if (ec) return;
    auto it = self->inflight_rpc_.find(hs_rid);
    if (it == self->inflight_rpc_.end()) return;
    auto on_complete = std::move(it->second.on_complete);
    self->inflight_rpc_.erase(it);
    if (on_complete) {
      DecodedResponse fake{};
      fake.sdk_code = ErrorCode::Timeout;
      fake.message  = "handshake timeout";
      on_complete(fake, ErrorCode::Timeout);
    }
    if (self->ws_) self->ws_->Close();
    self->ws_.reset();
  });

  inflight_rpc_[hs_rid] = std::move(rpc);

  if (!SendOrFail(std::move(hs_bytes))) {
    auto it = inflight_rpc_.find(hs_rid);
    if (it != inflight_rpc_.end()) {
      if (it->second.timeout_timer) it->second.timeout_timer->cancel();
      inflight_rpc_.erase(it);
    }
    TransitionTo(ConnectionState::TransientFailure,
      { "handshake send failed", ErrorCode::TransportFailure,
        "ws not open", NextBackoff(reconnect_attempt_), 0, {} });
    if (ws_) ws_->Close();
    ws_.reset();
    DoReconnect(reconnect_attempt_ + 1);
  }
}

void RobotImpl::OnInboundMessage(std::vector<uint8_t> bytes) {
  bytes_received_.fetch_add(bytes.size());
  ResetLivenessOnInbound();

  DecodedResponse resp;
  if (decode_response(bytes.data(), bytes.size(), resp) &&
      (resp.has_handshake || resp.has_rpc || resp.has_subscribe ||
       resp.request_id != 0)) {
    HandleSdkResponse(resp);
    return;
  }
  DecodedTopicPush push;
  if (decode_topic_push(bytes.data(), bytes.size(), push) && !push.topic.empty()) {
    HandleSdkTopicPush(push);
    return;
  }
  PostLog(LogLevel::Warn, "unknown wire message (size=" +
    std::to_string(bytes.size()) + ")");
}

void RobotImpl::OnTransportClose(ErrorCode reason, std::string msg) {
  auto s = state_.load();
  if (s == ConnectionState::Shutdown || s == ConnectionState::Fatal) return;

  for (auto& [topic, slot] : active_subs_) {
    auto prev = slot->state.exchange(SubscriptionState::Paused);
    if (prev == SubscriptionState::Active || prev == SubscriptionState::Pending) {
      FireStatus(slot, StreamStatusKind::Paused, std::nullopt, reason, msg);
    }

    slot->pending_buffer.clear();
    slot->inflight_subscribe_request_id = 0;
  }

  for (auto& [rid, rpc] : inflight_rpc_) {
    boost::system::error_code ec;
    if (rpc.timeout_timer) rpc.timeout_timer->cancel();
    if (rpc.on_complete) {
      DecodedResponse fake{};
      fake.sdk_code = reason;
      fake.message  = msg;
      rpc.on_complete(fake, reason);
    }
  }
  inflight_rpc_.clear();
  for (auto& [rid, t] : sub_ack_timers_) {
    boost::system::error_code ec;
    if (t) t->cancel();
  }
  sub_ack_timers_.clear();

  boost::system::error_code ec;
  idle_timer_.cancel();
  ping_deadline_timer_.cancel();
  heartbeat_.ping_deadline_active = false;
  heartbeat_.miss_count = 0;

  TransitionTo(ConnectionState::TransientFailure,
    { msg.empty() ? std::string("transport closed") : msg,
      reason, msg, NextBackoff(reconnect_attempt_), reconnect_attempt_, {} });

  if (ws_) ws_->Close();
  ws_.reset();
  DoReconnect(reconnect_attempt_ + 1);
}

void RobotImpl::HandleSdkResponse(const DecodedResponse& resp) {

  if (resp.has_subscribe) {
    HandleSubscribeAck(resp);
    return;
  }

  auto it = inflight_rpc_.find(resp.request_id);
  if (it == inflight_rpc_.end()) {
    PostLog(LogLevel::Debug, "response for unknown request_id=" +
      std::to_string(resp.request_id));
    return;
  }
  auto on_complete = std::move(it->second.on_complete);
  if (it->second.timeout_timer) {
    boost::system::error_code ec;
    it->second.timeout_timer->cancel();
  }
  inflight_rpc_.erase(it);
  if (on_complete) on_complete(resp, ErrorCode::Ok);
}

void RobotImpl::HandleSdkTopicPush(const DecodedTopicPush& push) {
  auto it = active_subs_.find(push.topic);
  if (it == active_subs_.end()) {
    PostLog(LogLevel::Debug, "topic push for unknown sub: " + push.topic);
    return;
  }
  auto slot = it->second;
  auto cur = slot->state.load();
  if (cur == SubscriptionState::Closed) return;

  if (cur == SubscriptionState::Pending) {
    if (slot->pending_buffer.size() >= slot->queue_size) {
      if (slot->drop_policy == DropPolicy::DropNewest) {
        slot->drop_count.fetch_add(1);
        return;
      }

      slot->pending_buffer.pop_front();
      slot->drop_count.fetch_add(1);
    }
    slot->pending_buffer.push_back(push);
    return;
  }

  if (cur == SubscriptionState::Paused) {

    slot->drop_count.fetch_add(1);
    return;
  }

  DeliverPushToSub(slot, push);
}

namespace {
DeliverySemantics resolve_semantics_from_info(
  const std::optional<RobotInfo>& info, const std::string& topic) {
  if (!info) return DeliverySemantics::Telemetry;
  for (const auto& d : info->available_topics) {
    if (d.name == topic) return d.delivery_semantics;
  }
  return DeliverySemantics::Telemetry;
}
}

std::unique_ptr<Subscription> RobotImpl::SubscribeInternal(
  const std::string& topic,
  double desired_hz,
  FrameDispatcher frame_dispatcher,
  StatusCallback on_status) {

  auto slot = std::make_shared<SubSlot>();
  slot->topic = topic;
  slot->desired_hz = desired_hz;
  slot->callback_mode = opts_.callback_mode;
  slot->drop_policy = opts_.drop_policy;
  slot->queue_size = opts_.subscription_queue_size;
  slot->status_callback = std::move(on_status);
  slot->frame_dispatcher = std::move(frame_dispatcher);
  {
    std::lock_guard<std::mutex> lk(info_mu_);
    slot->semantics = resolve_semantics_from_info(robot_info_, topic);
  }

  boost::asio::post(control_ioc_,
    [self = shared_from_this(), slot] {
      if (slot->user_closed.load()) {
        slot->state.store(SubscriptionState::Closed);
        return;
      }
      if (self->active_subs_.count(slot->topic)) {
        slot->state.store(SubscriptionState::Closed);
        slot->error_code.store(static_cast<int>(ErrorCode::TopicAlreadySubscribed));
        self->FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                         ErrorCode::TopicAlreadySubscribed, "already subscribed");
        return;
      }
      self->active_subs_[slot->topic] = slot;
      if (self->state_.load() != ConnectionState::Ready) return;
      self->SendSubscribe(slot, 0);
    });

  return std::make_unique<SubscriptionFacade>(std::move(slot), weak_from_this());
}

std::unique_ptr<Subscription> RobotImpl::SubscribeRobotStatus(
  double desired_hz,
  Robot::RobotStatusCallback on_data,
  StatusCallback on_status) {

  FrameDispatcher fd = [on_data](const DecodedTopicPush& push) {
    RobotStatus rs;
    if (decode_robot_status(push.payload, rs)) {
      rs.timestamp_ms = push.timestamp_ms;
      try { on_data(rs); } catch (...) {}
    }
  };
  return SubscribeInternal("robot.state", desired_hz, std::move(fd),
                           std::move(on_status));
}

std::unique_ptr<Subscription> RobotImpl::SubscribeTaskEvents(
  Robot::TaskEventCallback on_data,
  StatusCallback on_status) {

  FrameDispatcher fd = [on_data](const DecodedTopicPush& push) {
    TaskEvent ev;
    if (decode_task_event(push.payload, ev)) {
      ev.timestamp_ms = push.timestamp_ms;
      try { on_data(ev); } catch (...) {}
    }
  };
  return SubscribeInternal("tasks.events", 0.0, std::move(fd),
                           std::move(on_status));
}

std::unique_ptr<Subscription> RobotImpl::SubscribeSafetyEvents(
  Robot::SafetyEventCallback on_data,
  StatusCallback on_status) {

  FrameDispatcher fd = [on_data](const DecodedTopicPush& push) {
    SafetyEvent ev;
    if (decode_safety_event(push.payload, ev)) {
      ev.timestamp_ms = push.timestamp_ms;
      try { on_data(ev); } catch (...) {}
    }
  };
  return SubscribeInternal("safety.events", 0.0, std::move(fd),
                           std::move(on_status));
}

void RobotImpl::SendSubscribe(const std::shared_ptr<SubSlot>& slot, uint64_t since_seq) {
  uint64_t rid = NextRequestId();
  std::vector<uint8_t> bytes;
  try {

    uint64_t effective_since = since_seq;
    if (slot->semantics == DeliverySemantics::Telemetry) {
      effective_since = 0;
    }
    bytes = encode_subscribe(rid, slot->topic, slot->desired_hz,
                              effective_since,
                              effective_since > 0 ? opts_.history_limit : 0);
    slot->expected_resume_seq = effective_since;
  } catch (const std::exception& e) {
    slot->state.store(SubscriptionState::Closed);
    slot->error_code.store(static_cast<int>(ErrorCode::InternalError));
    FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
               ErrorCode::InternalError, e.what());
    active_subs_.erase(slot->topic);
    return;
  }

  auto old_state = slot->state.load();
  slot->is_reconnect_resume = (old_state == SubscriptionState::Paused) ||
    (slot->ever_active && state_.load() == ConnectionState::Reconnecting);
  slot->state.store(SubscriptionState::Pending);
  slot->inflight_subscribe_request_id = rid;
  inflight_sub_request_[rid] = slot->topic;

  auto timer = std::make_shared<boost::asio::steady_timer>(control_ioc_);
  timer->expires_after(opts_.subscribe_ack_timeout);
  std::weak_ptr<RobotImpl> weak = shared_from_this();
  std::weak_ptr<SubSlot> wslot = slot;
  timer->async_wait([weak, wslot, rid](const boost::system::error_code& ec) {
    if (ec) return;
    auto self = weak.lock();
    auto s = wslot.lock();
    if (!self || !s) return;
    if (s->inflight_subscribe_request_id != rid) return;

    self->PostLog(LogLevel::Warn, "SubscribeAck timeout for " + s->topic);
    self->OnTransportClose(ErrorCode::Timeout, "SubscribeAck timeout");
  });
  sub_ack_timers_[rid] = timer;

  if (!SendOrFail(std::move(bytes))) {
    timer->cancel();
    sub_ack_timers_.erase(rid);
    inflight_sub_request_.erase(rid);
    slot->inflight_subscribe_request_id = 0;
    OnTransportClose(ErrorCode::TransportFailure, "SendSubscribe failed");
  }
}

void RobotImpl::ResubscribeAllAfterReconnect() {
  for (auto& [topic, slot] : active_subs_) {
    slot->fresh_retry_done = false;
    uint64_t since = 0;
    if (slot->semantics == DeliverySemantics::Event &&
        opts_.reconnect_replay_history &&
        slot->last_seq.load() > 0) {
      since = slot->last_seq.load() + 1;
    }
    SendSubscribe(slot, since);
  }
}

void RobotImpl::HandleSubscribeAck(const DecodedResponse& resp) {
  auto rit = inflight_sub_request_.find(resp.request_id);
  if (rit == inflight_sub_request_.end()) {
    PostLog(LogLevel::Debug, "SubscribeAck for unknown request_id=" +
      std::to_string(resp.request_id));
    return;
  }
  std::string topic = rit->second;
  inflight_sub_request_.erase(rit);

  auto tit = sub_ack_timers_.find(resp.request_id);
  if (tit != sub_ack_timers_.end()) {
    boost::system::error_code ec;
    if (tit->second) tit->second->cancel();
    sub_ack_timers_.erase(tit);
  }

  auto sit = active_subs_.find(topic);
  if (sit == active_subs_.end()) {

    return;
  }
  auto slot = sit->second;
  if (slot->inflight_subscribe_request_id != resp.request_id) {

    return;
  }
  slot->inflight_subscribe_request_id = 0;

  auto is_reconnect_phase = [&] {
    return state_.load() == ConnectionState::Reconnecting;
  };

  if (!resp.success) {
    auto cat = classify(resp.sdk_code);
    if (cat == ErrorCategory::Fatal) {

      AbortToFatal(resp.sdk_code,
        "subscribe fatal: " + resp.message);
      return;
    }
    if (cat == ErrorCategory::Transient) {
      if (is_reconnect_phase()) {

        reconnect_failure_code_ = resp.sdk_code;
        reconnect_failure_message_ = resp.message;
        OnTransportClose(resp.sdk_code, resp.message);
      } else {

        slot->state.store(SubscriptionState::Closed);
        slot->error_code.store(static_cast<int>(resp.sdk_code));
        FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                   resp.sdk_code, resp.message);
        active_subs_.erase(slot->topic);
      }
      return;
    }

    slot->state.store(SubscriptionState::Closed);
    slot->error_code.store(static_cast<int>(resp.sdk_code));
    FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
               resp.sdk_code, resp.message);
    active_subs_.erase(slot->topic);
    if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
      reconnect_subs_pending_--;
      if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
        reconnect_attempt_ = 0;
        reconnect_total_count_++;
        TransitionTo(ConnectionState::Ready,
          { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
      }
    }
    return;
  }

  if (slot->semantics == DeliverySemantics::Telemetry) {
    if (!resp.sub_accepted) {
      ErrorCode err = reject_reason_to_error(resp.sub_reject_reason);
      slot->state.store(SubscriptionState::Closed);
      slot->error_code.store(static_cast<int>(err));
      FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                 err, resp.sub_message);
      active_subs_.erase(slot->topic);
      if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
        reconnect_subs_pending_--;
        if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
          reconnect_attempt_ = 0;
          reconnect_total_count_++;
          TransitionTo(ConnectionState::Ready,
            { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
        }
      }
      return;
    }

    slot->last_seq.store(0);
    slot->state.store(SubscriptionState::Active);

    if (slot->is_reconnect_resume) {
      FireStatus(slot, StreamStatusKind::Resumed, std::nullopt );
    } else {
      FireStatus(slot, StreamStatusKind::Started, std::nullopt);
    }
    DrainPendingBuffer(slot, 0);
    slot->ever_active = true;
    slot->is_reconnect_resume = false;
    if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
      reconnect_subs_pending_--;
      if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
        reconnect_attempt_ = 0;
        reconnect_total_count_++;
        TransitionTo(ConnectionState::Ready,
          { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
      }
    }
    return;
  }

  if (!resp.sub_accepted) {
    if (resp.sub_reject_reason == SubscribeRejectReason::HistoryTooOld ||
        resp.sub_reject_reason == SubscribeRejectReason::ReplayNotSupported) {
      if (slot->fresh_retry_done) {

        slot->state.store(SubscriptionState::Closed);
        slot->error_code.store(static_cast<int>(ErrorCode::InternalError));
        FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                   ErrorCode::InternalError,
                   "fresh subscribe rejected after replay fallback");
        active_subs_.erase(slot->topic);
        if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
          reconnect_subs_pending_--;
          if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
            reconnect_attempt_ = 0;
            reconnect_total_count_++;
            TransitionTo(ConnectionState::Ready,
              { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
          }
        }
        return;
      }

      slot->last_seq.store(0);
      slot->fresh_retry_done = true;
      SendSubscribe(slot, 0);
      return;
    }

    ErrorCode err = reject_reason_to_error(resp.sub_reject_reason);
    slot->state.store(SubscriptionState::Closed);
    slot->error_code.store(static_cast<int>(err));
    FireStatus(slot, StreamStatusKind::Closed, std::nullopt, err, resp.sub_message);
    active_subs_.erase(slot->topic);
    if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
      reconnect_subs_pending_--;
      if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
        reconnect_attempt_ = 0;
        reconnect_total_count_++;
        TransitionTo(ConnectionState::Ready,
          { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
      }
    }
    return;
  }

  uint64_t K = resp.sub_next_seq;
  uint64_t expected = slot->expected_resume_seq;

  bool is_resume = slot->is_reconnect_resume;
  StreamStatusKind kind = StreamStatusKind::Started;
  std::optional<uint64_t> gap = std::nullopt;

  if (expected == 0) {

    if (K >= 1) {
      slot->last_seq.store(K - 1);
    } else {
      slot->last_seq.store(0);
    }
    if (is_resume) {
      kind = StreamStatusKind::Resumed;
      gap = std::nullopt;
    } else {
      kind = StreamStatusKind::Started;
    }
  } else {

    if (K == expected) {

      kind = StreamStatusKind::Resumed;
      gap = 0;
    } else if (K > expected) {

      uint64_t gap_count = K - expected;
      slot->last_seq.store(K - 1);
      kind = StreamStatusKind::Resumed;
      gap = gap_count;
    } else {

      slot->last_seq.store(K >= 1 ? K - 1 : 0);
      kind = StreamStatusKind::Resumed;
      gap = std::nullopt;
    }
  }

  slot->state.store(SubscriptionState::Active);

  FireStatus(slot, kind, gap);
  DrainPendingBuffer(slot, K);
  slot->ever_active = true;
  slot->is_reconnect_resume = false;

  if (is_reconnect_phase() && reconnect_subs_pending_ > 0) {
    reconnect_subs_pending_--;
    if (reconnect_subs_pending_ == 0 && !reconnect_estop_pending_) {
      reconnect_attempt_ = 0;
      reconnect_total_count_++;
      TransitionTo(ConnectionState::Ready,
        { "reconnect ok", ErrorCode::Ok, {}, {}, 0, {} });
    }
  }
}

void RobotImpl::DeliverPushToSub(const std::shared_ptr<SubSlot>& slot,
                                  const DecodedTopicPush& push) {

  if (slot->semantics == DeliverySemantics::Event) {
    uint64_t new_seq = push.seq;
    uint64_t last = slot->last_seq.load();

    if (new_seq == 0) {

      PostLog(LogLevel::Trace,
        "EVENT push seq=0 for " + slot->topic + " (gap detection off)");
    } else if (last == 0) {

      slot->last_seq.store(new_seq);
    } else if (new_seq == last + 1) {
      slot->last_seq.store(new_seq);
    } else if (new_seq > last + 1) {
      uint64_t gap_count = new_seq - last - 1;
      slot->last_seq.store(new_seq);
      FireStatus(slot, StreamStatusKind::Gap, gap_count);
    } else if (new_seq == last) {
      PostLog(LogLevel::Debug, "duplicate envelope seq, dropping: " + slot->topic);
      return;
    } else {

      PostLog(LogLevel::Warn, "envelope seq regression in-session for " +
        slot->topic + " new=" + std::to_string(new_seq) +
        " last=" + std::to_string(last));
      return;
    }
  } else {

  }

  slot->messages_received.fetch_add(1);
  slot->last_message_at = std::chrono::steady_clock::now();

  int64_t cur_sec = SlideHzBuckets(slot);
  slot->hz_buckets[cur_sec % 60].fetch_add(1);

  auto disp = slot->frame_dispatcher;
  if (!disp) return;
  if (slot->callback_mode == CallbackMode::Inline) {

    try { disp(push); } catch (...) {}
  } else {
    auto push_copy = push;
    dispatcher_.Post([disp = std::move(disp), push = std::move(push_copy)] {
      if (disp) disp(push);
    });
  }
}

void RobotImpl::DrainPendingBuffer(const std::shared_ptr<SubSlot>& slot,
                                    uint64_t next_seq) {

  while (!slot->pending_buffer.empty()) {
    auto front = std::move(slot->pending_buffer.front());
    slot->pending_buffer.pop_front();
    if (slot->semantics == DeliverySemantics::Event &&
        next_seq > 0 && front.seq != 0 && front.seq < next_seq) {
      slot->drop_count.fetch_add(1);
      continue;
    }
    DeliverPushToSub(slot, front);
  }
}

void RobotImpl::FireStatus(const std::shared_ptr<SubSlot>& slot,
                            StreamStatusKind k, std::optional<uint64_t> gap,
                            ErrorCode err, const std::string& reason) {
  StreamStatus s;
  s.kind = k;
  s.gap_count = gap;
  s.error_code = err;
  s.reason = reason;
  auto cb = slot->status_callback;
  if (!cb) return;
  dispatcher_.Post([cb = std::move(cb), s = std::move(s)] {
    try { cb(s); } catch (...) {}
  });
}

ErrorCode RobotImpl::UnsubscribeRequest(const std::string& topic, bool sync_wait) {

  if (!sync_wait) {
    std::weak_ptr<RobotImpl> weak = weak_from_this();
    boost::asio::post(control_ioc_, [weak, topic] {
      auto self = weak.lock();
      if (!self) return;
      auto it = self->active_subs_.find(topic);
      if (it == self->active_subs_.end()) return;
      auto slot = it->second;

      if (self->state_.load() == ConnectionState::Ready &&
          self->ws_ && self->ws_->IsOpen() && !slot->unsubscribe_sent) {
        try {
          auto bytes = encode_unsubscribe(self->NextRequestId(), topic);
          self->bytes_sent_.fetch_add(bytes.size());
          self->ws_->Post(std::move(bytes));
          slot->unsubscribe_sent = true;
        } catch (...) {}
      }
      auto prev = slot->state.exchange(SubscriptionState::Closed);
      if (prev != SubscriptionState::Closed) {
        self->FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                         ErrorCode::Ok, "Unsubscribe()");
      }
      self->active_subs_.erase(it);
      self->PostLog(LogLevel::Warn,
        "subscription dropped without explicit Unsubscribe() — topic=" + topic);
    });
    return ErrorCode::Ok;
  }

  if (dispatcher_.IsInDispatcherThread()) {
    return ErrorCode::WouldDeadlock;
  }

  auto promise = std::make_shared<std::promise<ErrorCode>>();
  auto fut = promise->get_future();
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  boost::asio::post(control_ioc_, [weak, topic, promise] {
    auto self = weak.lock();
    if (!self) { promise->set_value(ErrorCode::Disconnected); return; }
    auto it = self->active_subs_.find(topic);
    if (it == self->active_subs_.end()) { promise->set_value(ErrorCode::Ok); return; }
    auto slot = it->second;

    if (self->state_.load() != ConnectionState::Ready || !self->ws_ || !self->ws_->IsOpen()) {
      slot->state.store(SubscriptionState::Closed);
      slot->error_code.store(static_cast<int>(ErrorCode::Disconnected));
      self->FireStatus(slot, StreamStatusKind::Closed, std::nullopt,
                       ErrorCode::Disconnected, "Unsubscribe() while disconnected");
      self->active_subs_.erase(it);
      promise->set_value(ErrorCode::Disconnected);
      return;
    }
    uint64_t rid = self->NextRequestId();
    InflightRpc rpc;
    rpc.request_id = rid;
    rpc.method = "unsubscribe";
    rpc.enqueued_at = std::chrono::steady_clock::now();
    rpc.on_complete = [self, topic, promise](const DecodedResponse& resp, ErrorCode err) {
      if (err != ErrorCode::Ok) { promise->set_value(err); return; }
      auto it2 = self->active_subs_.find(topic);
      if (it2 != self->active_subs_.end()) {
        auto s = it2->second;
        s->state.store(SubscriptionState::Closed);
        s->error_code.store(static_cast<int>(resp.success ? ErrorCode::Ok : resp.sdk_code));
        self->FireStatus(s, StreamStatusKind::Closed, std::nullopt,
                         resp.success ? ErrorCode::Ok : resp.sdk_code, resp.message);
        self->active_subs_.erase(it2);
      }
      promise->set_value(resp.success ? ErrorCode::Ok : resp.sdk_code);
    };
    auto timer = std::make_shared<boost::asio::steady_timer>(self->control_ioc_);
    rpc.timeout_timer = timer;
    timer->expires_after(self->opts_.subscribe_ack_timeout);
    timer->async_wait([self, rid, promise](const boost::system::error_code& ec) {
      if (ec) return;
      auto it3 = self->inflight_rpc_.find(rid);
      if (it3 != self->inflight_rpc_.end()) {
        self->inflight_rpc_.erase(it3);
        promise->set_value(ErrorCode::Timeout);
      }
    });
    self->inflight_rpc_[rid] = std::move(rpc);
    try {
      auto bytes = encode_unsubscribe(rid, topic);
      if (!self->SendOrFail(std::move(bytes))) {
        self->inflight_rpc_.erase(rid);
        promise->set_value(ErrorCode::TransportFailure);
        return;
      }
      slot->unsubscribe_sent = true;
    } catch (const std::exception& e) {
      self->inflight_rpc_.erase(rid);
      promise->set_value(ErrorCode::TransportFailure);
      self->PostLog(LogLevel::Error, std::string("Unsubscribe send: ") + e.what());
    }
  });
  return fut.get();
}

CmdVelResult RobotImpl::CmdVel(double vx, double vy, double wz) {
  CmdVelResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.accepted = false;
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    rpc_stat_cmdvel_.error_count.fetch_add(1);
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous CmdVel called from SDK callback";
    return r;
  }
  if (!cmdvel_rate_limiter_.try_acquire()) {
    r.code = ErrorCode::RateLimited;
    r.message = "client-side rate limit (20 Hz)";
    rpc_stat_cmdvel_.error_count.fetch_add(1);
    return r;
  }
  rpc_stat_cmdvel_.count.fetch_add(1);

  auto promise = std::make_shared<std::promise<CmdVelResult>>();
  auto fut = promise->get_future();
  auto t0 = std::chrono::steady_clock::now();
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  static std::atomic<uint64_t> cmdvel_seq{0};
  uint64_t seq = cmdvel_seq.fetch_add(1);
  boost::asio::post(control_ioc_,
    [weak, vx, vy, wz, seq, t0, promise] {
    auto self = weak.lock();
    if (!self) { promise->set_value({false, ErrorCode::Disconnected, "Robot gone", {}}); return; }
    if (self->state_.load() != ConnectionState::Ready || !self->ws_) {
      promise->set_value({false, ErrorCode::Disconnected, "not Ready", {}});
      return;
    }
    uint64_t rid = self->NextRequestId();
    InflightRpc rpc;
    rpc.request_id = rid;
    rpc.method = "control.cmd_vel";
    rpc.enqueued_at = t0;
    rpc.on_complete = [self, t0, promise](const DecodedResponse& resp, ErrorCode err) {
      CmdVelResult cr;
      if (err != ErrorCode::Ok) {
        cr.code = err;
        cr.message = "transport error";
        self->rpc_stat_cmdvel_.error_count.fetch_add(1);
        promise->set_value(cr);
        return;
      }
      cr.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
      self->rpc_stat_cmdvel_.latency_hist.record(cr.rtt.count());
      if (!resp.success) {
        cr.code = resp.sdk_code;
        cr.message = resp.message;
        self->rpc_stat_cmdvel_.error_count.fetch_add(1);
        promise->set_value(cr);
        return;
      }
      if (resp.has_rpc && resp.rpc_code != ErrorCode::Ok) {
        cr.code = resp.rpc_code;
        cr.message = resp.rpc_message;
        self->rpc_stat_cmdvel_.error_count.fetch_add(1);
        promise->set_value(cr);
        return;
      }
      cr.accepted = true;
      cr.code = ErrorCode::Ok;
      promise->set_value(cr);
    };
    auto timer = std::make_shared<boost::asio::steady_timer>(self->control_ioc_);
    rpc.timeout_timer = timer;
    timer->expires_after(self->opts_.cmdvel_rpc_timeout);
    timer->async_wait([self, rid, promise](const boost::system::error_code& ec) {
      if (ec) return;
      auto it = self->inflight_rpc_.find(rid);
      if (it != self->inflight_rpc_.end()) {
        self->inflight_rpc_.erase(it);
        promise->set_value({false, ErrorCode::Timeout, "cmd_vel timeout", {}});
        self->rpc_stat_cmdvel_.error_count.fetch_add(1);
      }
    });
    self->inflight_rpc_[rid] = std::move(rpc);
    try {
      auto bytes = encode_cmd_vel(rid, vx, vy, wz, seq, now_steady_ms());
      if (!self->SendOrFail(std::move(bytes))) {
        self->inflight_rpc_.erase(rid);
        promise->set_value({false, ErrorCode::TransportFailure, "ws not open", {}});
        self->rpc_stat_cmdvel_.error_count.fetch_add(1);
        return;
      }
    } catch (const std::exception& e) {
      self->inflight_rpc_.erase(rid);
      promise->set_value({false, ErrorCode::TransportFailure, e.what(), {}});
      self->rpc_stat_cmdvel_.error_count.fetch_add(1);
    }
  });
  return fut.get();
}

EmergencyStopResult RobotImpl::EmergencyStopRpc(bool engage, const std::string& reason) {
  EmergencyStopResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.ok = false;
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous EmergencyStop called from SDK callback";
    return r;
  }
  auto promise = std::make_shared<std::promise<EmergencyStopResult>>();
  auto fut = promise->get_future();
  auto t0 = std::chrono::steady_clock::now();
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  boost::asio::post(control_ioc_, [weak, engage, reason, t0, promise] {
    auto self = weak.lock();
    if (!self) { promise->set_value({false, ErrorCode::Disconnected, "Robot gone"}); return; }
    if (self->state_.load() != ConnectionState::Ready || !self->ws_) {
      promise->set_value({false, ErrorCode::Disconnected, "not Ready"});
      return;
    }
    uint64_t rid = self->NextRequestId();
    InflightRpc rpc;
    rpc.request_id = rid;
    rpc.method = "safety.emergency_stop";
    rpc.enqueued_at = t0;
    rpc.on_complete = [self, engage, reason, t0, promise]
                      (const DecodedResponse& resp, ErrorCode err) {
      EmergencyStopResult er;
      self->rpc_stat_estop_.count.fetch_add(1);
      if (err != ErrorCode::Ok) {
        er.code = err; er.message = "transport error";
        self->rpc_stat_estop_.error_count.fetch_add(1);
        promise->set_value(er); return;
      }
      auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
      self->rpc_stat_estop_.latency_hist.record(rtt);
      if (!resp.success) {
        er.code = resp.sdk_code; er.message = resp.message;
        self->rpc_stat_estop_.error_count.fetch_add(1);
        promise->set_value(er); return;
      }
      if (resp.has_rpc && resp.rpc_code != ErrorCode::Ok) {
        er.code = resp.rpc_code; er.message = resp.rpc_message;
        self->rpc_stat_estop_.error_count.fetch_add(1);
        promise->set_value(er); return;
      }
      er.ok = true;
      er.code = ErrorCode::Ok;

      self->last_estop_intent_ = engage ? EStopIntent::Engaged : EStopIntent::None;
      self->last_estop_reason_ = reason;
      promise->set_value(er);
    };
    auto timer = std::make_shared<boost::asio::steady_timer>(self->control_ioc_);
    rpc.timeout_timer = timer;
    timer->expires_after(self->opts_.estop_rpc_timeout);
    timer->async_wait([self, rid, promise](const boost::system::error_code& ec) {
      if (ec) return;
      auto it = self->inflight_rpc_.find(rid);
      if (it != self->inflight_rpc_.end()) {
        self->inflight_rpc_.erase(it);
        promise->set_value({false, ErrorCode::Timeout, "estop timeout"});
        self->rpc_stat_estop_.error_count.fetch_add(1);
      }
    });
    self->inflight_rpc_[rid] = std::move(rpc);
    try {
      auto bytes = encode_emergency_stop(rid, engage, reason);
      if (!self->SendOrFail(std::move(bytes))) {
        self->inflight_rpc_.erase(rid);
        promise->set_value({false, ErrorCode::TransportFailure, "ws not open"});
        self->rpc_stat_estop_.error_count.fetch_add(1);
        return;
      }
    } catch (const std::exception& e) {
      self->inflight_rpc_.erase(rid);
      promise->set_value({false, ErrorCode::TransportFailure, e.what()});
      self->rpc_stat_estop_.error_count.fetch_add(1);
    }
  });
  return fut.get();
}

RobotImpl::RpcOutcome RobotImpl::InvokeRpc(
  const std::string& method,
  std::chrono::milliseconds deadline,
  std::function<std::vector<uint8_t>(uint64_t)> encode) {

  auto promise = std::make_shared<std::promise<RpcOutcome>>();
  auto fut = promise->get_future();
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  boost::asio::post(control_ioc_, [weak, method, deadline, encode, promise] {
    auto self = weak.lock();
    if (!self) {
      promise->set_value({ErrorCode::Disconnected, "Robot gone", {}});
      return;
    }
    if (self->state_.load() != ConnectionState::Ready || !self->ws_) {
      promise->set_value({ErrorCode::Disconnected, "not Ready", {}});
      return;
    }
    uint64_t rid = self->NextRequestId();
    InflightRpc rpc;
    rpc.request_id = rid;
    rpc.method = method;
    rpc.enqueued_at = std::chrono::steady_clock::now();
    rpc.on_complete = [promise](const DecodedResponse& resp, ErrorCode err) {
      RpcOutcome o;
      if (err != ErrorCode::Ok) {
        o.code = err;
        o.message = "transport error";
        promise->set_value(std::move(o));
        return;
      }
      if (!resp.success) {
        o.code = resp.sdk_code;
        o.message = resp.message;
        promise->set_value(std::move(o));
        return;
      }
      if (!resp.has_rpc) {
        o.code = ErrorCode::InternalError;
        o.message = "missing rpc payload in response";
        promise->set_value(std::move(o));
        return;
      }
      if (resp.rpc_code != ErrorCode::Ok) {
        o.code = resp.rpc_code;
        o.message = resp.rpc_message;
        promise->set_value(std::move(o));
        return;
      }
      o.code = ErrorCode::Ok;
      o.payload = resp.rpc_payload;
      promise->set_value(std::move(o));
    };
    auto timer = std::make_shared<boost::asio::steady_timer>(self->control_ioc_);
    rpc.timeout_timer = timer;
    timer->expires_after(deadline);
    timer->async_wait([self, rid, promise](const boost::system::error_code& ec) {
      if (ec) return;
      auto it = self->inflight_rpc_.find(rid);
      if (it != self->inflight_rpc_.end()) {
        self->inflight_rpc_.erase(it);
        promise->set_value({ErrorCode::Timeout, "rpc timeout", {}});
      }
    });
    self->inflight_rpc_[rid] = std::move(rpc);
    try {
      auto bytes = encode(rid);
      if (!self->SendOrFail(std::move(bytes))) {
        self->inflight_rpc_.erase(rid);
        promise->set_value({ErrorCode::TransportFailure, "ws not open", {}});
        return;
      }
    } catch (const std::exception& e) {
      self->inflight_rpc_.erase(rid);
      promise->set_value({ErrorCode::TransportFailure, e.what(), {}});
    }
  });
  return fut.get();
}

CreateTaskResult RobotImpl::CreateNavigationTaskToMarker(const std::string& marker_id) {
  CreateTaskResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous task RPC called from SDK callback";
    return r;
  }
  auto out = InvokeRpc("tasks.create", opts_.estop_rpc_timeout,
    [marker_id](uint64_t rid) {
      return encode_create_nav_task_marker(rid, marker_id);
    });
  r.code = out.code;
  r.message = out.message;
  if (out.code == ErrorCode::Ok) {
    if (decode_create_task_response(out.payload, r.task_id) &&
        !r.task_id.empty()) {
      r.accepted = true;
    } else {
      r.code = ErrorCode::InternalError;
      r.message = "malformed CreateNavigationTask response";
    }
  }
  return r;
}

CreateTaskResult RobotImpl::CreateNavigationTaskToPose(
  double x, double y, double theta) {
  CreateTaskResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous task RPC called from SDK callback";
    return r;
  }
  auto out = InvokeRpc("tasks.create", opts_.estop_rpc_timeout,
    [x, y, theta](uint64_t rid) {
      return encode_create_nav_task_pose(rid, x, y, theta);
    });
  r.code = out.code;
  r.message = out.message;
  if (out.code == ErrorCode::Ok) {
    if (decode_create_task_response(out.payload, r.task_id) &&
        !r.task_id.empty()) {
      r.accepted = true;
    } else {
      r.code = ErrorCode::InternalError;
      r.message = "malformed CreateNavigationTask response";
    }
  }
  return r;
}

TaskCommandResult RobotImpl::CancelTask(const std::string& task_id) {
  TaskCommandResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous task RPC called from SDK callback";
    return r;
  }
  auto out = InvokeRpc("tasks.cancel", opts_.estop_rpc_timeout,
    [task_id](uint64_t rid) { return encode_cancel_task(rid, task_id); });
  r.code = out.code;
  r.message = out.message;
  r.ok = (out.code == ErrorCode::Ok);
  return r;
}

TaskCommandResult RobotImpl::PauseTask(const std::string& task_id) {
  TaskCommandResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous task RPC called from SDK callback";
    return r;
  }
  auto out = InvokeRpc("tasks.pause", opts_.estop_rpc_timeout,
    [task_id](uint64_t rid) { return encode_pause_task(rid, task_id); });
  r.code = out.code;
  r.message = out.message;
  r.ok = (out.code == ErrorCode::Ok);
  return r;
}

TaskCommandResult RobotImpl::ResumeTask(const std::string& task_id) {
  TaskCommandResult r;
  if (state_.load() != ConnectionState::Ready) {
    r.code = ErrorCode::Disconnected;
    r.message = "Robot is not Ready";
    return r;
  }
  if (dispatcher_.IsInDispatcherThread()) {
    r.code = ErrorCode::WouldDeadlock;
    r.message = "synchronous task RPC called from SDK callback";
    return r;
  }
  auto out = InvokeRpc("tasks.resume", opts_.estop_rpc_timeout,
    [task_id](uint64_t rid) { return encode_resume_task(rid, task_id); });
  r.code = out.code;
  r.message = out.message;
  r.ok = (out.code == ErrorCode::Ok);
  return r;
}

std::chrono::milliseconds RobotImpl::Ping() {
  if (state_.load() != ConnectionState::Ready) {
    return std::chrono::milliseconds(-1);
  }
  if (dispatcher_.IsInDispatcherThread()) {
    return std::chrono::milliseconds(-1);
  }
  auto promise = std::make_shared<std::promise<std::chrono::milliseconds>>();
  auto fut = promise->get_future();
  auto t0 = std::chrono::steady_clock::now();
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  boost::asio::post(control_ioc_, [weak, t0, promise] {
    auto self = weak.lock();
    if (!self) { promise->set_value(std::chrono::milliseconds(-1)); return; }
    if (self->state_.load() != ConnectionState::Ready || !self->ws_) {
      promise->set_value(std::chrono::milliseconds(-1));
      return;
    }
    uint64_t rid = self->NextRequestId();
    InflightRpc rpc;
    rpc.request_id = rid;
    rpc.method = "ping";
    rpc.enqueued_at = t0;
    rpc.on_complete = [self, t0, promise]
                      (const DecodedResponse& resp, ErrorCode err) {
      if (err != ErrorCode::Ok) {
        promise->set_value(std::chrono::milliseconds(-1));
        return;
      }
      auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
      self->rtt_hist_.record(rtt.count());
      promise->set_value(rtt);
    };
    auto timer = std::make_shared<boost::asio::steady_timer>(self->control_ioc_);
    rpc.timeout_timer = timer;
    timer->expires_after(self->opts_.ping_timeout);
    timer->async_wait([self, rid, promise](const boost::system::error_code& ec) {
      if (ec) return;
      auto it = self->inflight_rpc_.find(rid);
      if (it != self->inflight_rpc_.end()) {
        self->inflight_rpc_.erase(it);
        promise->set_value(std::chrono::milliseconds(-1));
      }
    });
    self->inflight_rpc_[rid] = std::move(rpc);
    try {
      auto bytes = encode_ping(rid, now_steady_ms());
      if (!self->SendOrFail(std::move(bytes))) {
        self->inflight_rpc_.erase(rid);
        promise->set_value(std::chrono::milliseconds(-1));
        return;
      }
      self->last_ping_sent_ms_.store(now_epoch_ms());
    } catch (...) {
      self->inflight_rpc_.erase(rid);
      promise->set_value(std::chrono::milliseconds(-1));
    }
  });
  return fut.get();
}

void RobotImpl::StartHeartbeat() {
  heartbeat_.last_inbound_at = std::chrono::steady_clock::now();
  heartbeat_.miss_count = 0;
  heartbeat_.ping_deadline_active = false;
  if (opts_.heartbeat_interval.count() <= 0) return;

  std::weak_ptr<RobotImpl> weak = weak_from_this();
  idle_timer_.expires_after(opts_.heartbeat_interval);
  idle_timer_.async_wait([weak](const boost::system::error_code& ec) {
    if (ec) return;
    if (auto self = weak.lock()) self->OnIdleTimerExpired();
  });
}

void RobotImpl::OnIdleTimerExpired() {
  if (state_.load() != ConnectionState::Ready) return;
  auto now = std::chrono::steady_clock::now();
  auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
    now - heartbeat_.last_inbound_at);
  if (idle < opts_.heartbeat_interval) {

    auto remaining = opts_.heartbeat_interval - idle;
    std::weak_ptr<RobotImpl> weak = weak_from_this();
    idle_timer_.expires_after(remaining);
    idle_timer_.async_wait([weak](const boost::system::error_code& ec) {
      if (ec) return;
      if (auto self = weak.lock()) self->OnIdleTimerExpired();
    });
    return;
  }

  if (!ws_ || !ws_->IsOpen()) return;
  uint64_t rid = NextRequestId();
  heartbeat_.last_ping_request_id = rid;
  heartbeat_.last_ping_sent_at = now;
  heartbeat_.ping_deadline_active = true;

  InflightRpc rpc;
  rpc.request_id = rid;
  rpc.method = "ping";
  rpc.enqueued_at = now;
  rpc.on_complete = [self = shared_from_this(), rid, sent = now]
                    (const DecodedResponse& resp, ErrorCode err) {
    if (err != ErrorCode::Ok) return;
    if (resp.has_rpc && resp.rpc_id == self->heartbeat_.last_ping_request_id) {
      auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sent).count();
      self->rtt_hist_.record(rtt);
    }
    (void)rid;
  };
  inflight_rpc_[rid] = std::move(rpc);

  std::weak_ptr<RobotImpl> weak = weak_from_this();
  ping_deadline_timer_.expires_after(opts_.ping_timeout);
  ping_deadline_timer_.async_wait([weak](const boost::system::error_code& ec) {
    if (ec) return;
    if (auto self = weak.lock()) self->OnPingDeadlineExpired();
  });

  try {
    auto bytes = encode_ping(rid, now_steady_ms());
    if (!SendOrFail(std::move(bytes))) {
      OnTransportClose(ErrorCode::TransportFailure, "heartbeat send failed");
      return;
    }
    last_ping_sent_ms_.store(now_epoch_ms());
  } catch (...) {
    OnTransportClose(ErrorCode::TransportFailure, "heartbeat send failed");
    return;
  }

  idle_timer_.expires_after(opts_.heartbeat_interval);
  idle_timer_.async_wait([weak](const boost::system::error_code& ec) {
    if (ec) return;
    if (auto self = weak.lock()) self->OnIdleTimerExpired();
  });
}

void RobotImpl::OnPingDeadlineExpired() {
  if (state_.load() != ConnectionState::Ready) return;
  if (!heartbeat_.ping_deadline_active) return;
  heartbeat_.ping_deadline_active = false;
  heartbeat_.miss_count++;
  if (heartbeat_.miss_count >= opts_.max_missed_pings) {
    PostLog(LogLevel::Warn,
      "heartbeat miss_count=" + std::to_string(heartbeat_.miss_count) +
      " >= max_missed_pings, declaring dead");
    OnTransportClose(ErrorCode::TransportFailure, "heartbeat dead");
    return;
  }

  if (!ws_ || !ws_->IsOpen()) return;
  uint64_t rid = NextRequestId();
  heartbeat_.last_ping_request_id = rid;
  heartbeat_.last_ping_sent_at = std::chrono::steady_clock::now();
  heartbeat_.ping_deadline_active = true;

  InflightRpc rpc;
  rpc.request_id = rid;
  rpc.method = "ping";
  rpc.enqueued_at = heartbeat_.last_ping_sent_at;
  inflight_rpc_[rid] = std::move(rpc);

  std::weak_ptr<RobotImpl> weak = weak_from_this();
  ping_deadline_timer_.expires_after(opts_.ping_timeout);
  ping_deadline_timer_.async_wait([weak](const boost::system::error_code& ec) {
    if (ec) return;
    if (auto self = weak.lock()) self->OnPingDeadlineExpired();
  });
  try {
    auto bytes = encode_ping(rid, now_steady_ms());
    if (!SendOrFail(std::move(bytes))) {
      OnTransportClose(ErrorCode::TransportFailure, "heartbeat resend failed");
    }
  } catch (...) {
    OnTransportClose(ErrorCode::TransportFailure, "heartbeat resend failed");
  }
}

void RobotImpl::ResetLivenessOnInbound() {
  heartbeat_.last_inbound_at = std::chrono::steady_clock::now();
  heartbeat_.miss_count = 0;
  if (heartbeat_.ping_deadline_active) {
    boost::system::error_code ec;
    ping_deadline_timer_.cancel();
    heartbeat_.ping_deadline_active = false;
  }
}

std::chrono::milliseconds RobotImpl::NextBackoff(uint32_t attempt) {

  double base_ms = static_cast<double>(opts_.reconnect_base_delay.count());
  double max_ms  = static_cast<double>(opts_.reconnect_max_delay.count());
  double exp_ms = base_ms;
  for (uint32_t i = 0; i < attempt && exp_ms < max_ms; ++i) exp_ms *= 2.0;
  if (exp_ms > max_ms) exp_ms = max_ms;

  double jr = std::max(0.05, opts_.reconnect_jitter_ratio);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  double j = exp_ms * jr * dist(rng_);
  double actual = exp_ms - j;
  if (actual < 0) actual = 0;
  return std::chrono::milliseconds(static_cast<int64_t>(actual));
}

void RobotImpl::DoReconnect(uint32_t attempt) {
  if (state_.load() == ConnectionState::Fatal ||
      state_.load() == ConnectionState::Shutdown) return;
  if (opts_.reconnect_max_attempts >= 0 &&
      static_cast<int32_t>(attempt) > opts_.reconnect_max_attempts) {
    AbortToFatal(ErrorCode::TransportFailure, "reconnect_max_attempts reached");
    return;
  }
  auto delay = NextBackoff(attempt - 1);
  reconnect_attempt_ = attempt - 1;
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  reconnect_timer_.expires_after(delay);
  reconnect_timer_.async_wait([weak, attempt](const boost::system::error_code& ec) {
    if (ec) return;
    auto self = weak.lock();
    if (!self) return;
    if (self->state_.load() != ConnectionState::TransientFailure) return;
    self->reconnect_attempt_ = attempt;
    self->TransitionTo(ConnectionState::Reconnecting,
      { "reconnecting", ErrorCode::Ok, {}, {}, attempt, {} });

    self->EstablishConnection(true);
  });
}

void RobotImpl::DoEStopReplay() {
  uint64_t rid = NextRequestId();
  InflightRpc rpc;
  rpc.request_id = rid;
  rpc.method = "safety.emergency_stop.replay";
  rpc.enqueued_at = std::chrono::steady_clock::now();
  rpc.on_complete = [self = shared_from_this()]
                    (const DecodedResponse& resp, ErrorCode err) {
    bool ok = (err == ErrorCode::Ok) && resp.success &&
              (!resp.has_rpc || resp.rpc_code == ErrorCode::Ok);
    self->reconnect_estop_pending_ = false;
    if (!ok) {

      self->OnTransportClose(
        err != ErrorCode::Ok ? err : (resp.success ? resp.rpc_code : resp.sdk_code),
        "estop replay failed");
      return;
    }

    if (self->reconnect_subs_pending_ == 0) {
      self->reconnect_attempt_ = 0;
      self->reconnect_total_count_++;
      self->TransitionTo(ConnectionState::Ready,
        { "reconnect ok (estop replayed)", ErrorCode::Ok, {}, {}, 0, "estop_replay_ok" });
    }
  };
  auto timer = std::make_shared<boost::asio::steady_timer>(control_ioc_);
  rpc.timeout_timer = timer;
  timer->expires_after(opts_.estop_rpc_timeout);
  std::weak_ptr<RobotImpl> weak = weak_from_this();
  timer->async_wait([weak, rid](const boost::system::error_code& ec) {
    if (ec) return;
    auto self = weak.lock();
    if (!self) return;
    auto it = self->inflight_rpc_.find(rid);
    if (it != self->inflight_rpc_.end()) {
      self->inflight_rpc_.erase(it);
      self->reconnect_estop_pending_ = false;
      self->OnTransportClose(ErrorCode::Timeout, "estop replay timeout");
    }
  });
  inflight_rpc_[rid] = std::move(rpc);
  try {
    auto bytes = encode_emergency_stop(rid, true,
      last_estop_reason_ + " [auto-replayed after reconnect]");
    if (!SendOrFail(std::move(bytes))) {
      inflight_rpc_.erase(rid);
      reconnect_estop_pending_ = false;
      OnTransportClose(ErrorCode::TransportFailure, "estop replay send failed");
      return;
    }
  } catch (const std::exception& e) {
    inflight_rpc_.erase(rid);
    reconnect_estop_pending_ = false;
    OnTransportClose(ErrorCode::TransportFailure, e.what());
  }
}

Diagnostics RobotImpl::GetDiagnostics() const {

  auto is_control = (std::this_thread::get_id() == control_thread_.get_id());
  if (dispatcher_.IsInDispatcherThread() || is_control) {
    Diagnostics d;
    d.connection.state = state_.load();
    d.connection.last_state_change_ms = last_state_change_ms_.load();
    d.transport.rtt_p50_ms = rtt_hist_.p50_ms();
    d.transport.rtt_p99_ms = rtt_hist_.p99_ms();
    d.transport.bytes_sent = bytes_sent_.load();
    d.transport.bytes_received = bytes_received_.load();
    return d;
  }
  auto promise = std::make_shared<std::promise<Diagnostics>>();
  auto fut = promise->get_future();

  auto* self = const_cast<RobotImpl*>(this);
  boost::asio::post(self->control_ioc_, [self, promise] {
    Diagnostics d;
    d.connection.state = self->state_.load();
    d.connection.last_state_change_ms = self->last_state_change_ms_.load();
    d.connection.reconnect_count = self->reconnect_total_count_;
    d.connection.last_error_code =
      static_cast<ErrorCode>(self->last_error_code_.load());
    {
      std::lock_guard<std::mutex> lk(self->last_error_msg_mu_);
      d.connection.last_error_message = self->last_error_msg_;
    }
    {
      std::lock_guard<std::mutex> lk(self->info_mu_);
      if (self->robot_info_) {
        d.connection.server_protocol_version = self->robot_info_->protocol_version;
      }
    }
    d.transport.rtt_p50_ms = self->rtt_hist_.p50_ms();
    d.transport.rtt_p99_ms = self->rtt_hist_.p99_ms();
    d.transport.bytes_sent = self->bytes_sent_.load();
    d.transport.bytes_received = self->bytes_received_.load();
    int64_t last_ping = self->last_ping_sent_ms_.load();
    d.transport.last_ping_age_ms = (last_ping < 0)
      ? -1 : (now_epoch_ms() - last_ping);
    for (const auto& [topic, slot] : self->active_subs_) {
      Diagnostics::SubscriptionStat s;
      s.topic = topic;
      s.state = slot->state.load();
      s.configured_hz = slot->desired_hz;

      self->SlideHzBuckets(slot);
      uint64_t total = 0;
      for (const auto& b : slot->hz_buckets) total += b.load();
      s.received_hz_1min = static_cast<double>(total) / 60.0;
      s.drop_count = slot->drop_count.load();
      s.last_seq = slot->last_seq.load();
      auto now = std::chrono::steady_clock::now();
      s.last_message_age_ms = (slot->last_message_at == std::chrono::steady_clock::time_point{})
        ? -1
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            now - slot->last_message_at).count();
      d.subscriptions.push_back(std::move(s));
    }
    auto fill_rpc = [&](const char* name, const RpcStat& s) {
      Diagnostics::RpcStat r;
      r.name = name;
      r.count = s.count.load();
      r.error_count = s.error_count.load();
      r.p99_latency_ms = s.latency_hist.p99_ms();
      d.rpcs.push_back(r);
    };
    fill_rpc("cmd_vel", self->rpc_stat_cmdvel_);
    fill_rpc("emergency_stop", self->rpc_stat_estop_);
    fill_rpc("ping", self->rpc_stat_ping_);
    promise->set_value(std::move(d));
  });
  return fut.get();
}

}

