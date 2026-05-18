#include "proto_codec.hpp"

#include <stdexcept>

#include "sdk/common.pb.h"
#include "sdk/session.pb.h"
#include "sdk/v1/telemetry.pb.h"
#include "karo/v1/common.pb.h"
#include "karo/v1/rpc.pb.h"
#include "karo/v1/control.pb.h"
#include "karo/v1/safety.pb.h"
#include "karo/v1/tasks.pb.h"
#include "karo/v1/telemetry_stream.pb.h"

namespace karo::sdk::detail {

namespace {

std::vector<uint8_t> serialize_or_throw(const google::protobuf::MessageLite& msg)
{
  const auto size = msg.ByteSizeLong();
  std::vector<uint8_t> buf(size);
  if (!msg.SerializeToArray(buf.data(), static_cast<int>(size))) {
    throw SdkException(ErrorCode::InternalError,
      "failed to serialize protobuf message");
  }
  return buf;
}

Capabilities convert_capabilities(const ::karo::sdk::SdkCapabilitySet& src)
{
  Capabilities c;
  c.chassis_control = src.chassis_control();
  c.telemetry_read  = src.telemetry_read();
  c.image_stream    = src.image_stream();
  c.map_read        = src.map_read();
  c.map_write       = src.map_write();
  c.task_control    = src.task_control();
  return c;
}

DeliverySemantics convert_delivery_semantics(int wire) noexcept {
  switch (wire) {
    case ::karo::sdk::TOPIC_EVENT:
      return DeliverySemantics::Event;
    case ::karo::sdk::TOPIC_TELEMETRY:
    default:

      return DeliverySemantics::Telemetry;
  }
}

}

std::vector<uint8_t> encode_handshake_cert(
  uint64_t request_id,
  const std::string& sdk_version,
  const std::string& client_id,
  const std::vector<uint8_t>& cert_der)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(request_id);
  auto* hs = cmd.mutable_handshake();
  hs->set_sdk_version(sdk_version);
  hs->set_client_id(client_id);
  hs->set_certificate_der(cert_der.data(), cert_der.size());
  return serialize_or_throw(cmd);
}

std::vector<uint8_t> encode_handshake_token(
  uint64_t request_id,
  const std::string& sdk_version,
  const std::string& client_id,
  const std::vector<uint8_t>& token)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(request_id);
  auto* hs = cmd.mutable_handshake();
  hs->set_sdk_version(sdk_version);
  hs->set_client_id(client_id);
  hs->set_access_token(token.data(), token.size());
  return serialize_or_throw(cmd);
}

std::vector<uint8_t> encode_ping(uint64_t request_id, uint64_t client_ts_ms)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(request_id);
  cmd.mutable_ping()->set_client_timestamp_ms(client_ts_ms);
  return serialize_or_throw(cmd);
}

std::vector<uint8_t> encode_subscribe(
  uint64_t request_id,
  const std::string& topic,
  double desired_hz,
  uint64_t since_seq,
  uint32_t history_limit)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(request_id);
  auto* sub = cmd.mutable_subscribe();
  sub->set_topic(topic);
  sub->set_desired_hz(static_cast<float>(desired_hz));
  sub->set_since_seq(since_seq);
  sub->set_history_limit(history_limit);
  return serialize_or_throw(cmd);
}

std::vector<uint8_t> encode_unsubscribe(
  uint64_t request_id, const std::string& topic)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(request_id);
  cmd.mutable_unsubscribe()->set_topic(topic);
  return serialize_or_throw(cmd);
}

namespace {
std::vector<uint8_t> wrap_in_sdk_command(uint64_t sdk_request_id,
                                          ::karo::v1::Request& rpc_req)
{
  ::karo::sdk::SdkCommand cmd;
  cmd.set_request_id(sdk_request_id);
  cmd.mutable_rpc()->Swap(&rpc_req);
  return serialize_or_throw(cmd);
}
}

