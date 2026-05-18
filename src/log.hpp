#ifndef KARO_SDK_INTERNAL_LOG_HPP_
#define KARO_SDK_INTERNAL_LOG_HPP_

#include <mutex>
#include <string>

#include "karo/sdk/diagnostics.hpp"

namespace karo::sdk::detail {

class LogSink {
 public:
  LogSink();

  void set_handler(LogHandler h);
  LogHandler take_handler() const;

  static LogLevel level_from_env();

  void set_min_level(LogLevel l) noexcept;
  LogLevel min_level() const noexcept;

  void default_stderr_write(LogLevel level, const std::string& msg);

 private:
  mutable std::mutex mu_;
  LogHandler handler_;
  LogLevel min_level_;
  std::mutex stderr_mu_;
};

std::string format_kv(const std::string& key, const std::string& value);

}

#endif

