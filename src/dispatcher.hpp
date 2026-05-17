#ifndef KARO_SDK_INTERNAL_DISPATCHER_HPP_
#define KARO_SDK_INTERNAL_DISPATCHER_HPP_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace karo::sdk::detail {

class Dispatcher {
 public:
  using Task = std::function<void()>;

  Dispatcher();
  ~Dispatcher();

  Dispatcher(const Dispatcher&)            = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  void Post(Task t);

  void Shutdown();

  bool IsInDispatcherThread() const noexcept;

 private:
  void WorkerLoop();

  std::mutex              mu_;
  std::condition_variable cv_;
  std::deque<Task>        queue_;
  std::atomic<bool>       running_{true};
  std::thread             worker_;
  std::thread::id         worker_id_;
};

}

#endif

