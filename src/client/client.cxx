#include "src/client/scheduler_client.hxx"

#include "src/ipc/codec.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string_view>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace gravesentinel::ccm {
namespace {
using Clock = std::chrono::steady_clock;
using JsonResult = std::expected<nlohmann::json, RpcFailure>;

[[nodiscard]] RpcFailure failure(const RpcFailureCode code,
                                 std::string message) {
  return RpcFailure{code, std::move(message)};
}

[[nodiscard]] std::expected<std::string, RpcFailure> random_request_id() {
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset,
                                   GRND_NONBLOCK);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable,
                  "secure request ID generation is temporarily unavailable"));
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string id = "req-";
  id.reserve(4 + bytes.size() * 2);
  for (const auto byte : bytes) {
    id.push_back(digits[byte >> 4U]);
    id.push_back(digits[byte & 0x0fU]);
  }
  return id;
}

[[nodiscard]] int poll_timeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline)
    return 0;
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(std::clamp<long long>(remaining, 1, INT_MAX));
}

[[nodiscard]] std::expected<void, RpcFailure>
wait_for(const int fd, const short events, const Clock::time_point deadline) {
  for (;;) {
    const auto wait = poll_timeout(deadline);
    if (wait == 0) {
      return std::unexpected(
          failure(RpcFailureCode::deadline_exceeded, "RPC deadline exceeded"));
    }
    pollfd descriptor{fd, events, 0};
    const auto ready = ::poll(&descriptor, 1, wait);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "socket poll failed"));
    }
    if (ready == 0)
      continue;
    if ((descriptor.revents & events) != 0)
      return {};
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "RPC connection closed"));
    }
  }
}

[[nodiscard]] std::expected<void, RpcFailure>
send_packet(const int fd, const std::string_view packet,
            const Clock::time_point deadline, bool &may_have_been_sent) {
  may_have_been_sent = false;
  for (;;) {
    const auto sent =
        ::send(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(packet.size())) {
      may_have_been_sent = true;
      return {};
    }
    if (sent >= 0) {
      may_have_been_sent = true;
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "partial SOCK_SEQPACKET write"));
    }
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "RPC send failed"));
    }
    if (auto ready = wait_for(fd, POLLOUT, deadline); !ready)
      return ready;
  }
}

[[nodiscard]] std::expected<std::string, RpcFailure>
receive_packet(const int fd, const Clock::time_point deadline) {
  for (;;) {
    if (auto ready = wait_for(fd, POLLIN, deadline); !ready)
      return std::unexpected(ready.error());
    std::string packet(gravesentinel::ipc::max_message_bytes + 1, '\0');
    iovec part{packet.data(), packet.size()};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    const auto received = ::recvmsg(fd, &message, MSG_DONTWAIT);
    if (received < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      continue;
    if (received <= 0) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "RPC connection closed"));
    }
    if ((message.msg_flags & MSG_TRUNC) != 0 ||
        static_cast<std::size_t>(received) >
            gravesentinel::ipc::max_message_bytes) {
      return std::unexpected(failure(RpcFailureCode::protocol_error,
                                     "RPC response exceeds size bound"));
    }
    packet.resize(static_cast<std::size_t>(received));
    return packet;
  }
}

[[nodiscard]] bool
has_exact_keys(const nlohmann::json &value,
               std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.size() != keys.size())
    return false;
  return std::all_of(keys.begin(), keys.end(),
                     [&value](const std::string_view key) {
                       return value.contains(std::string(key));
                     });
}

[[nodiscard]] RpcFailure remote_failure(const std::string_view code) {
  if (code == "invalid_request")
    return failure(RpcFailureCode::invalid_argument, "remote invalid_request");
  if (code == "permission_denied")
    return failure(RpcFailureCode::permission_denied,
                   "remote permission_denied");
  if (code == "server_busy")
    return failure(RpcFailureCode::server_busy, "remote server_busy");
  if (code == "request_failed")
    return failure(RpcFailureCode::request_failed, "remote request_failed");
  return failure(RpcFailureCode::internal_error, "remote internal_error");
}

