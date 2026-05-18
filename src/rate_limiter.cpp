#include "rate_limiter.hpp"

#include <algorithm>

namespace karo::sdk::detail {

TokenBucket::TokenBucket(double max_hz, double burst)
  : max_hz_(max_hz),
    burst_(burst),
    tokens_(burst),
    last_(std::chrono::steady_clock::now())
{}

bool TokenBucket::try_acquire()
{
  return try_acquire_at<std::chrono::steady_clock>(
    std::chrono::steady_clock::now());
}

template <typename Clock>
bool TokenBucket::try_acquire_at(typename Clock::time_point now)
{
  std::lock_guard<std::mutex> lock(mu_);
  return try_acquire_locked(now);
}

bool TokenBucket::try_acquire_locked(std::chrono::steady_clock::time_point now)
{

  const auto elapsed = std::chrono::duration<double>(now - last_).count();
  tokens_ = std::min(burst_, tokens_ + elapsed * max_hz_);
  last_   = now;

  constexpr double kEpsilon = 1e-9;
  if (tokens_ >= 1.0 - kEpsilon) {
    tokens_ = std::max(0.0, tokens_ - 1.0);
    return true;
  }
  return false;
}

template bool TokenBucket::try_acquire_at<std::chrono::steady_clock>(
  std::chrono::steady_clock::time_point);

Watchdog::Watchdog(std::chrono::milliseconds timeout)
  : timeout_(timeout),
    last_feed_(std::chrono::steady_clock::now())
{}

void Watchdog::feed()
{
  feed_at<std::chrono::steady_clock>(std::chrono::steady_clock::now());
}

template <typename Clock>
void Watchdog::feed_at(typename Clock::time_point now)
{
  std::lock_guard<std::mutex> lock(mu_);
  last_feed_ = now;
  fired_     = false;
}

bool Watchdog::check()
{
  return check_at<std::chrono::steady_clock>(std::chrono::steady_clock::now());
}

template <typename Clock>
bool Watchdog::check_at(typename Clock::time_point now)
{
  std::lock_guard<std::mutex> lock(mu_);
  return check_locked(now);
}

bool Watchdog::check_locked(std::chrono::steady_clock::time_point now)
{
  if (fired_) return false;
  if (now - last_feed_ >= timeout_) {
    fired_ = true;
    return true;
  }
  return false;
}

template void Watchdog::feed_at<std::chrono::steady_clock>(
  std::chrono::steady_clock::time_point);
template bool Watchdog::check_at<std::chrono::steady_clock>(
  std::chrono::steady_clock::time_point);

}

