#include "karo/sdk/error.hpp"

namespace karo::sdk {

const char* to_string(ErrorCode code)
{
  switch (code) {
    case ErrorCode::Ok:                            return "Ok";

    case ErrorCode::AuthFailed:                    return "AuthFailed";
    case ErrorCode::AuthCnNotAllowed:              return "AuthCnNotAllowed";
    case ErrorCode::AuthSerialRevoked:             return "AuthSerialRevoked";
    case ErrorCode::AuthTokenInvalid:              return "AuthTokenInvalid";
    case ErrorCode::AuthTokenExpired:              return "AuthTokenExpired";
    case ErrorCode::SessionTokenInvalid:           return "SessionTokenInvalid";
    case ErrorCode::SessionTokenExpired:           return "SessionTokenExpired";
    case ErrorCode::HandshakeRequired:             return "HandshakeRequired";
    case ErrorCode::CapabilityDenied:              return "CapabilityDenied";
    case ErrorCode::RateLimited:                   return "RateLimited";
    case ErrorCode::UnsupportedVersion:            return "UnsupportedVersion";
    case ErrorCode::TopicNotFound:                 return "TopicNotFound";
    case ErrorCode::TopicAlreadySubscribed:        return "TopicAlreadySubscribed";
    case ErrorCode::TopicNotSubscribed:            return "TopicNotSubscribed";

    case ErrorCode::ControlEStopActive:            return "ControlEStopActive";
    case ErrorCode::ControlPublisherUnavailable:   return "ControlPublisherUnavailable";
    case ErrorCode::ControlInvalidTwist:           return "ControlInvalidTwist";

    case ErrorCode::TaskRunning:                   return "TaskRunning";
    case ErrorCode::TaskNotFound:                  return "TaskNotFound";
    case ErrorCode::TaskInvalidState:              return "TaskInvalidState";
    case ErrorCode::NavMarkerNotFound:             return "NavMarkerNotFound";
    case ErrorCode::NavNoActiveMap:                return "NavNoActiveMap";
    case ErrorCode::NavGoalUnreachable:            return "NavGoalUnreachable";
    case ErrorCode::NavLowBattery:                 return "NavLowBattery";

    case ErrorCode::TransportFailure:              return "TransportFailure";
    case ErrorCode::Timeout:                       return "Timeout";
    case ErrorCode::Cancelled:                     return "Cancelled";
    case ErrorCode::Disconnected:                  return "Disconnected";
    case ErrorCode::WouldDeadlock:                 return "WouldDeadlock";
    case ErrorCode::InternalError:                 return "InternalError";
  }
  return "Unknown";
}

ErrorCategory classify(ErrorCode code) noexcept
{
  switch (code) {
    case ErrorCode::Ok:
      return ErrorCategory::Ok;

    case ErrorCode::AuthFailed:
    case ErrorCode::AuthCnNotAllowed:
    case ErrorCode::AuthSerialRevoked:
    case ErrorCode::AuthTokenInvalid:
    case ErrorCode::AuthTokenExpired:
    case ErrorCode::UnsupportedVersion:
      return ErrorCategory::Fatal;

    case ErrorCode::SessionTokenInvalid:
    case ErrorCode::SessionTokenExpired:
    case ErrorCode::HandshakeRequired:
    case ErrorCode::TransportFailure:

    case ErrorCode::Timeout:
      return ErrorCategory::Transient;

    case ErrorCode::CapabilityDenied:
    case ErrorCode::RateLimited:
    case ErrorCode::TopicNotFound:
    case ErrorCode::TopicAlreadySubscribed:
    case ErrorCode::TopicNotSubscribed:
    case ErrorCode::ControlEStopActive:
    case ErrorCode::ControlPublisherUnavailable:
    case ErrorCode::ControlInvalidTwist:
    case ErrorCode::TaskRunning:
    case ErrorCode::TaskNotFound:
    case ErrorCode::TaskInvalidState:
    case ErrorCode::NavMarkerNotFound:
    case ErrorCode::NavNoActiveMap:
    case ErrorCode::NavGoalUnreachable:
    case ErrorCode::NavLowBattery:
      return ErrorCategory::Application;

    case ErrorCode::Cancelled:
    case ErrorCode::Disconnected:
    case ErrorCode::WouldDeadlock:
    case ErrorCode::InternalError:
      return ErrorCategory::Local;
  }
  return ErrorCategory::Local;
}

SdkException::SdkException(ErrorCode code, const std::string& message)
  : std::runtime_error(message), code_(code) {}

}

