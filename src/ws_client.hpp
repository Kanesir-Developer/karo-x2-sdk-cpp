#ifndef KARO_SDK_WS_CLIENT_HPP_
#define KARO_SDK_WS_CLIENT_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "karo/sdk/error.hpp"

namespace karo::sdk::detail {

class WsClient {
 public:
  struct Config {
    std::string endpoint;
    std::string path = "/";

    std::string cert_pem;
    std::string key_pem;
    std::string ca_pem;

    bool insecure_skip_verify = false;

    std::chrono::milliseconds connect_timeout = std::chrono::seconds(5);
    std::chrono::milliseconds write_timeout   = std::chrono::seconds(2);
  };

  using MessageHandler = std::function<void(std::vector<uint8_t>)>;

  using CloseHandler   = std::function<void(ErrorCode reason, std::string message)>;

  explicit WsClient(Config cfg);
  ~WsClient();

  WsClient(const WsClient&)            = delete;
  WsClient& operator=(const WsClient&) = delete;

  void SetMessageHandler(MessageHandler cb);
  void SetCloseHandler(CloseHandler cb);

  void Connect();

  void Send(std::vector<uint8_t> bytes);

  bool Post(std::vector<uint8_t> bytes) noexcept;

  void Close();

  bool IsOpen() const noexcept;

 private:

  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}

#endif

