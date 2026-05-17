#ifndef KARO_SDK_MESSAGES_ROBOT_STATUS_HPP_
#define KARO_SDK_MESSAGES_ROBOT_STATUS_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace karo::sdk {

struct RobotStatus {

  std::string robot_id;
  uint64_t    sequence       = 0;
  uint64_t    timestamp_ms   = 0;

  uint32_t    health_score   = 0;
  uint32_t    battery_percent = 0;

  enum class ChargeType {
    None = 0,
    Wire = 1,
    Dock = 2,
  };
  ChargeType  charge_type    = ChargeType::None;

  enum class ServiceState {
    Idle             = 0,
    Task             = 1,
    Mapping          = 2,
    Starting         = 3,
    ShuttingDown     = 4,
    Upgrading        = 5,
    RemoteControl    = 6,
    GotoCharging     = 7,
  };
  ServiceState service_state = ServiceState::Idle;

  bool        is_estop       = false;
  bool        is_hw_estop    = false;
  bool        is_sw_estop    = false;

  std::string active_map_id;
  std::string active_map_name;
  int32_t     active_floor   = 0;
  std::string current_task_id;

  uint64_t    uptime_seconds   = 0;
  uint64_t    idle_seconds     = 0;
  uint64_t    charging_seconds = 0;

  std::vector<int32_t> error_codes;
};

}

#endif

