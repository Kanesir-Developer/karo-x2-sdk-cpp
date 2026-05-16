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

const char* service_state_name(karo::sdk::RobotStatus::ServiceState s) {
  using S = karo::sdk::RobotStatus::ServiceState;
  switch (s) {
    case S::Idle:           return "Idle";
    case S::Task:           return "Task";
    case S::Mapping:        return "Mapping";
    case S::Starting:       return "Starting";
    case S::ShuttingDown:   return "ShuttingDown";
    case S::Upgrading:      return "Upgrading";
    case S::RemoteControl:  return "RemoteControl";
    case S::GotoCharging:   return "GotoCharging";
  }
  return "Unknown";
}
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <robot_host> <cert_dir>\n";
    return 1;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  karo::sdk::ConnectOptions opts;
  opts.host          = argv[1];
  opts.cert.cert_pem = read_file(std::string(argv[2]) + "/cert.pem");
  opts.cert.key_pem  = read_file(std::string(argv[2]) + "/key.pem");
  opts.cert.ca_pem   = read_file(std::string(argv[2]) + "/ca.pem");
  opts.cert.insecure_skip_verify = true;
  opts.client_id     = "subscribe-status-example/2.0";

  std::mutex mu; std::condition_variable cv;
  bool ready = false; bool fatal = false;

  karo::sdk::Robot robot(opts);
  robot.SetConnectionStateHandler(
    [&](karo::sdk::ConnectionState old_s, karo::sdk::ConnectionState new_s,
        const karo::sdk::StateChangeInfo& info) {
      std::cerr << "[state] " << karo::sdk::to_string(old_s) << " -> "
                << karo::sdk::to_string(new_s) << "\n";
      std::lock_guard<std::mutex> lk(mu);
      if (new_s == karo::sdk::ConnectionState::Ready) ready = true;
      if (new_s == karo::sdk::ConnectionState::Fatal) fatal = true;
      cv.notify_all();
      (void)info;
    });
  robot.Connect();

  {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::seconds(30), [&] { return ready || fatal; });
    if (!ready) { std::cerr << "not ready\n"; return 2; }
  }

  auto info = robot.info();
  std::cout << "connected: sn=" << info->sn << " model=" << info->model << "\n"
            << "available topics:\n";
  for (const auto& t : info->available_topics) {
    std::cout << "  - " << t.name
              << " (default=" << t.default_hz << "Hz max=" << t.max_hz << "Hz, "
              << (t.delivery_semantics == karo::sdk::DeliverySemantics::Event ? "EVENT" : "TELEMETRY")
              << "): " << t.description << "\n";
  }
  if (!info->granted_capabilities.telemetry_read) {
    std::cerr << "ERROR: application lacks telemetry_read capability\n";
    return 2;
  }

  auto sub = robot.SubscribeRobotStatus(
    5.0,
    [](const karo::sdk::RobotStatus& s) {
      std::cout << "[" << s.timestamp_ms << "]"
                << " battery=" << s.battery_percent << "%"
                << " state=" << service_state_name(s.service_state)
                << " estop=" << (s.is_estop ? "Y" : "N")
                << " (hw=" << (s.is_hw_estop ? "Y" : "N")
                << " sw=" << (s.is_sw_estop ? "Y" : "N") << ")"
                << " errors=" << s.error_codes.size()
                << "\n";
    },
    [](const karo::sdk::StreamStatus& st) {
      std::cerr << "[stream] " << karo::sdk::to_string(st.kind);
      if (st.gap_count) std::cerr << " gap=" << *st.gap_count;
      if (st.error_code != karo::sdk::ErrorCode::Ok)
        std::cerr << " err=" << karo::sdk::to_string(st.error_code);
      if (!st.reason.empty()) std::cerr << " (" << st.reason << ")";
      std::cerr << "\n";
    });

  while (!g_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  auto err = sub->Unsubscribe();
  if (err != karo::sdk::ErrorCode::Ok) {
    std::cerr << "unsubscribe: " << karo::sdk::to_string(err) << "\n";
  }
  return 0;
}