std::vector<uint8_t> encode_cmd_vel(
  uint64_t request_id,
  double linear_x, double linear_y, double angular_z,
  uint64_t sequence, uint64_t client_ts_ms)
{
  ::karo::v1::CmdVelRequest req;
  auto* twist = req.mutable_twist();
  auto* lin   = twist->mutable_linear();
  lin->set_x(linear_x);
  lin->set_y(linear_y);
  lin->set_z(0.0);
  auto* ang   = twist->mutable_angular();
  ang->set_x(0.0);
  ang->set_y(0.0);
  ang->set_z(angular_z);
  req.set_sequence(sequence);
  req.set_timestamp_ms(client_ts_ms);

  ::karo::v1::Request rpc;
  rpc.set_id(request_id);
  rpc.set_method("control.cmd_vel");
  rpc.set_deadline_ms(200);
  std::string payload;
  if (!req.SerializeToString(&payload)) {
    throw SdkException(ErrorCode::InternalError, "serialize CmdVelRequest");
  }
  rpc.set_payload(std::move(payload));

  return wrap_in_sdk_command(request_id, rpc);
}

std::vector<uint8_t> encode_emergency_stop(
  uint64_t request_id, bool engage, const std::string& reason)
{
  ::karo::v1::EmergencyStopRequest req;
  req.set_activate(engage);

  ::karo::v1::Request rpc;
  rpc.set_id(request_id);
  rpc.set_method("safety.emergency_stop");
  rpc.set_deadline_ms(2000);
  if (!reason.empty()) {
    (*rpc.mutable_metadata())["reason"] = reason;
  }
  std::string payload;
  if (!req.SerializeToString(&payload)) {
    throw SdkException(ErrorCode::InternalError, "serialize EmergencyStopRequest");
  }
  rpc.set_payload(std::move(payload));

  return wrap_in_sdk_command(request_id, rpc);
}

namespace {
std::vector<uint8_t> encode_task_rpc(
  uint64_t request_id, const std::string& method,
  const google::protobuf::MessageLite& req)
{
  ::karo::v1::Request rpc;
  rpc.set_id(request_id);
  rpc.set_method(method);
  rpc.set_deadline_ms(2000);
  std::string payload;
  if (!req.SerializeToString(&payload)) {
    throw SdkException(ErrorCode::InternalError, "serialize task request");
  }
  rpc.set_payload(std::move(payload));
  return wrap_in_sdk_command(request_id, rpc);
}
}

std::vector<uint8_t> encode_create_nav_task_marker(
  uint64_t request_id, const std::string& marker_id)
{
  ::karo::v1::CreateNavigationTaskRequest req;
  req.set_marker_id(marker_id);
  return encode_task_rpc(request_id, "tasks.create", req);
}

std::vector<uint8_t> encode_create_nav_task_pose(
  uint64_t request_id, double x, double y, double theta)
{
  ::karo::v1::CreateNavigationTaskRequest req;
  auto* pose = req.mutable_pose();
  pose->set_x(x);
  pose->set_y(y);
  pose->set_theta(theta);
  return encode_task_rpc(request_id, "tasks.create", req);
}

std::vector<uint8_t> encode_cancel_task(
  uint64_t request_id, const std::string& task_id)
{
  ::karo::v1::CancelTaskRequest req;
  req.set_task_id(task_id);
  return encode_task_rpc(request_id, "tasks.cancel", req);
}

std::vector<uint8_t> encode_pause_task(
  uint64_t request_id, const std::string& task_id)
{
  ::karo::v1::PauseTaskRequest req;
  req.set_task_id(task_id);
  return encode_task_rpc(request_id, "tasks.pause", req);
}

std::vector<uint8_t> encode_resume_task(
  uint64_t request_id, const std::string& task_id)
{
  ::karo::v1::ResumeTaskRequest req;
  req.set_task_id(task_id);
  return encode_task_rpc(request_id, "tasks.resume", req);
}

