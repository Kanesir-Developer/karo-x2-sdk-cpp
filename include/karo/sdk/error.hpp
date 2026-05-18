#ifndef KARO_SDK_ERROR_HPP_
#define KARO_SDK_ERROR_HPP_

#include <stdexcept>
#include <string>

namespace karo::sdk {

enum class ErrorCode : int {
  Ok = 0,

  AuthFailed             = 1,
  AuthCnNotAllowed       = 2,
  AuthSerialRevoked      = 3,
  AuthTokenInvalid       = 4,
  AuthTokenExpired       = 5,
  SessionTokenInvalid    = 10,
  SessionTokenExpired    = 11,
  HandshakeRequired      = 12,
  CapabilityDenied       = 20,
  RateLimited            = 21,
  UnsupportedVersion     = 22,
  TopicNotFound          = 30,
  TopicAlreadySubscribed = 31,
  TopicNotSubscribed     = 32,

  ControlEStopActive             = 9300,
  ControlPublisherUnavailable    = 9301,
  ControlInvalidTwist            = 9302,

  TaskRunning                    = 9400,
  TaskNotFound                   = 9401,
  TaskInvalidState               = 9402,
  NavMarkerNotFound              = 9403,
  NavNoActiveMap                 = 9404,
  NavGoalUnreachable             = 9405,
  NavLowBattery                  = 9406,

  TransportFailure       = 10000,
  Timeout                = 10001,
  Cancelled              = 10002,

  Disconnected           = 10003,

  WouldDeadlock          = 10004,
  InternalError          = 10999,
};

const char* to_string(ErrorCode code);

enum class ErrorCategory {
  Ok = 0,
  Transient,
  Application,
  Fatal,
  Local,
};
ErrorCategory classify(ErrorCode code) noexcept;

class SdkException : public std::runtime_error {
 public:
  SdkException(ErrorCode code, const std::string& message);
  ErrorCode code() const noexcept { return code_; }
 private:
  ErrorCode code_;
};

}

#endif

