#include "src/client/scheduler_client.hxx"
#include "src/ipc/codec.hxx"
#include "src/ipc/server.hxx"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using gravesentinel::ccm::ContainerId;
using gravesentinel::ccm::ContainerRegistration;
using gravesentinel::ccm::RequestId;
using gravesentinel::ccm::RuntimeContainerRef;

[[nodiscard]] nlohmann::json handshake_result() {
  return {{"protocol_version", gravesentinel::ipc::protocol_version},
          {"service", "gravesentinel_hypervisor"}};
}

[[nodiscard]] nlohmann::json health_result() {
  return {{"status", "healthy"},
          {"queue", "available"},
          {"protocol_version", gravesentinel::ipc::protocol_version}};
}

[[nodiscard]] int connect_raw(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

[[nodiscard]] ContainerRegistration complete_registration() {
  gravesentinel::ccm::ColdStartSpec cold_start{
      .image_reference = "registry.example/worker:latest",
      .argv = {"worker", "", "--threads", "2"},
      .environment = {{.name = "WORKER_MODE", .value = "steady"},
                      {.name = "_EMPTY_VALUE", .value = ""}},
      .mounts = {{.source = "/srv/input",
                  .target = "/input",
                  .read_only = true}},
      .runtime_options = {"--log-level=warning"},
  };
  gravesentinel::ccm::RuntimeFingerprint fingerprint{
      .runtime_name = "podman",
      .runtime_version = "5.4.2",
      .runtime_build_origin = "Debian 13",
      .architecture = "amd64",
      .kernel_release = "6.12.0",
      .criu_version = "4.1.1",
      .oci_runtime_name = "crun",
      .oci_runtime_version = "1.21",
      .oci_runtime_commit = "abcdef",
      .oci_runtime_features = "+CRIU +SECCOMP",
      .cgroup_version = "v2",
      .cgroup_manager = "systemd",
      .cgroup_controllers = "cpu memory pids",
      .privilege_mode = "rootful",
      .user_namespace_mode = "host",
      .capability_profile = "default",
      .selinux_mode = "disabled",
      .apparmor_profile = "host-default",
      .seccomp_profile = "default",
      .command_profile = "podman-criu-rootful-v1",
  };
  return ContainerRegistration{
      .container_id = ContainerId::create("container-1").value(),
      .runtime_container = RuntimeContainerRef::create("runtime-1").value(),
      .checkpoint = {.interval = 60s,
                     .keep_last = 3,
                     .container_cpu = 0,
                     .checkpoint_cpu = 1},
      .cold_start = std::move(cold_start),
      .runtime_fingerprint = std::move(fingerprint),
  };
}

class IpcServerClientTest : public ::testing::Test {
protected:
  std::filesystem::path directory_;
  std::string socket_path_;
  std::string lock_path_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<int> server_result_{-1};
  std::optional<gravesentinel::ipc::ServerLock> server_lock_;
  std::thread server_thread_;

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
    stop_server();
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  void
  start_server(gravesentinel::ipc::Handler handler,
               const std::function<void(gravesentinel::ipc::ServerConfig &)>
                   &configure = {}) {
    stop_requested_.store(false);
    gravesentinel::ipc::ServerConfig config;
    config.socket_path = socket_path_;
    config.lock_path = lock_path_;
    config.mode = 0600;
    config.peer_io_timeout = 500ms;
    if (configure)
      configure(config);
    server_lock_ = gravesentinel::ipc::ServerLock::acquire(config.lock_path);
    ASSERT_TRUE(server_lock_);
    server_thread_ =
        std::thread([this, config, handler = std::move(handler)]() mutable {
          server_result_.store(
              gravesentinel::ipc::serve(config, *server_lock_, handler, [this] {
                return stop_requested_.load();
              }));
        });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
      const int probe = connect_raw(socket_path_);
      if (probe >= 0) {
        ::close(probe);
        ready = true;
        break;
      }
      if (server_result_.load() != -1)
        break;
      std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(ready) << "server failed to become ready, result="
                       << server_result_.load();
  }

  void stop_server() {
    if (server_thread_.joinable()) {
      stop_requested_.store(true);
      server_thread_.join();
      EXPECT_EQ(server_result_.load(), 0);
    }
    server_lock_.reset();
  }
};

TEST_F(IpcServerClientTest, ClientReportsConnectionFailureForMissingSocket) {
  const auto missing_socket = (directory_ / "does-not-exist.sock").string();
  gravesentinel::ccm::SchedulerClient client(missing_socket, 100ms);

  const auto result = client.get_health();

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code,
            gravesentinel::ccm::RpcFailureCode::unavailable);
}

