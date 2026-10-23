
#include "src/client/scheduler_client.hxx"
#include "src/ipc/server.hxx"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

class IpcServerClientTest : public ::testing::Test {
protected:
  std::filesystem::path directory_;
  std::string socket_path_;
  std::string lock_path_;

  void SetUp() override {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    directory_ =
        std::filesystem::temp_directory_path() /
        ("gravesentinel-ipc-" + std::to_string(::getpid()) + "-" + suffix);

    std::filesystem::create_directories(directory_);
    socket_path_ = (directory_ / "server.sock").string();
    lock_path_ = (directory_ / "server.lock").string();
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }
};

TEST_F(IpcServerClientTest, ClientReportsConnectionFailureForMissingSocket) {
  const auto missing_socket = (directory_ / "does-not-exist.sock").string();

  gravesentinel::ccm::SchedulerClient client(missing_socket, 100ms);

  const auto result = client.get_health();

  EXPECT_FALSE(result);
}

TEST_F(IpcServerClientTest, ServerRejectsSocketPathThatIsTooLong) {
  volatile std::sig_atomic_t stop_requested = 0;

  const std::string long_path(sizeof(sockaddr_un{}.sun_path), 'x');

  gravesentinel::ipc::ServerConfig config{long_path, lock_path_, 0600, true};

  const int result = gravesentinel::ipc::serve(
      config, [](const nlohmann::json &) { return nlohmann::json::object(); },
      &stop_requested);

  EXPECT_NE(result, 0);
}

} // namespace
