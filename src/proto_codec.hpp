#ifndef KARO_SDK_PROTO_CODEC_HPP_
#define KARO_SDK_PROTO_CODEC_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "karo/sdk/capabilities.hpp"
#include "karo/sdk/error.hpp"
#include "karo/sdk/messages/robot_status.hpp"
#include "karo/sdk/topic.hpp"

namespace karo::sdk::detail {

std::vector<uint8_t> encode_handshake_cert(
  uint64_t request_id,
  const std::string& sdk_version,
  const std::string& client_id,
  const std::vector<uint8_t>& cert_der);

std::vector<uint8_t> encode_handshake_token(
  uint64_t request_id,
  const std::string& sdk_version,
  const std::string& client_id,
  const std::vector<uint8_t>& token);

std::vector<uint8_t> encode_ping(uint64_t request_id, uint64_t client_ts_ms);

std::vector<uint8_t> encode_subscribe(
  uint64_t request_id,
  const std::string& topic,
  double desired_hz,
  uint64_t since_seq,
  uint32_t history_limit);

std::vector<uint8_t> encode_unsubscribe(
  uint64_t request_id, const std::string& topic);

std::vector<uint8_t> encode_cmd_vel(
  uint64_t request_id,
  double linear_x, double linear_y, double angular_z,
  uint64_t sequence, uint64_t client_ts_ms);

std::vector<uint8_t> encode_emergency_stop(
  uint64_t request_id, bool engage, const std::string& reason);

enum class SubscribeRejectReason {
  None              = 0,
  TopicNotFound     = 1,
  CapabilityDenied  = 2,
  AlreadySubscribed = 3,
  InvalidHz         = 4,
  HistoryTooOld     = 10,
  ReplayNotSupported = 11,
};

struct DecodedResponse {
  uint64_t    request_id   = 0;
  bool        success      = false;
  ErrorCode   sdk_code     = ErrorCode::Ok;
  std::string message;
  uint64_t    server_ts_ms = 0;

  bool has_handshake = false;
  bool has_rpc       = false;
  bool has_subscribe = false;

  std::string protocol_version;
  std::string robot_sn;
  std::string robot_model;
  std::string session_token;
  Capabilities granted_capabilities;
  std::vector<TopicDescriptor> available_topics;

  uint64_t              rpc_id       = 0;
  ErrorCode             rpc_code     = ErrorCode::Ok;
  std::string           rpc_message;
  std::vector<uint8_t>  rpc_payload;

  std::string           sub_topic;
  bool                  sub_accepted     = false;
  SubscribeRejectReason sub_reject_reason = SubscribeRejectReason::None;
  uint64_t              sub_next_seq     = 0;
  std::string           sub_message;
};

bool decode_response(const uint8_t* data, size_t size, DecodedResponse& out);

struct DecodedTopicPush {
  std::string          topic;
  uint64_t             timestamp_ms = 0;
  uint64_t             seq          = 0;
  std::vector<uint8_t> payload;
};

bool decode_topic_push(const uint8_t* data, size_t size, DecodedTopicPush& out);

struct DecodedCmdVelResponse {
  bool     accepted             = false;
  uint64_t device_timestamp_ms  = 0;
};

bool decode_cmd_vel_response(
  const std::vector<uint8_t>& payload, DecodedCmdVelResponse& out);

struct DecodedPingResponse {
  int64_t     client_timestamp_ms = 0;
  int64_t     server_timestamp_ms = 0;
  std::string protocol_version;
  std::string device_sn;
};

bool decode_ping_response(
  const std::vector<uint8_t>& payload, DecodedPingResponse& out);

bool decode_robot_status(
  const std::vector<uint8_t>& payload, RobotStatus& out);

ErrorCode map_sdk_error_code(int proto_sdk_code) noexcept;

ErrorCode map_v1_error_code(int proto_v1_code) noexcept;

}

#endif

