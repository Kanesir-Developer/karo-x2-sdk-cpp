#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <karo/sdk/robot.hpp>

namespace {
std::string read_file(const std::string& path) {
  std::ifstream ifs(path);
  std::stringstream ss; ss << ifs.rdbuf();
  return ss.str();
}
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <robot_host> <cert_dir>\n";
    return 1;
  }

  karo::sdk::ConnectOptions opts;
  opts.host          = argv[1];
  opts.cert.cert_pem = read_file(std::string(argv[2]) + "/cert.pem");
  opts.cert.key_pem  = read_file(std::string(argv[2]) + "/key.pem");
  opts.cert.ca_pem   = read_file(std::string(argv[2]) + "/ca.pem");
  opts.cert.insecure_skip_verify = true;
  opts.client_id     = "estop-example/2.0";

  std::mutex mu; std::condition_variable cv;
  bool ready = false; bool fatal = false;

  karo::sdk::Robot robot(opts);
  robot.SetConnectionStateHandler(
    [&](karo::sdk::ConnectionState old_s, karo::sdk::ConnectionState new_s,
        const karo::sdk::StateChangeInfo& info) {
      std::cerr << "[state] " << karo::sdk::to_string(old_s) << " -> "
                << karo::sdk::to_string(new_s);
      if (!info.last_action.empty()) std::cerr << " action=" << info.last_action;
      std::cerr << "\n";
      std::lock_guard<std::mutex> lk(mu);
      if (new_s == karo::sdk::ConnectionState::Ready) ready = true;
      if (new_s == karo::sdk::ConnectionState::Fatal) fatal = true;
      cv.notify_all();
    });
  robot.Connect();

  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::seconds(30), [&] { return ready || fatal; });
    if (!ready) { std::cerr << "not ready\n"; return 2; }
  }

  std::cout << "engaging soft e-stop...\n";
  auto r1 = robot.EmergencyStop(true, "demo: triggering soft estop");
  if (!r1.ok) {
    std::cerr << "engage failed: " << r1.message
              << " code=" << karo::sdk::to_string(r1.code) << "\n";
    return 3;
  }
  std::cout << "estop engaged. cmd_vel will be rejected for 5s...\n";
  std::this_thread::sleep_for(std::chrono::seconds(5));

  std::cout << "releasing soft e-stop...\n";
  auto r2 = robot.EmergencyStop(false, "demo end");
  if (!r2.ok) {
    std::cerr << "release failed: " << r2.message
              << " code=" << karo::sdk::to_string(r2.code) << "\n";
    return 4;
  }
  std::cout << "estop released.\n";
  return 0;
}