bool decode_response(const uint8_t* data, size_t size, DecodedResponse& out)
{
  ::karo::sdk::SdkResponse resp;
  if (!resp.ParseFromArray(data, static_cast<int>(size))) {
    return false;
  }

  out.request_id   = resp.request_id();
  out.success      = resp.success();
  out.sdk_code     = map_sdk_error_code(resp.error_code());
  out.message      = resp.message();
  out.server_ts_ms = resp.server_timestamp_ms();

  out.has_handshake = false;
  out.has_rpc       = false;
  out.has_subscribe = false;

  if (resp.has_handshake()) {
    out.has_handshake          = true;
    const auto& hs             = resp.handshake();
    out.protocol_version       = hs.protocol_version();
    out.robot_sn               = hs.robot_sn();
    out.robot_model            = hs.robot_model();
    out.session_token          = hs.session_token();
    out.granted_capabilities   = convert_capabilities(hs.granted_capabilities());
    out.available_topics.clear();
    out.available_topics.reserve(hs.available_topics_size());
    for (const auto& t : hs.available_topics()) {
      TopicDescriptor d;
      d.name        = t.name();
      d.description = t.description();
      d.default_hz  = t.default_hz();
      d.max_hz      = t.max_hz();
      d.delivery_semantics = convert_delivery_semantics(
        static_cast<int>(t.delivery_semantics()));
      out.available_topics.push_back(std::move(d));
    }
  } else if (resp.has_rpc()) {
    out.has_rpc      = true;
    const auto& r    = resp.rpc();
    out.rpc_id       = r.id();
    out.rpc_code     = map_v1_error_code(r.code());
    out.rpc_message  = r.message();
    const auto& payload = r.payload();
    out.rpc_payload.assign(payload.begin(), payload.end());
  } else if (resp.has_subscribe()) {
    out.has_subscribe = true;
    const auto& a     = resp.subscribe();
    out.sub_topic     = a.topic();
    out.sub_accepted  = a.accepted();
    out.sub_reject_reason =
      static_cast<SubscribeRejectReason>(a.reject_reason());
    out.sub_next_seq  = a.next_seq();
    out.sub_message   = a.message();
  }

  return true;
}

bool decode_topic_push(const uint8_t* data, size_t size, DecodedTopicPush& out)
{
  ::karo::sdk::SdkTopicPush push;
  if (!push.ParseFromArray(data, static_cast<int>(size))) {
    return false;
  }
  out.topic        = push.topic();
  out.timestamp_ms = push.timestamp_ms();
  out.seq          = push.seq();
  const auto& payload = push.payload();
  out.payload.assign(payload.begin(), payload.end());
  return true;
}

