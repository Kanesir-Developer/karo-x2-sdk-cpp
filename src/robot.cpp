#include "karo/sdk/robot.hpp"

#include "robot_impl.hpp"

namespace karo::sdk {

struct Robot::Impl {
  std::shared_ptr<detail::RobotImpl> rimpl;
};

const char* sdk_version() {

  return "3.2.16";
}

Robot::Robot(const ConnectOptions& opts)
  : impl_(std::make_unique<Impl>()) {
  impl_->rimpl = detail::RobotImpl::Create(opts);
}

Robot::~Robot() {
  if (impl_ && impl_->rimpl) {
    impl_->rimpl->Close();
  }
}

void Robot::SetConnectionStateHandler(ConnectionStateHandler h) {
  impl_->rimpl->SetConnectionStateHandler(std::move(h));
}

void Robot::SetLogHandler(LogHandler h) {
  impl_->rimpl->SetLogHandler(std::move(h));
}

void Robot::Connect() {
  impl_->rimpl->Connect();
}

void Robot::Close() {
  impl_->rimpl->Close();
}

ConnectionState Robot::state() const noexcept {
  return impl_->rimpl->state();
}

bool Robot::IsConnected() const noexcept {
  return impl_->rimpl->IsConnected();
}

std::optional<RobotInfo> Robot::info() const {
  return impl_->rimpl->info();
}

std::unique_ptr<Subscription> Robot::SubscribeRobotStatus(
  double desired_hz,
  RobotStatusCallback on_data,
  StatusCallback on_status) {
  return impl_->rimpl->SubscribeRobotStatus(
    desired_hz, std::move(on_data), std::move(on_status));
}

std::unique_ptr<Subscription> Robot::Subscribe(
  const std::string& topic,
  double desired_hz,
  TopicDataCallback on_data,
  StatusCallback on_status) {
  return impl_->rimpl->SubscribeGeneric(
    topic, desired_hz, std::move(on_data), std::move(on_status));
}

CmdVelResult Robot::CmdVel(double linear_x, double linear_y, double angular_z) {
  return impl_->rimpl->CmdVel(linear_x, linear_y, angular_z);
}

EmergencyStopResult Robot::EmergencyStop(bool engage, const std::string& reason) {
  return impl_->rimpl->EmergencyStopRpc(engage, reason);
}

std::chrono::milliseconds Robot::Ping() {
  return impl_->rimpl->Ping();
}

Diagnostics Robot::GetDiagnostics() const {
  return impl_->rimpl->GetDiagnostics();
}

}

