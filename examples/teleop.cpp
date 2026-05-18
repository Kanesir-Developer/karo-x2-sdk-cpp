#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <karo/sdk/robot.hpp>

namespace {

std::atomic<bool> g_shutdown{false};
void on_signal(int ) { g_shutdown.store(true); }

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
  const std::string host     = argv[1];
  const std::string cert_dir = argv[2];

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  karo::sdk::ConnectOptions opts;
  opts.host = host;
  opts.cert.cert_pem = read_file(cert_dir + "/cert.pem");
  opts.cert.key_pem  = read_file(cert_dir + "/key.pem");
  opts.cert.ca_pem   = read_file(cert_dir + "/ca.pem");
  opts.cert.insecure_skip_verify = true;
  opts.client_id     = "teleop-example/2.0";

  std::mutex mu;
  std::condition_variable cv;
  bool ready_seen = false;
  bool fatal_seen = false;
  karo::sdk::ErrorCode last_err = karo::sdk::ErrorCode::Ok;
  std::string last_err_msg;

  std::unique_ptr<karo::sdk::Robot> robot;
  try {
    robot = std::make_unique<karo::sdk::Robot>(opts);
  } catch (const karo::sdk::SdkException& e) {
    std::cerr << "invalid opts: " << e.what() << "\n";
    return 1;
  }

  robot->SetConnectionStateHandler(
    [&](karo::sdk::ConnectionState old_s, karo::sdk::ConnectionState new_s,
        const karo::sdk::StateChangeInfo& info) {
      std::cerr << "[state] " << karo::sdk::to_string(old_s) << " -> "
                << karo::sdk::to_string(new_s);
      if (!info.reason.empty()) std::cerr << " (" << info.reason << ")";
      if (info.last_error_code != karo::sdk::ErrorCode::Ok) {
        std::cerr << " err=" << karo::sdk::to_string(info.last_error_code);
      }
      std::cerr << "\n";
      std::unique_lock<std::mutex> lk(mu);
      if (new_s == karo::sdk::ConnectionState::Ready && !ready_seen) {
        ready_seen = true;
        cv.notify_all();
      } else if (new_s == karo::sdk::ConnectionState::Fatal) {
        fatal_seen = true;
        last_err = info.last_error_code;
        last_err_msg = info.last_error_message;
        cv.notify_all();
      }
    });

  robot->SetLogHandler([](karo::sdk::LogLevel level, const std::string& msg) {
    if (level >= karo::sdk::LogLevel::Warn) {
      std::cerr << "[sdk-" << karo::sdk::to_string(level) << "] " << msg << "\n";
    }
  });

  robot->Connect();

  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::seconds(30),
      [&] { return ready_seen || fatal_seen; });
    if (!ready_seen) {
      std::cerr << "failed to reach Ready: "
                << (fatal_seen ? karo::sdk::to_string(last_err) : "timeout") << " "
                << last_err_msg << "\n";
      return 2;
    }
  }

  auto info = robot->info();
  if (!info) {
    std::cerr << "info() unavailable\n";
    return 2;
  }
  std::cout << "connected: sn=" << info->sn << " model=" << info->model
            << " proto=" << info->protocol_version << "\n";
  if (!info->granted_capabilities.chassis_control) {
    std::cerr << "ERROR: application lacks chassis_control capability\n";
    return 2;
  }

  const auto interval = std::chrono::milliseconds(100);
  size_t total = 0, accepted = 0, disconnected = 0;
  auto next_tick = std::chrono::steady_clock::now();

  while (!g_shutdown.load()) {
    auto r = robot->CmdVel(0.2, 0.0, 0.0);
    ++total;
    if (r.accepted) {
      ++accepted;
    } else if (r.code == karo::sdk::ErrorCode::Disconnected) {

      ++disconnected;
    } else {
      std::cerr << "cmd_vel rejected: " << r.message
                << " (code=" << karo::sdk::to_string(r.code) << ")\n";
    }
    if (total % 10 == 0) {
      std::cout << "tick " << total << " accepted=" << accepted
                << " disc=" << disconnected
                << " rtt~" << r.rtt.count() << "ms\n";
    }
    next_tick += interval;
    std::this_thread::sleep_until(next_tick);
  }

  robot->CmdVel(0.0, 0.0, 0.0);
  std::cout << "shutdown: " << accepted << "/" << total
            << " accepted, " << disconnected << " disconnected\n";
  return 0;
}