bool decode_cmd_vel_response(
  const std::vector<uint8_t>& payload, DecodedCmdVelResponse& out)
{
  ::karo::v1::CmdVelResponse resp;
  if (!resp.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  out.accepted            = resp.accepted();
  out.device_timestamp_ms = resp.device_timestamp_ms();
  return true;
}

bool decode_ping_response(
  const std::vector<uint8_t>& payload, DecodedPingResponse& out)
{
  ::karo::v1::PingResponse resp;
  if (!resp.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  out.client_timestamp_ms = resp.client_timestamp_ms();
  out.server_timestamp_ms = resp.server_timestamp_ms();
  out.protocol_version    = resp.protocol_version();
  out.device_sn           = resp.device_sn();
  return true;
}

bool decode_robot_status(
  const std::vector<uint8_t>& payload, RobotStatus& out)
{

  ::karo::sdk::v1::SdkRobotStateUpdate src;
  if (!src.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }

  out.robot_id        = src.robot_id();
  out.sequence        = src.sequence();
  out.health_score    = src.health_score();
  out.battery_percent = src.battery_level();
  out.charge_type     = static_cast<RobotStatus::ChargeType>(src.charge_type());
  out.service_state   = static_cast<RobotStatus::ServiceState>(src.service_state());
  out.is_estop        = src.is_estop();
  out.is_hw_estop     = src.is_hw_estop();
  out.is_sw_estop     = src.is_sw_estop();
  out.active_map_id    = src.active_map_id();
  out.active_map_name  = src.active_map_name();
  out.active_floor     = src.active_floor();
  out.current_task_id  = src.current_task_id();
  out.uptime_seconds   = src.uptime_seconds();
  out.idle_seconds     = src.idle_seconds();
  out.charging_seconds = src.charging_seconds();
  out.error_codes.clear();
  out.error_codes.reserve(src.errors_size());
  for (auto code : src.errors()) {
    out.error_codes.push_back(static_cast<int32_t>(code));
  }
  return true;
}

namespace {
TaskType map_task_type(int wire) noexcept {
  switch (wire) {
    case ::karo::v1::TASK_TYPE_NAVIGATION: return TaskType::Navigation;
    case ::karo::v1::TASK_TYPE_PATROL:     return TaskType::Patrol;
    case ::karo::v1::TASK_TYPE_GO_BACK:    return TaskType::GoBack;
    default:                               return TaskType::Unspecified;
  }
}

TaskState map_task_state(int wire) noexcept {
  switch (wire) {
    case ::karo::v1::TASK_STATE_PENDING:    return TaskState::Pending;
    case ::karo::v1::TASK_STATE_RUNNING:    return TaskState::Running;
    case ::karo::v1::TASK_STATE_PAUSED:     return TaskState::Paused;
    case ::karo::v1::TASK_STATE_CANCELLING: return TaskState::Cancelling;
    case ::karo::v1::TASK_STATE_SUCCEEDED:  return TaskState::Succeeded;
    case ::karo::v1::TASK_STATE_FAILED:     return TaskState::Failed;
    case ::karo::v1::TASK_STATE_CANCELLED:  return TaskState::Cancelled;
    default:                                return TaskState::Unspecified;
  }
}

TaskCancelReason map_task_cancel_reason(int wire) noexcept {
  switch (wire) {
    case ::karo::v1::TASK_CANCEL_REASON_USER:          return TaskCancelReason::User;
    case ::karo::v1::TASK_CANCEL_REASON_ESTOP:         return TaskCancelReason::Estop;
    case ::karo::v1::TASK_CANCEL_REASON_TIMEOUT:       return TaskCancelReason::Timeout;
    case ::karo::v1::TASK_CANCEL_REASON_PAUSE_TIMEOUT: return TaskCancelReason::PauseTimeout;
    case ::karo::v1::TASK_CANCEL_REASON_RESUME_FAILED: return TaskCancelReason::ResumeFailed;
    case ::karo::v1::TASK_CANCEL_REASON_ERROR:         return TaskCancelReason::Error;
    default:                                           return TaskCancelReason::Unspecified;
  }
}

TaskPauseReason map_task_pause_reason(int wire) noexcept {
  switch (wire) {
    case ::karo::v1::TASK_PAUSE_REASON_USER:  return TaskPauseReason::User;
    case ::karo::v1::TASK_PAUSE_REASON_ESTOP: return TaskPauseReason::Estop;
    default:                                  return TaskPauseReason::Unspecified;
  }
}
}

bool decode_create_task_response(
  const std::vector<uint8_t>& payload, std::string& task_id_out)
{
  ::karo::v1::CreateNavigationTaskResponse resp;
  if (!resp.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  task_id_out = resp.task_id();
  return true;
}

bool decode_task_event(const std::vector<uint8_t>& payload, TaskEvent& out)
{
  ::karo::v1::Task t;
  if (!t.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  out.task_id         = t.task_id();
  out.task_type       = map_task_type(static_cast<int>(t.task_type()));
  out.state           = map_task_state(static_cast<int>(t.state()));
  out.cancel_reason   = map_task_cancel_reason(static_cast<int>(t.cancel_reason()));
  out.pause_reason    = map_task_pause_reason(static_cast<int>(t.pause_reason()));
  out.error_code      = map_v1_error_code(static_cast<int>(t.error_code()));
  out.error_message   = t.error_message();
  out.created_at_ms   = t.created_at_ms();
  out.started_at_ms   = t.started_at_ms();
  out.completed_at_ms = t.completed_at_ms();
  if (t.has_move()) {
    out.marker_id         = t.move().marker_id();
    out.planned_distance  = t.move().planned_distance();
    out.traveled_distance = t.move().distance_traveled();
  }
  return true;
}

bool decode_safety_event(const std::vector<uint8_t>& payload, SafetyEvent& out)
{
  ::karo::v1::SafetyStateUpdate s;
  if (!s.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  out.code          = s.code();
  out.level         = s.level();
  out.severity      = s.severity();
  out.mode          = s.mode();
  out.active_source = s.active_source();
  return true;
}

ErrorCode map_sdk_error_code(int proto_sdk_code) noexcept
{
  switch (proto_sdk_code) {

    case ::karo::sdk::SDK_ERROR_NONE:               return ErrorCode::Ok;

    case ::karo::sdk::SDK_AUTH_FAILED:              return ErrorCode::AuthFailed;
    case ::karo::sdk::SDK_AUTH_CN_NOT_ALLOWED:      return ErrorCode::AuthCnNotAllowed;
    case ::karo::sdk::SDK_AUTH_SERIAL_REVOKED:      return ErrorCode::AuthSerialRevoked;
    case ::karo::sdk::SDK_AUTH_TOKEN_INVALID:       return ErrorCode::AuthTokenInvalid;
    case ::karo::sdk::SDK_AUTH_TOKEN_EXPIRED:       return ErrorCode::AuthTokenExpired;

    case ::karo::sdk::SDK_SESSION_TOKEN_INVALID:    return ErrorCode::SessionTokenInvalid;
    case ::karo::sdk::SDK_SESSION_TOKEN_EXPIRED:    return ErrorCode::SessionTokenExpired;
    case ::karo::sdk::SDK_HANDSHAKE_REQUIRED:       return ErrorCode::HandshakeRequired;

    case ::karo::sdk::SDK_CAPABILITY_DENIED:        return ErrorCode::CapabilityDenied;
    case ::karo::sdk::SDK_RATE_LIMITED:             return ErrorCode::RateLimited;
    case ::karo::sdk::SDK_UNSUPPORTED_VERSION:      return ErrorCode::UnsupportedVersion;

    case ::karo::sdk::SDK_TOPIC_NOT_FOUND:          return ErrorCode::TopicNotFound;
    case ::karo::sdk::SDK_TOPIC_ALREADY_SUBSCRIBED: return ErrorCode::TopicAlreadySubscribed;
    case ::karo::sdk::SDK_TOPIC_NOT_SUBSCRIBED:     return ErrorCode::TopicNotSubscribed;

    case ::karo::sdk::SDK_DISCONNECTED:             return ErrorCode::Disconnected;
    case ::karo::sdk::SDK_WOULD_DEADLOCK:           return ErrorCode::WouldDeadlock;

    case ::karo::sdk::CONTROL_REJECTED_TASK_RUNNING:
      return ErrorCode::TaskRunning;

    case ::karo::sdk::CONTROL_REJECTED_CHARGING:
    case ::karo::sdk::CONTROL_REJECTED_ESTOP:
    case ::karo::sdk::CONTROL_REJECTED_POSE_ABNORMAL:
    case ::karo::sdk::CONTROL_REJECTED_SHUTTING_DOWN:
    case ::karo::sdk::CONTROL_REJECTED_OBSTACLE_TOO_CLOSE:
      return ErrorCode::ControlEStopActive;
    case ::karo::sdk::CONTROL_REJECTED_MOTOR_FAULT:
    case ::karo::sdk::CONTROL_REJECTED_STEERING_FAULT:
    case ::karo::sdk::CONTROL_REJECTED_CAN_FAULT:
    case ::karo::sdk::CONTROL_REJECTED_NOT_READY:
      return ErrorCode::ControlPublisherUnavailable;

    default:
      return ErrorCode::InternalError;
  }
}

ErrorCode map_v1_error_code(int proto_v1_code) noexcept
{

  switch (proto_v1_code) {
    case 0: return ErrorCode::Ok;

    case 201: return ErrorCode::CapabilityDenied;
    case 202: return ErrorCode::AuthFailed;
    case 203: return ErrorCode::ControlInvalidTwist;
    case 204: return ErrorCode::Cancelled;
    case 205: return ErrorCode::Timeout;

    case 1200: return ErrorCode::NavMarkerNotFound;
    case 1201: return ErrorCode::NavNoActiveMap;
    case 1202: return ErrorCode::TaskRunning;
    case 1203: return ErrorCode::NavLowBattery;
    case 1204: return ErrorCode::ControlEStopActive;
    case 1205: return ErrorCode::NavGoalUnreachable;
    case 1206: return ErrorCode::NavGoalUnreachable;
    case 1207: return ErrorCode::Timeout;
    case 1208: return ErrorCode::TaskRunning;
    case 1209: return ErrorCode::TaskNotFound;
    case 1210: return ErrorCode::ControlPublisherUnavailable;
    case 1211: return ErrorCode::NavGoalUnreachable;
    case 1212: return ErrorCode::NavGoalUnreachable;

    case 9300: return ErrorCode::ControlEStopActive;
    case 9301: return ErrorCode::ControlPublisherUnavailable;
    case 9302: return ErrorCode::ControlInvalidTwist;
    default:

      return ErrorCode::InternalError;
  }
}

}