[[nodiscard]] nlohmann::json
cold_start_json(const ColdStartSpec &specification) {
  nlohmann::json environment = nlohmann::json::array();
  for (const auto &variable : specification.environment) {
    environment.push_back({{"name", variable.name}, {"value", variable.value}});
  }
  nlohmann::json mounts = nlohmann::json::array();
  for (const auto &mount : specification.mounts) {
    mounts.push_back({{"source", mount.source},
                      {"target", mount.target},
                      {"read_only", mount.read_only}});
  }
  return {{"image_reference", specification.image_reference},
          {"argv", specification.argv},
          {"environment", std::move(environment)},
          {"mounts", std::move(mounts)},
          {"runtime_options", specification.runtime_options}};
}

[[nodiscard]] nlohmann::json
fingerprint_json(const RuntimeFingerprint &fingerprint) {
  return {{"runtime_name", fingerprint.runtime_name},
          {"runtime_version", fingerprint.runtime_version},
          {"runtime_build_origin", fingerprint.runtime_build_origin},
          {"architecture", fingerprint.architecture},
          {"kernel_release", fingerprint.kernel_release},
          {"criu_version", fingerprint.criu_version},
          {"oci_runtime_name", fingerprint.oci_runtime_name},
          {"oci_runtime_version", fingerprint.oci_runtime_version},
          {"oci_runtime_commit", fingerprint.oci_runtime_commit},
          {"oci_runtime_features", fingerprint.oci_runtime_features},
          {"cgroup_version", fingerprint.cgroup_version},
          {"cgroup_manager", fingerprint.cgroup_manager},
          {"cgroup_controllers", fingerprint.cgroup_controllers},
          {"privilege_mode", fingerprint.privilege_mode},
          {"user_namespace_mode", fingerprint.user_namespace_mode},
          {"capability_profile", fingerprint.capability_profile},
          {"selinux_mode", fingerprint.selinux_mode},
          {"apparmor_profile", fingerprint.apparmor_profile},
          {"seccomp_profile", fingerprint.seccomp_profile},
          {"command_profile", fingerprint.command_profile}};
}

[[nodiscard]] nlohmann::json
registration_json(const ContainerRegistration &registration) {
  nlohmann::json cold_start = nullptr;
  if (registration.cold_start)
    cold_start = cold_start_json(*registration.cold_start);
  nlohmann::json fingerprint = nullptr;
  if (registration.runtime_fingerprint)
    fingerprint = fingerprint_json(*registration.runtime_fingerprint);
  return {{"container_id", registration.container_id.value()},
          {"runtime_container", registration.runtime_container.value()},
          {"checkpoint",
           {{"interval_seconds", registration.checkpoint.interval.count()},
            {"keep_last", registration.checkpoint.keep_last},
            {"container_cpus", registration.checkpoint.container_cpus.values()},
            {"checkpoint_cpu", registration.checkpoint.checkpoint_cpu}}},
          {"cold_start", std::move(cold_start)},
          {"runtime_fingerprint", std::move(fingerprint)}};
}

} // namespace

