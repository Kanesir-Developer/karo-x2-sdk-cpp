#ifndef KARO_SDK_RATE_LIMITER_HPP_
#define KARO_SDK_RATE_LIMITER_HPP_

#include <chrono>
#include <mutex>

namespace karo::sdk::detail {

class TokenBucket {
 public:

  explicit TokenBucket(double max_hz, double burst = 1.0);

  bool try_acquire();

  template <typename Clock>
  bool try_acquire_at(typename Clock::time_point now);

 private:
  bool try_acquire_locked(std::chrono::steady_clock::time_point now);

  const double max_hz_;
  const double burst_;
  std::mutex   mu_;
  double       tokens_;
  std::chrono::steady_clock::time_point last_;
};

class Watchdog {
 public:
  Watchdog(std::chrono::milliseconds timeout);

  void feed();

  bool check();

  template <typename Clock>
  bool check_at(typename Clock::time_point now);

  template <typename Clock>
  void feed_at(typename Clock::time_point now);

 private:
  bool check_locked(std::chrono::steady_clock::time_point now);

  const std::chrono::milliseconds timeout_;
  std::mutex mu_;
  std::chrono::steady_clock::time_point last_feed_;
  bool fired_ = false;
};

}

#endif

