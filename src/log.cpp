#include "log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace karo::sdk::detail {

namespace {
LogLevel parse_level(const std::string& raw) {
  std::string s;
  s.reserve(raw.size());
  for (char c : raw) s.push_back(static_cast<char>(std::tolower(c)));
  if (s == "trace") return LogLevel::Trace;
  if (s == "debug") return LogLevel::Debug;
  if (s == "info")  return LogLevel::Info;
  if (s == "warn" || s == "warning") return LogLevel::Warn;
  if (s == "error") return LogLevel::Error;
  if (s == "off" || s == "none") return LogLevel::Off;
  std::fprintf(stderr,
    "[karo-sdk] warning: unknown KARO_SDK_LOG_LEVEL=%s, defaulting to info\n",
    raw.c_str());
  return LogLevel::Info;
}
}

LogSink::LogSink()
  : min_level_(level_from_env()) {}

void LogSink::set_handler(LogHandler h) {
  std::lock_guard<std::mutex> lk(mu_);
  handler_ = std::move(h);
}

LogHandler LogSink::take_handler() const {
  std::lock_guard<std::mutex> lk(mu_);
  return handler_;
}

LogLevel LogSink::level_from_env() {
  const char* env = std::getenv("KARO_SDK_LOG_LEVEL");
  if (env == nullptr || env[0] == '\0') return LogLevel::Info;
  return parse_level(env);
}

void LogSink::set_min_level(LogLevel l) noexcept {
  min_level_ = l;
}

LogLevel LogSink::min_level() const noexcept {
  return min_level_;
}

void LogSink::default_stderr_write(LogLevel level, const std::string& msg) {

  std::lock_guard<std::mutex> lk(stderr_mu_);
  const char* lvl = "info";
  switch (level) {
    case LogLevel::Trace: lvl = "trace"; break;
    case LogLevel::Debug: lvl = "debug"; break;
    case LogLevel::Info:  lvl = "info";  break;
    case LogLevel::Warn:  lvl = "warn";  break;
    case LogLevel::Error: lvl = "error"; break;
    case LogLevel::Off:   return;
  }
  std::fprintf(stderr, "[karo-sdk %s] %s\n", lvl, msg.c_str());
}

std::string format_kv(const std::string& key, const std::string& value) {
  std::string out;
  out.reserve(key.size() + value.size() + 2);
  out.append(key).append("=").append(value);
  return out;
}

}

namespace karo::sdk {
const char* to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return "Trace";
    case LogLevel::Debug: return "Debug";
    case LogLevel::Info:  return "Info";
    case LogLevel::Warn:  return "Warn";
    case LogLevel::Error: return "Error";
    case LogLevel::Off:   return "Off";
  }
  return "Unknown";
}
}