struct SchedulerClient::Impl {
  Impl(std::string socket_path, const std::chrono::milliseconds rpc,
       const std::chrono::milliseconds operation,
       const std::optional<uid_t> expected_uid)
      : path(std::move(socket_path)),
        rpc_timeout(std::clamp(rpc, std::chrono::milliseconds{1},
                               std::chrono::milliseconds{60'000})),
        operation_timeout(std::clamp(operation, std::chrono::milliseconds{1},
                                     std::chrono::milliseconds{86'400'000})),
        expected_server_uid(expected_uid.value_or(0)) {}

  ~Impl() {
    if (fd >= 0)
      ::close(fd);
  }

  void close_locked() noexcept {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] std::expected<void, RpcFailure>
  open_transport_locked(const Clock::time_point deadline) {
    close_locked();
    if (path.empty() || path.size() >= sizeof(sockaddr_un{}.sun_path)) {
      return std::unexpected(
          failure(RpcFailureCode::invalid_argument, "socket path is invalid"));
    }
    const int candidate =
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (candidate < 0) {
      return std::unexpected(
          failure(RpcFailureCode::unavailable, "cannot create RPC socket"));
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(candidate, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) != 0) {
      if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
        ::close(candidate);
        return std::unexpected(failure(RpcFailureCode::unavailable,
                                       "cannot connect to RPC server"));
      }
      if (auto ready = wait_for(candidate, POLLOUT, deadline); !ready) {
        ::close(candidate);
        return ready;
      }
      int socket_error = 0;
      socklen_t error_size = sizeof(socket_error);
      if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socket_error,
                       &error_size) != 0 ||
          socket_error != 0) {
        ::close(candidate);
        return std::unexpected(failure(RpcFailureCode::unavailable,
                                       "cannot connect to RPC server"));
      }
    }

    ucred credentials{};
    socklen_t credential_size = sizeof(credentials);
    if (::getsockopt(candidate, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credential_size) != 0 ||
        credential_size != sizeof(credentials)) {
      ::close(candidate);
      return std::unexpected(failure(RpcFailureCode::permission_denied,
                                     "cannot verify RPC server credentials"));
    }
    if (credentials.uid != expected_server_uid) {
      ::close(candidate);
      return std::unexpected(failure(RpcFailureCode::permission_denied,
                                     "RPC server UID does not match"));
    }
    fd = candidate;
    return {};
  }

  [[nodiscard]] JsonResult transact_once_locked(
      const std::string_view method, const std::string_view request_id,
      nlohmann::json parameters, const Clock::time_point deadline,
      bool *const request_may_have_been_sent = nullptr) {
    const nlohmann::json request{
        {"version", gravesentinel::ipc::protocol_version},
        {"method", method},
        {"request_id", request_id},
        {"params", std::move(parameters)},
    };
    auto encoded = gravesentinel::ipc::encode_request(request);
    if (!encoded) {
      return std::unexpected(
          failure(RpcFailureCode::invalid_argument, encoded.error().message));
    }
    bool sent_request = false;
    if (auto sent = send_packet(fd, *encoded, deadline, sent_request); !sent) {
      if (request_may_have_been_sent != nullptr)
        *request_may_have_been_sent = sent_request;
      return std::unexpected(sent.error());
    }
    if (request_may_have_been_sent != nullptr)
      *request_may_have_been_sent = true;
    auto packet = receive_packet(fd, deadline);
    if (!packet)
      return std::unexpected(packet.error());
    auto response = gravesentinel::ipc::decode_response(*packet);
    if (!response) {
      return std::unexpected(failure(RpcFailureCode::protocol_error,
                                     "invalid RPC response envelope"));
    }
    if (response->at("request_id").get_ref<const std::string &>() !=
        request_id) {
      return std::unexpected(failure(RpcFailureCode::protocol_error,
                                     "RPC response correlation mismatch"));
    }
    if (response->at("close_connection").get<bool>())
      close_locked();
    if (!response->at("ok").get<bool>()) {
      return std::unexpected(remote_failure(
          response->at("error_code").get_ref<const std::string &>()));
    }
    return response->at("result");
  }

  [[nodiscard]] std::expected<void, RpcFailure>
  connect_locked(const Clock::time_point deadline) {
    if (auto opened = open_transport_locked(deadline); !opened)
      return opened;
    auto request_id = random_request_id();
    if (!request_id) {
      close_locked();
      return std::unexpected(request_id.error());
    }
    auto handshake = transact_once_locked("Handshake", *request_id,
                                          nlohmann::json::object(), deadline);
    if (!handshake) {
      close_locked();
      return std::unexpected(handshake.error());
    }
    if (!has_exact_keys(*handshake, {"protocol_version", "service"}) ||
        !handshake->at("protocol_version").is_number_unsigned() ||
        handshake->at("protocol_version").get<unsigned>() !=
            gravesentinel::ipc::protocol_version ||
        !handshake->at("service").is_string() ||
        handshake->at("service").get_ref<const std::string &>() !=
            "gravesentinel_hypervisor") {
      close_locked();
      return std::unexpected(
          failure(RpcFailureCode::protocol_error, "RPC handshake mismatch"));
    }
    return {};
  }

