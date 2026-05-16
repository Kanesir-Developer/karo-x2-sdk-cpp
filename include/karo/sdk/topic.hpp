#ifndef KARO_SDK_TOPIC_HPP_
#define KARO_SDK_TOPIC_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace karo::sdk {

enum class DeliverySemantics {

  Telemetry = 0,

  Event = 1,
};

struct TopicDescriptor {
  std::string name;
  std::string description;
  float default_hz = 0.0f;
  float max_hz     = 0.0f;
  DeliverySemantics delivery_semantics = DeliverySemantics::Telemetry;
};

struct TopicMessage {
  std::string topic;
  uint64_t timestamp_ms = 0;
  uint64_t seq          = 0;
  std::vector<uint8_t> payload;
};

}

#endif

