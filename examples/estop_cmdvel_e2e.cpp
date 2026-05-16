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
  opts.host                      = argv[1];
  opts.cert.cert_pem             = read_file(std::string(argv[2]) + "/cert.pem");
  opts.cert.key_pem              = read_file(std::string(argv[2]) + "/key.pem");
  opts.cert.ca_pem               = read_file(std::string(argv[2]) + "/ca.pem");
  opts.cert.insecure_skip_verify = true;
  opts.client_id                 = "estop-cmdvel-e2e/2.0";

  std::mutex mu; std::condition_variable cv;
  bool ready = false; bool fatal = false;

  karo::sdk::Robot robot(opts);
  robot.SetConnectionStateHandler(
    [&](karo::sdk::ConnectionState old_s, karo::sdk::ConnectionState new_s,
        const karo::sdk::StateChangeInfo&) {
      std::cerr << "[state] " << karo::sdk::to_string(old_s) << " -> "
                << karo::sdk::to_string(new_s) << "\n";
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

  auto info = robot.info();
  std::cout << "connected: sn=" << info->sn
            << " caps[chassis_control=" << info->granted_capabilities.chassis_control
            << " telemetry_read="     << info->granted_capabilities.telemetry_read << "]\n";

  std::cout << "\n[1] EmergencyStop(engage=true) ..." << std::flush;
  auto r1 = robot.EmergencyStop(true, "e2e test trigger");
  std::cout << " ok=" << r1.ok
            << " code=" << karo::sdk::to_string(r1.code)
            << " msg='" << r1.message << "'\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::cout << "\n[2] CmdVel(0.1, 0, 0) during estop ..." << std::flush;
  auto r2 = robot.CmdVel(0.1, 0.0, 0.0);
  std::cout << " accepted=" << r2.accepted
            << " code=" << karo::sdk::to_string(r2.code)
            << " msg='" << r2.message << "' rtt=" << r2.rtt.count() << "ms\n";
  bool step2_ok = !r2.accepted &&
    (r2.code == karo::sdk::ErrorCode::ControlEStopActive ||
     r2.code != karo::sdk::ErrorCode::Ok);
  std::cout << "    => " << (step2_ok ? "PASS" : "FAIL") << "\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::cout << "\n[3] EmergencyStop(engage=false) ..." << std::flush;
  auto r3 = robot.EmergencyStop(false, "e2e test release");
  std::cout << " ok=" << r3.ok
            << " code=" << karo::sdk::to_string(r3.code) << "\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  std::cout << "\n[4] CmdVel(0, 0, 0) after release ..." << std::flush;
  auto r4 = robot.CmdVel(0.0, 0.0, 0.0);
  std::cout << " accepted=" << r4.accepted
            << " code=" << karo::sdk::to_string(r4.code)
            << " rtt=" << r4.rtt.count() << "ms\n";
  bool step4_ok = r4.accepted && r4.code == karo::sdk::ErrorCode::Ok;
  std::cout << "    => " << (step4_ok ? "PASS" : "FAIL") << "\n";

  std::cout << "\n=== summary ===\n"
            << "  step 1 estop engage:   " << (r1.ok ? "OK" : "FAIL") << "\n"
            << "  step 2 cmd_vel reject: " << (step2_ok ? "OK" : "FAIL") << "\n"
            << "  step 3 estop release:  " << (r3.ok ? "OK" : "FAIL") << "\n"
            << "  step 4 cmd_vel accept: " << (step4_ok ? "OK" : "FAIL") << "\n";

  return (r1.ok && step2_ok && r3.ok && step4_ok) ? 0 : 2;
}

