#ifndef KARO_SDK_MESSAGES_TASK_EVENT_HPP_
#define KARO_SDK_MESSAGES_TASK_EVENT_HPP_

#include <cstdint>
#include <string>

#include "karo/sdk/error.hpp"

namespace karo::sdk {

enum class TaskType {
  Unspecified = 0,
  Navigation  = 1,
  Patrol      = 2,
  GoBack      = 3,
};

enum class TaskState {
  Unspecified = 0,
  Pending     = 1,
  Running     = 2,
  Paused      = 3,
  Cancelling  = 4,
  Succeeded   = 10,
  Failed      = 11,
  Cancelled   = 12,
};

enum class TaskCancelReason {
  Unspecified  = 0,
  User         = 1,
  Estop        = 2,
  Timeout      = 3,
  PauseTimeout = 4,
  ResumeFailed = 5,
  Error        = 6,
};

enum class TaskPauseReason {
  Unspecified = 0,
  User        = 1,
  Estop       = 2,
};

struct TaskEvent {
  std::string      task_id;
  TaskType         task_type     = TaskType::Unspecified;
  TaskState        state         = TaskState::Unspecified;
  TaskCancelReason cancel_reason = TaskCancelReason::Unspecified;
  TaskPauseReason  pause_reason  = TaskPauseReason::Unspecified;
  ErrorCode        error_code    = ErrorCode::Ok;
  std::string      error_message;
  std::string      marker_id;
  double           planned_distance  = 0.0;
  double           traveled_distance = 0.0;
  uint64_t         created_at_ms   = 0;
  uint64_t         started_at_ms   = 0;
  uint64_t         completed_at_ms = 0;
  uint64_t         timestamp_ms    = 0;
};

}  // namespace karo::sdk

#endif  // KARO_SDK_MESSAGES_TASK_EVENT_HPP_
