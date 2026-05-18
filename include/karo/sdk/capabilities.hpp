#ifndef KARO_SDK_CAPABILITIES_HPP_
#define KARO_SDK_CAPABILITIES_HPP_

namespace karo::sdk {

struct Capabilities {
  bool chassis_control = false;
  bool telemetry_read  = false;
  bool image_stream    = false;
  bool map_read        = false;
  bool map_write       = false;
  bool task_control    = false;
};

}

#endif