  [[nodiscard]] JsonResult
  call(const std::string_view method, const std::string_view request_id,
       nlohmann::json parameters,
       const std::optional<Clock::time_point> operation_deadline = std::nullopt,
       const bool retry_after_exchange_failure = true) {
    std::lock_guard lock(mutex);
    auto deadline = Clock::now() + rpc_timeout;
    if (operation_deadline)
      deadline = std::min(deadline, *operation_deadline);
    if (Clock::now() >= deadline) {
      return std::unexpected(
          failure(RpcFailureCode::deadline_exceeded, "RPC deadline exceeded"));
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (fd < 0) {
        if (auto connected = connect_locked(deadline); !connected) {
          if (connected.error().code == RpcFailureCode::unavailable &&
              attempt == 0)
            continue;
          return std::unexpected(connected.error());
        }
      }
      bool request_may_have_been_sent = false;
      auto result = transact_once_locked(method, request_id, parameters,
                                         deadline, &request_may_have_been_sent);
      if (result)
        return result;
      const auto error = result.error();
      if (error.code == RpcFailureCode::unavailable ||
          error.code == RpcFailureCode::deadline_exceeded ||
          error.code == RpcFailureCode::protocol_error) {
        close_locked();
      }
      if ((!request_may_have_been_sent || retry_after_exchange_failure) &&
          error.code == RpcFailureCode::unavailable && attempt == 0 &&
          Clock::now() < deadline)
        continue;
      return std::unexpected(error);
    }
    return std::unexpected(
        failure(RpcFailureCode::unavailable, "RPC transport unavailable"));
  }

  [[nodiscard]] std::expected<void, RpcFailure> connect() {
    std::lock_guard lock(mutex);
    return connect_locked(Clock::now() + rpc_timeout);
  }

  std::string path;
  std::chrono::milliseconds rpc_timeout;
  std::chrono::milliseconds operation_timeout;
  uid_t expected_server_uid;
  std::mutex mutex;
  int fd{-1};
};

namespace {

[[nodiscard]] std::expected<OperationId, RpcFailure>
parse_submission(const nlohmann::json &result) {
  if (!has_exact_keys(result, {"operation_id", "duplicate"}) ||
      !result.at("operation_id").is_string() ||
      !result.at("duplicate").is_boolean()) {
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "invalid submission response"));
  }
  const auto &text = result.at("operation_id").get_ref<const std::string &>();
  if (!gravesentinel::ipc::valid_identifier(text)) {
    return std::unexpected(failure(RpcFailureCode::protocol_error,
                                   "invalid operation identifier"));
  }
  auto operation_id = OperationId::create(text);
  if (!operation_id) {
    return std::unexpected(failure(RpcFailureCode::protocol_error,
                                   "invalid operation identifier"));
  }
  return std::move(*operation_id);
}

[[nodiscard]] std::optional<StableErrorCode>
parse_stable_error(const std::string_view code) {
  if (code == "invalid_argument")
    return StableErrorCode::invalid_argument;
  if (code == "not_found")
    return StableErrorCode::not_found;
  if (code == "already_exists")
    return StableErrorCode::already_exists;
  if (code == "resource_exhausted")
    return StableErrorCode::resource_exhausted;
  if (code == "conflict")
    return StableErrorCode::conflict;
  if (code == "permission_denied")
    return StableErrorCode::permission_denied;
  if (code == "capture_failed")
    return StableErrorCode::capture_failed;
  if (code == "storage_error")
    return StableErrorCode::storage_error;
  if (code == "corrupted_snapshot")
    return StableErrorCode::corrupted_snapshot;
  if (code == "incompatible_runtime")
    return StableErrorCode::incompatible_runtime;
  if (code == "restore_failed")
    return StableErrorCode::restore_failed;
  if (code == "cold_start_failed")
    return StableErrorCode::cold_start_failed;
  if (code == "deadline_exceeded")
    return StableErrorCode::deadline_exceeded;
  if (code == "unavailable")
    return StableErrorCode::unavailable;
  if (code == "internal")
    return StableErrorCode::internal;
  return std::nullopt;
}

