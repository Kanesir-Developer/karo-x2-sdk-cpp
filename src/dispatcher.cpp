#include "dispatcher.hpp"

#include <utility>

namespace karo::sdk::detail {

Dispatcher::Dispatcher()
  : worker_([this] { WorkerLoop(); })
{
  worker_id_ = worker_.get_id();
}

Dispatcher::~Dispatcher() {
  Shutdown();
}

void Dispatcher::Post(Task t) {
  if (!t) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return;
    queue_.push_back(std::move(t));
  }
  cv_.notify_one();
}

void Dispatcher::Shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load()) return;
    running_.store(false);
  }
  cv_.notify_all();
  if (worker_.joinable()) {

    if (std::this_thread::get_id() != worker_id_) {
      worker_.join();
    } else {
      worker_.detach();
    }
  }
}

bool Dispatcher::IsInDispatcherThread() const noexcept {
  return std::this_thread::get_id() == worker_id_;
}

void Dispatcher::WorkerLoop() {
  while (true) {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
      if (!running_.load() && queue_.empty()) return;
      if (queue_.empty()) continue;
      task = std::move(queue_.front());
      queue_.pop_front();
    }

    try { task(); } catch (...) { }
  }
}

}