TEST_F(IpcServerClientTest, ServerRejectsSocketPathThatIsTooLong) {
  const std::string long_path(sizeof(sockaddr_un{}.sun_path), 'x');
  gravesentinel::ipc::ServerConfig config;
  config.socket_path = long_path;
  config.lock_path = lock_path_;
  config.mode = 0600;

  auto lock = gravesentinel::ipc::ServerLock::acquire(lock_path_);
  ASSERT_TRUE(lock);
  const int result = gravesentinel::ipc::serve(
      config, *lock,
      [](const nlohmann::json &) { return nlohmann::json::object(); },
      [] { return false; });

  EXPECT_NE(result, 0);
}

TEST_F(IpcServerClientTest, HandshakeAndHealthUseOnePersistentConnection) {
  std::atomic<int> handshakes{0};
  std::atomic<int> health_calls{0};
  start_server([&](const nlohmann::json &request) {
    const auto method = request.at("method").get<std::string>();
    if (method == "Handshake") {
      ++handshakes;
      return handshake_result();
    }
    if (method == "GetHealth") {
      ++health_calls;
      return health_result();
    }
    return nlohmann::json{{"ok", false}, {"error", "unexpected method"}};
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  ASSERT_TRUE(client.connect());
  const auto health = client.get_health();

  ASSERT_TRUE(health) << health.error().message;
  EXPECT_EQ(health->state, gravesentinel::ccm::HealthState::healthy);
  EXPECT_EQ(health->queue, gravesentinel::ccm::QueueState::available);
  EXPECT_EQ(handshakes.load(), 1);
  EXPECT_EQ(health_calls.load(), 1);
}

TEST_F(IpcServerClientTest, MapsOperationStoreSaturationToServerBusy) {
  start_server([](const nlohmann::json &request) {
    if (request.at("method") == "Handshake")
      return handshake_result();
    if (request.at("method") == "SubmitCreateCheckpoint") {
      return nlohmann::json{{"ok", false}, {"error_code", "server_busy"}};
    }
    return nlohmann::json{{"ok", false}};
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  const auto container = ContainerId::create("container-busy").value();
  const auto request_id = RequestId::create("request-busy").value();
  const auto submitted = client.submit_create_checkpoint(container, request_id);
  ASSERT_FALSE(submitted);
  EXPECT_EQ(submitted.error().code,
            gravesentinel::ccm::RpcFailureCode::server_busy);
}

TEST_F(IpcServerClientTest,
       MinimumRequestLimitStillAllowsBusinessRequestAndSignalsRotation) {
  std::atomic<int> handshakes{0};
  std::atomic<int> registrations{0};
  start_server(
      [&](const nlohmann::json &request) {
        const auto method = request.at("method").get<std::string>();
        if (method == "Handshake") {
          ++handshakes;
          return handshake_result();
        }
        if (method == "GetHealth")
          return health_result();
        if (method == "Register") {
          ++registrations;
          return nlohmann::json::object();
        }
        return nlohmann::json{{"ok", false}};
      },
      [](gravesentinel::ipc::ServerConfig &config) {
        config.max_requests_per_connection = 1;
      });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  ASSERT_TRUE(client.get_health());
  const auto request_id = RequestId::create("register-after-rotation").value();
  EXPECT_TRUE(client.register_container(complete_registration(), request_id));
  EXPECT_EQ(handshakes.load(), 2);
  EXPECT_EQ(registrations.load(), 1);
}

TEST_F(IpcServerClientTest, SecuresAndValidatesLockFile) {
  {
    const int fd =
        ::open(lock_path_.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::close(fd), 0);
    ASSERT_EQ(::chmod(lock_path_.c_str(), 0666), 0);
  }
  start_server([](const nlohmann::json &request) {
    if (request.at("method") == "Handshake")
      return handshake_result();
    return health_result();
  });

  struct stat status{};
  ASSERT_EQ(::lstat(lock_path_.c_str(), &status), 0);
  EXPECT_TRUE(S_ISREG(status.st_mode));
  EXPECT_EQ(status.st_uid, ::geteuid());
  EXPECT_EQ(status.st_nlink, 1);
  EXPECT_EQ(status.st_mode & 07777, 0600);
  EXPECT_FALSE(gravesentinel::ipc::ServerLock::acquire(lock_path_));
  stop_server();

  const auto target = (directory_ / "lock-target").string();
  {
    const int fd = ::open(target.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::close(fd), 0);
  }
  ASSERT_EQ(::unlink(lock_path_.c_str()), 0);
  ASSERT_EQ(::symlink(target.c_str(), lock_path_.c_str()), 0);
  EXPECT_FALSE(gravesentinel::ipc::ServerLock::acquire(lock_path_));

  ASSERT_EQ(::unlink(lock_path_.c_str()), 0);
  ASSERT_EQ(::link(target.c_str(), lock_path_.c_str()), 0);
  EXPECT_FALSE(gravesentinel::ipc::ServerLock::acquire(lock_path_));
}

TEST_F(IpcServerClientTest, TransfersCompleteRegistrationSchema) {
  std::mutex captured_mutex;
  nlohmann::json captured;
  start_server([&](const nlohmann::json &request) {
    const auto method = request.at("method").get<std::string>();
    if (method == "Handshake")
      return handshake_result();
    if (method == "Register") {
      std::lock_guard lock(captured_mutex);
      captured = request.at("params");
      return nlohmann::json{{"ok", true}};
    }
    return nlohmann::json{{"ok", false}, {"error", "unexpected method"}};
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  const auto request_id = RequestId::create("registration-request").value();
  ASSERT_TRUE(client.register_container(complete_registration(), request_id));

  std::lock_guard lock(captured_mutex);
  ASSERT_TRUE(captured.is_object());
  EXPECT_EQ(captured.size(), 5U);
  EXPECT_EQ(captured.at("checkpoint").at("interval_seconds"), 60);
  EXPECT_EQ(captured.at("cold_start").at("argv").at(1), "");
  EXPECT_EQ(captured.at("cold_start").at("environment").size(), 2U);
  EXPECT_EQ(captured.at("cold_start").at("mounts").at(0).at("target"),
            "/input");
  EXPECT_EQ(captured.at("runtime_fingerprint").at("runtime_name"), "podman");
  EXPECT_EQ(captured.at("runtime_fingerprint").size(), 20U);
}

TEST_F(IpcServerClientTest, DoesNotRetryAmbiguousRegister) {
  const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
  ASSERT_EQ(
      ::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
      0);
  ASSERT_EQ(::listen(listener, 2), 0);

  std::atomic<int> registrations{0};
  std::atomic<bool> server_ok{true};
  std::thread server([&] {
    const auto receive_request =
        [&](const int peer) -> std::optional<nlohmann::json> {
      std::string packet(gravesentinel::ipc::max_message_bytes, '\0');
      const auto received = ::recv(peer, packet.data(), packet.size(), 0);
      if (received <= 0)
        return std::nullopt;
      packet.resize(static_cast<std::size_t>(received));
      auto decoded = gravesentinel::ipc::decode_request(packet);
      if (!decoded)
        return std::nullopt;
      return std::move(*decoded);
    };
    const auto serve_exchange = [&](const int peer) {
      const auto handshake = receive_request(peer);
      if (!handshake || handshake->at("method") != "Handshake")
        return false;
      const nlohmann::json response{
          {"version", gravesentinel::ipc::protocol_version},
          {"request_id", handshake->at("request_id")},
          {"ok", true},
          {"result", handshake_result()},
          {"close_connection", false},
      };
      const auto encoded = gravesentinel::ipc::encode_response(response);
      if (!encoded ||
          ::send(peer, encoded->data(), encoded->size(), MSG_NOSIGNAL) !=
              static_cast<ssize_t>(encoded->size())) {
        return false;
      }
      const auto registration = receive_request(peer);
      if (!registration || registration->at("method") != "Register")
        return false;
      ++registrations;
      return true;
    };

    const int first = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (first < 0) {
      server_ok = false;
      return;
    }
    server_ok = serve_exchange(first);
    ::close(first);

    pollfd descriptor{listener, POLLIN, 0};
    if (::poll(&descriptor, 1, 250) > 0 && (descriptor.revents & POLLIN) != 0) {
      const int second = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (second < 0) {
        server_ok = false;
        return;
      }
      server_ok = serve_exchange(second) && server_ok.load();
      ::close(second);
    }
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 100ms, 1s,
                                             ::geteuid());
  const auto request_id = RequestId::create("ambiguous-register").value();
  EXPECT_FALSE(client.register_container(complete_registration(), request_id));

  server.join();
  ASSERT_EQ(::close(listener), 0);
  EXPECT_TRUE(server_ok.load());
  EXPECT_EQ(registrations.load(), 1);
}

TEST_F(IpcServerClientTest, ResponseEchoesRequestIdAndHidesDiagnostics) {
  start_server([](const nlohmann::json &request) {
    if (request.at("method") == "Handshake")
      return handshake_result();
    if (request.at("method") == "GetOperation") {
      return nlohmann::json{
          {"operation_id", "op-legacy"},
          {"request_id", "legacy-submit-request"},
          {"method", "SubmitRestoreOrRestart"},
          {"container_id", "container-1"},
          {"state", "indeterminate"},
          {"result", "secret reconciliation diagnostic"},
      };
    }
    return nlohmann::json{{"ok", false},
                          {"error", "secret internal database diagnostic"}};
  });

  const int peer = connect_raw(socket_path_);
  ASSERT_GE(peer, 0);
  const auto exchange = [peer](const nlohmann::json &request) {
    const auto packet = request.dump();
    const auto sent = ::send(peer, packet.data(), packet.size(), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(packet.size()))
      return nlohmann::json::object();
    std::string response(gravesentinel::ipc::max_message_bytes + 1, '\0');
    const auto received = ::recv(peer, response.data(), response.size(), 0);
    if (received <= 0)
      return nlohmann::json::object();
    response.resize(static_cast<std::size_t>(received));
    return nlohmann::json::parse(response);
  };

  const auto handshake =
      exchange({{"version", gravesentinel::ipc::protocol_version},
                {"method", "Handshake"},
                {"request_id", "raw-handshake"},
                {"params", nlohmann::json::object()}});
  ASSERT_EQ(handshake.at("request_id"), "raw-handshake");
  ASSERT_TRUE(handshake.at("ok").get<bool>());

  const auto failed =
      exchange({{"version", gravesentinel::ipc::protocol_version},
                {"method", "GetHealth"},
                {"request_id", "raw-health"},
                {"params", nlohmann::json::object()}});
  EXPECT_EQ(failed.at("request_id"), "raw-health");
  EXPECT_FALSE(failed.at("ok").get<bool>());
  EXPECT_EQ(failed.at("error_code"), "request_failed");
  EXPECT_EQ(failed.size(), 5U);
  EXPECT_EQ(failed.dump().find("secret"), std::string::npos);

  const auto operation =
      exchange({{"version", gravesentinel::ipc::protocol_version},
                {"method", "GetOperation"},
                {"request_id", "raw-operation"},
                {"params", {{"operation_id", "op-legacy"}}}});
  ASSERT_TRUE(operation.at("ok").get<bool>());
  EXPECT_EQ(operation.at("request_id"), "raw-operation");
  EXPECT_EQ(operation.at("result").size(), 3U);
  EXPECT_EQ(operation.at("result").at("operation_id"), "op-legacy");
  EXPECT_EQ(operation.at("result").at("state"), "indeterminate");
  EXPECT_EQ(operation.at("result").at("error_code"), "unavailable");
  EXPECT_EQ(operation.dump().find("secret"), std::string::npos);
  ::close(peer);
}

TEST_F(IpcServerClientTest, ClientRejectsUnexpectedServerUid) {
  start_server([](const nlohmann::json &request) {
    if (request.at("method") == "Handshake")
      return handshake_result();
    return health_result();
  });
  const uid_t unexpected_uid = ::geteuid() == 0 ? 1 : 0;
  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             unexpected_uid);

  const auto connected = client.connect();

  ASSERT_FALSE(connected);
  EXPECT_EQ(connected.error().code,
            gravesentinel::ccm::RpcFailureCode::permission_denied);
}

TEST_F(IpcServerClientTest, ServerRejectsDisallowedPeerUid) {
  const uid_t disallowed_uid = ::geteuid() == 0 ? 1 : 0;
  start_server(
      [](const nlohmann::json &request) {
        if (request.at("method") == "Handshake")
          return handshake_result();
        return health_result();
      },
      [disallowed_uid](gravesentinel::ipc::ServerConfig &config) {
        config.allowed_uid = disallowed_uid;
      });
  gravesentinel::ccm::SchedulerClient client(socket_path_, 200ms, 1s,
                                             ::geteuid());

  const auto connected = client.connect();

  ASSERT_FALSE(connected);
  EXPECT_EQ(connected.error().code,
            gravesentinel::ccm::RpcFailureCode::unavailable);
}

TEST_F(IpcServerClientTest, SilentPeerDoesNotBlockSecondClient) {
  start_server(
      [](const nlohmann::json &request) {
        if (request.at("method") == "Handshake")
          return handshake_result();
        if (request.at("method") == "GetHealth")
          return health_result();
        return nlohmann::json{{"ok", false}, {"error", "unexpected"}};
      },
      [](gravesentinel::ipc::ServerConfig &config) {
        config.peer_io_timeout = 800ms;
        config.worker_count = 2;
        config.peer_queue_capacity = 4;
      });

  const int silent_peer = connect_raw(socket_path_);
  ASSERT_GE(silent_peer, 0);
  std::this_thread::sleep_for(50ms);

  const auto started = std::chrono::steady_clock::now();
  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  const auto health = client.get_health();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_TRUE(health) << health.error().message;
  EXPECT_LT(elapsed, 600ms);
  ::close(silent_peer);
}

TEST_F(IpcServerClientTest, BoolOperationsPollUntilTerminalState) {
  std::atomic<int> create_polls{0};
  std::mutex ids_mutex;
  std::vector<std::string> request_ids;
  start_server([&](const nlohmann::json &request) {
    {
      std::lock_guard lock(ids_mutex);
      request_ids.push_back(request.at("request_id").get<std::string>());
    }
    const auto method = request.at("method").get<std::string>();
    if (method == "Handshake")
      return handshake_result();
    if (method == "SubmitCreateCheckpoint") {
      return nlohmann::json{{"operation_id", "op-create"},
                            {"duplicate", false}};
    }
    if (method == "SubmitRestoreOrRestart") {
      return nlohmann::json{{"operation_id", "op-restore"},
                            {"duplicate", false}};
    }
    if (method == "GetOperation") {
      const auto operation =
          request.at("params").at("operation_id").get<std::string>();
      if (operation == "op-create") {
        const auto poll = ++create_polls;
        return nlohmann::json{
            {"operation_id", operation},
            {"state", poll == 1 ? "queued" : "succeeded"},
            {"error_code", nullptr},
        };
      }
      return nlohmann::json{{"operation_id", operation},
                            {"state", "indeterminate"},
                            {"error_code", "restore_failed"}};
    }
    return nlohmann::json{{"ok", false}, {"error", "unexpected method"}};
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 1s,
                                             ::geteuid());
  const auto container = ContainerId::create("container-1").value();
  EXPECT_TRUE(client.create_checkpoint(container));
  EXPECT_FALSE(client.restore_or_restart(container));
  EXPECT_GE(create_polls.load(), 2);

  std::lock_guard lock(ids_mutex);
  const std::set<std::string> unique_ids(request_ids.begin(),
                                         request_ids.end());
  EXPECT_EQ(unique_ids.size(), request_ids.size());
  for (const auto &id : request_ids) {
    EXPECT_EQ(id.find("req-"), 0U);
    EXPECT_EQ(id.size(), 36U);
  }
}

TEST_F(IpcServerClientTest, QueuedOperationStopsAtOperationDeadline) {
  start_server([](const nlohmann::json &request) {
    const auto method = request.at("method").get<std::string>();
    if (method == "Handshake")
      return handshake_result();
    if (method == "SubmitCreateCheckpoint") {
      return nlohmann::json{{"operation_id", "op-never-finishes"},
                            {"duplicate", false}};
    }
    if (method == "GetOperation") {
      return nlohmann::json{{"operation_id", "op-never-finishes"},
                            {"state", "running"},
                            {"error_code", nullptr}};
    }
    return nlohmann::json{{"ok", false}, {"error", "unexpected method"}};
  });

  gravesentinel::ccm::SchedulerClient client(socket_path_, 300ms, 120ms,
                                             ::geteuid());
  const auto container = ContainerId::create("container-1").value();
  const auto started = std::chrono::steady_clock::now();

  EXPECT_FALSE(client.create_checkpoint(container));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
}
} // namespace