[[nodiscard]] std::expected<OperationStatus, RpcFailure>
parse_operation(const nlohmann::json &result,
                const OperationId &expected_operation_id) {
  if (!has_exact_keys(result, {"operation_id", "state", "error_code"}) ||
      !result.at("operation_id").is_string() ||
      !result.at("state").is_string() ||
      (!result.at("error_code").is_null() &&
       !result.at("error_code").is_string())) {
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "invalid operation response"));
  }
  const auto &operation_text =
      result.at("operation_id").get_ref<const std::string &>();
  if (operation_text != expected_operation_id.value()) {
    return std::unexpected(failure(RpcFailureCode::protocol_error,
                                   "operation response correlation mismatch"));
  }

  OperationState state;
  const auto &state_text = result.at("state").get_ref<const std::string &>();
  if (state_text == "queued")
    state = OperationState::queued;
  else if (state_text == "running")
    state = OperationState::running;
  else if (state_text == "succeeded")
    state = OperationState::succeeded;
  else if (state_text == "failed")
    state = OperationState::failed;
  else if (state_text == "indeterminate")
    state = OperationState::indeterminate;
  else
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "unknown operation state"));

  std::optional<StableErrorCode> operation_error;
  if (result.at("error_code").is_string()) {
    operation_error = parse_stable_error(
        result.at("error_code").get_ref<const std::string &>());
    if (!operation_error) {
      return std::unexpected(failure(RpcFailureCode::protocol_error,
                                     "unknown operation error code"));
    }
  }
  if ((state == OperationState::queued || state == OperationState::running ||
       state == OperationState::succeeded) &&
      operation_error.has_value()) {
    return std::unexpected(failure(RpcFailureCode::protocol_error,
                                   "unexpected operation error code"));
  }
  return OperationStatus{expected_operation_id, state, operation_error};
}

[[nodiscard]] std::expected<HealthStatus, RpcFailure>
parse_health(const nlohmann::json &result) {
  if (!has_exact_keys(result, {"status", "queue", "protocol_version"}) ||
      !result.at("status").is_string() || !result.at("queue").is_string() ||
      !result.at("protocol_version").is_number_unsigned() ||
      result.at("protocol_version").get<unsigned>() !=
          gravesentinel::ipc::protocol_version) {
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "invalid health response"));
  }
  HealthState health;
  const auto &health_text = result.at("status").get_ref<const std::string &>();
  if (health_text == "healthy")
    health = HealthState::healthy;
  else if (health_text == "degraded")
    health = HealthState::degraded;
  else if (health_text == "unhealthy")
    health = HealthState::unhealthy;
  else
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "unknown health state"));

  QueueState queue;
  const auto &queue_text = result.at("queue").get_ref<const std::string &>();
  if (queue_text == "available")
    queue = QueueState::available;
  else if (queue_text == "saturated")
    queue = QueueState::saturated;
  else
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "unknown queue state"));
  return HealthStatus{health, queue, gravesentinel::ipc::protocol_version};
}

[[nodiscard]] std::expected<RuntimeStatus, RpcFailure>
parse_runtime_status(const nlohmann::json &result) {
  if (!has_exact_keys(result, {"state", "exit_code"}) ||
      !result.at("state").is_string() ||
      (!result.at("exit_code").is_null() &&
       !result.at("exit_code").is_number_integer())) {
    return std::unexpected(failure(RpcFailureCode::protocol_error,
                                   "invalid runtime status response"));
  }
  RuntimeState state;
  const auto &state_text = result.at("state").get_ref<const std::string &>();
  if (state_text == "absent")
    state = RuntimeState::absent;
  else if (state_text == "running")
    state = RuntimeState::running;
  else if (state_text == "exited")
    state = RuntimeState::exited;
  else
    return std::unexpected(
        failure(RpcFailureCode::protocol_error, "unknown runtime state"));

  std::optional<int> exit_code;
  if (result.at("exit_code").is_number_integer()) {
    const auto value = result.at("exit_code").get<std::int64_t>();
    if (value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
      return std::unexpected(failure(RpcFailureCode::protocol_error,
                                     "runtime exit code is out of range"));
    }
    exit_code = static_cast<int>(value);
  }
  if ((state == RuntimeState::exited) != exit_code.has_value()) {
    return std::unexpected(
        failure(RpcFailureCode::protocol_error,
                "runtime status has an inconsistent exit code"));
  }
  return RuntimeStatus{.state = state, .exit_code = exit_code};
}

