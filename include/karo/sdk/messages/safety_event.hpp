#ifndef KARO_SDK_MESSAGES_SAFETY_EVENT_HPP_
#define KARO_SDK_MESSAGES_SAFETY_EVENT_HPP_

#include <cstdint>
#include <string>

namespace karo::sdk {

struct SafetyEvent {
  uint32_t    code     = 0;
  uint32_t    level    = 0;
  uint32_t    severity = 0;
  std::string mode;
  std::string active_source;
  uint64_t    timestamp_ms = 0;
};

}  // namespace karo::sdk

#endif  // KARO_SDK_MESSAGES_SAFETY_EVENT_HPP_