[[nodiscard]] RpcFailure moved_client_failure() {
  return failure(RpcFailureCode::unavailable, "client has been moved");
}
} // namespace

SchedulerClient::SchedulerClient(
    std::string path, const std::chrono::milliseconds rpc_timeout,
    const std::chrono::milliseconds operation_timeout,
    const std::optional<uid_t> expected_server_uid)
    : impl_(std::make_unique<Impl>(std::move(path), rpc_timeout,
                                   operation_timeout, expected_server_uid)) {}

SchedulerClient::~SchedulerClient() = default;
SchedulerClient::SchedulerClient(SchedulerClient &&) noexcept = default;
SchedulerClient &
SchedulerClient::operator=(SchedulerClient &&) noexcept = default;

std::expected<void, RpcFailure> SchedulerClient::connect() {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  return impl_->connect();
}

bool SchedulerClient::register_container(
    const ContainerRegistration &registration, const RequestId &request_id) {
  if (!impl_)
    return false;
  auto result =
      impl_->call("Register", request_id.value(),
                  registration_json(registration), std::nullopt, false);
  return result && result->empty();
}

bool SchedulerClient::register_container(
    const ContainerRegistration &registration) {
  auto request_id = random_request_id();
  if (!request_id)
    return false;
  auto typed_id = RequestId::create(std::move(*request_id));
  return typed_id && register_container(registration, *typed_id);
}

bool SchedulerClient::unregister_container(const ContainerId &container_id,
                                           const RequestId &request_id) {
  if (!impl_)
    return false;
  auto result = impl_->call("Unregister", request_id.value(),
                            {{"container_id", container_id.value()}},
                            std::nullopt, false);
  return result && result->empty();
}

bool SchedulerClient::unregister_container(const ContainerId &container_id) {
  auto request_id = random_request_id();
  if (!request_id)
    return false;
  auto typed_id = RequestId::create(std::move(*request_id));
  return typed_id && unregister_container(container_id, *typed_id);
}

std::expected<OperationId, RpcFailure>
SchedulerClient::submit_create_checkpoint(const ContainerId &container_id,
                                          const RequestId &request_id) {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto result = impl_->call("SubmitCreateCheckpoint", request_id.value(),
                            {{"container_id", container_id.value()}});
  if (!result)
    return std::unexpected(result.error());
  return parse_submission(*result);
}

std::expected<OperationId, RpcFailure>
SchedulerClient::submit_restore_or_restart(const ContainerId &container_id,
                                           const RequestId &request_id) {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto result = impl_->call("SubmitRestoreOrRestart", request_id.value(),
                            {{"container_id", container_id.value()}});
  if (!result)
    return std::unexpected(result.error());
  return parse_submission(*result);
}

std::expected<OperationId, RpcFailure>
SchedulerClient::submit_stop_container(const ContainerId &container_id,
                                       const RequestId &request_id) {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto result = impl_->call("SubmitStopContainer", request_id.value(),
                            {{"container_id", container_id.value()}});
  if (!result)
    return std::unexpected(result.error());
  return parse_submission(*result);
}

std::expected<OperationStatus, RpcFailure>
SchedulerClient::get_operation(const OperationId &operation_id) {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto request_id = random_request_id();
  if (!request_id)
    return std::unexpected(request_id.error());
  auto result = impl_->call("GetOperation", *request_id,
                            {{"operation_id", operation_id.value()}});
  if (!result)
    return std::unexpected(result.error());
  return parse_operation(*result, operation_id);
}

std::expected<HealthStatus, RpcFailure> SchedulerClient::get_health() {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto request_id = random_request_id();
  if (!request_id)
    return std::unexpected(request_id.error());
  auto result = impl_->call("GetHealth", *request_id, nlohmann::json::object());
  if (!result)
    return std::unexpected(result.error());
  return parse_health(*result);
}

std::expected<RuntimeStatus, RpcFailure>
SchedulerClient::get_runtime_status(const ContainerId &container_id) {
  if (!impl_)
    return std::unexpected(moved_client_failure());
  auto request_id = random_request_id();
  if (!request_id)
    return std::unexpected(request_id.error());
  auto result = impl_->call("GetRuntimeStatus", *request_id,
                            {{"container_id", container_id.value()}});
  if (!result)
    return std::unexpected(result.error());
  return parse_runtime_status(*result);
}

bool SchedulerClient::create_checkpoint(const ContainerId &container_id) {
  if (!impl_)
    return false;
  const auto deadline = Clock::now() + impl_->operation_timeout;
  auto request_id = random_request_id();
  if (!request_id)
    return false;
  auto submitted =
      impl_->call("SubmitCreateCheckpoint", *request_id,
                  {{"container_id", container_id.value()}}, deadline);
  if (!submitted)
    return false;
  auto operation_id = parse_submission(*submitted);
  if (!operation_id)
    return false;

  while (Clock::now() < deadline) {
    auto poll_id = random_request_id();
    if (!poll_id)
      return false;
    auto result =
        impl_->call("GetOperation", *poll_id,
                    {{"operation_id", operation_id->value()}}, deadline);
    if (!result)
      return false;
    auto status = parse_operation(*result, *operation_id);
    if (!status)
      return false;
    if (status->state == OperationState::succeeded)
      return true;
    if (status->state == OperationState::failed ||
        status->state == OperationState::indeterminate)
      return false;
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero())
      return false;
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{25},
                 std::chrono::ceil<std::chrono::milliseconds>(remaining)));
  }
  return false;
}

bool SchedulerClient::restore_or_restart(const ContainerId &container_id) {
  if (!impl_)
    return false;
  const auto deadline = Clock::now() + impl_->operation_timeout;
  auto request_id = random_request_id();
  if (!request_id)
    return false;
  auto submitted =
      impl_->call("SubmitRestoreOrRestart", *request_id,
                  {{"container_id", container_id.value()}}, deadline);
  if (!submitted)
    return false;
  auto operation_id = parse_submission(*submitted);
  if (!operation_id)
    return false;

  while (Clock::now() < deadline) {
    auto poll_id = random_request_id();
    if (!poll_id)
      return false;
    auto result =
        impl_->call("GetOperation", *poll_id,
                    {{"operation_id", operation_id->value()}}, deadline);
    if (!result)
      return false;
    auto status = parse_operation(*result, *operation_id);
    if (!status)
      return false;
    if (status->state == OperationState::succeeded)
      return true;
    if (status->state == OperationState::failed ||
        status->state == OperationState::indeterminate)
      return false;
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero())
      return false;
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{25},
                 std::chrono::ceil<std::chrono::milliseconds>(remaining)));
  }
  return false;
}

bool SchedulerClient::stop_container(const ContainerId &container_id) {
  if (!impl_)
    return false;
  const auto deadline = Clock::now() + impl_->operation_timeout;
  auto request_id = random_request_id();
  if (!request_id)
    return false;
  auto submitted =
      impl_->call("SubmitStopContainer", *request_id,
                  {{"container_id", container_id.value()}}, deadline);
  if (!submitted)
    return false;
  auto operation_id = parse_submission(*submitted);
  if (!operation_id)
    return false;

  while (Clock::now() < deadline) {
    auto poll_id = random_request_id();
    if (!poll_id)
      return false;
    auto result =
        impl_->call("GetOperation", *poll_id,
                    {{"operation_id", operation_id->value()}}, deadline);
    if (!result)
      return false;
    auto status = parse_operation(*result, *operation_id);
    if (!status)
      return false;
    if (status->state == OperationState::succeeded)
      return true;
    if (status->state == OperationState::failed ||
        status->state == OperationState::indeterminate)
      return false;
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero())
      return false;
    std::this_thread::sleep_for(
        std::min(std::chrono::milliseconds{25},
                 std::chrono::ceil<std::chrono::milliseconds>(remaining)));
  }
  return false;
}
} // namespace gravesentinel::ccm
