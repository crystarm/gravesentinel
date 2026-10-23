#include "src/ipc/server.hxx"

#include "src/ipc/codec.hxx"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <initializer_list>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gravesentinel::ipc {
namespace {
using Clock = std::chrono::steady_clock;

[[nodiscard]] std::chrono::milliseconds
bounded_timeout(const std::chrono::milliseconds value) {
  return std::clamp(value, std::chrono::milliseconds{1},
                    std::chrono::milliseconds{60'000});
}

[[nodiscard]] int poll_timeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline)
    return 0;
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(std::clamp(remaining, std::chrono::milliseconds{1},
                                     std::chrono::milliseconds{100})
                              .count());
}

[[nodiscard]] bool stop_requested(const StopPredicate &stop) noexcept {
  try {
    return !stop || stop();
  } catch (...) {
    return true;
  }
}

[[nodiscard]] bool write_packet(const int fd, const nlohmann::json &response,
                                const std::chrono::milliseconds timeout,
                                const StopPredicate &stop) {
  const auto encoded = encode_response(response);
  if (!encoded)
    return false;
  const auto deadline = Clock::now() + timeout;
  while (!stop_requested(stop)) {
    const auto sent = ::send(fd, encoded->data(), encoded->size(),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(encoded->size()))
      return true;
    if (sent >= 0 ||
        (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return false;
    if (errno == EINTR)
      continue;
    const auto wait = poll_timeout(deadline);
    if (wait == 0)
      return false;
    pollfd descriptor{fd, POLLOUT, 0};
    const auto ready = ::poll(&descriptor, 1, wait);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return false;
    if (ready > 0 && (descriptor.revents & POLLOUT) != 0)
      continue;
    if (ready > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return false;
  }
  return false;
}

[[nodiscard]] std::optional<std::string>
read_packet(const int fd, const std::chrono::milliseconds timeout,
            const StopPredicate &stop) {
  const auto deadline = Clock::now() + timeout;
  while (!stop_requested(stop)) {
    const auto wait = poll_timeout(deadline);
    if (wait == 0)
      return std::nullopt;
    pollfd descriptor{fd, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, wait);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return std::nullopt;
    if (ready == 0)
      continue;

    // A final packet may be readable together with POLLHUP. Consume POLLIN
    // before treating the hangup as terminal.
    if ((descriptor.revents & POLLIN) != 0) {
      std::string packet(max_message_bytes + 1, '\0');
      iovec part{packet.data(), packet.size()};
      msghdr message{};
      message.msg_iov = &part;
      message.msg_iovlen = 1;
      const auto received = ::recvmsg(fd, &message, MSG_DONTWAIT);
      if (received < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        continue;
      if (received <= 0 || (message.msg_flags & MSG_TRUNC) != 0 ||
          static_cast<std::size_t>(received) > max_message_bytes) {
        return std::nullopt;
      }
      packet.resize(static_cast<std::size_t>(received));
      return packet;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] nlohmann::json error_response(const std::string_view request_id,
                                            const std::string_view code,
                                            const bool close_connection) {
  return {{"version", protocol_version},
          {"request_id", request_id},
          {"ok", false},
          {"error_code", code},
          {"close_connection", close_connection}};
}

[[nodiscard]] nlohmann::json success_response(const std::string_view request_id,
                                              nlohmann::json result,
                                              const bool close_connection) {
  return {{"version", protocol_version},
          {"request_id", request_id},
          {"ok", true},
          {"result", std::move(result)},
          {"close_connection", close_connection}};
}

[[nodiscard]] std::string
correlation_from_untrusted(const std::string_view packet) {
  try {
    const auto message = nlohmann::json::parse(packet, nullptr, false, true);
    if (message.is_object() && message.contains("request_id") &&
        message.at("request_id").is_string()) {
      const auto &candidate =
          message.at("request_id").get_ref<const std::string &>();
      if (valid_identifier(candidate))
        return candidate;
    }
  } catch (const std::exception &) {
  }
  return "invalid-request";
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

[[nodiscard]] bool valid_operation_state(const std::string_view state) {
  return state == "queued" || state == "running" || state == "succeeded" ||
         state == "failed" || state == "indeterminate";
}

[[nodiscard]] bool valid_operation_error(const std::string_view code) {
  return code == "invalid_argument" || code == "not_found" ||
         code == "already_exists" || code == "resource_exhausted" ||
         code == "conflict" || code == "permission_denied" ||
         code == "capture_failed" || code == "storage_error" ||
         code == "corrupted_snapshot" || code == "incompatible_runtime" ||
         code == "restore_failed" || code == "cold_start_failed" ||
         code == "deadline_exceeded" || code == "unavailable" ||
         code == "internal";
}

[[nodiscard]] std::optional<nlohmann::json>
normalize_handler_result(const std::string_view method,
                         const nlohmann::json &result) {
  if (method == "Handshake") {
    if (!has_exact_keys(result, {"protocol_version", "service"}) ||
        !result.at("protocol_version").is_number_unsigned() ||
        result.at("protocol_version").get<unsigned>() != protocol_version ||
        !result.at("service").is_string() ||
        result.at("service").get_ref<const std::string &>() !=
            "gravesentinel_hypervisor") {
      return std::nullopt;
    }
    return result;
  }
  if (method == "GetHealth") {
    if (!has_exact_keys(result, {"status", "queue", "protocol_version"}) ||
        !result.at("status").is_string() || !result.at("queue").is_string() ||
        !result.at("protocol_version").is_number_unsigned() ||
        result.at("protocol_version").get<unsigned>() != protocol_version) {
      return std::nullopt;
    }
    const auto &status = result.at("status").get_ref<const std::string &>();
    const auto &queue = result.at("queue").get_ref<const std::string &>();
    if ((status != "healthy" && status != "degraded" &&
         status != "unhealthy") ||
        (queue != "available" && queue != "saturated")) {
      return std::nullopt;
    }
    return result;
  }
  if (method == "Register" || method == "Unregister")
    return result.empty() ? std::optional<nlohmann::json>(result)
                          : std::nullopt;
  if (method == "SubmitCreateCheckpoint" ||
      method == "SubmitRestoreOrRestart") {
    if (!has_exact_keys(result, {"operation_id", "duplicate"}) ||
        !result.at("operation_id").is_string() ||
        !valid_identifier(
            result.at("operation_id").get_ref<const std::string &>()) ||
        !result.at("duplicate").is_boolean()) {
      return std::nullopt;
    }
    return result;
  }
  if (method != "GetOperation")
    return std::nullopt;

  const bool current_shape =
      has_exact_keys(result, {"operation_id", "state", "error_code"});
  const bool legacy_shape =
      has_exact_keys(result, {"operation_id", "request_id", "method",
                              "container_id", "state", "result"});
  if ((!current_shape && !legacy_shape) ||
      !result.at("operation_id").is_string() ||
      !valid_identifier(
          result.at("operation_id").get_ref<const std::string &>()) ||
      !result.at("state").is_string()) {
    return std::nullopt;
  }
  const auto &state = result.at("state").get_ref<const std::string &>();
  if (!valid_operation_state(state))
    return std::nullopt;

  nlohmann::json error_code = nullptr;
  if (current_shape) {
    const auto &candidate = result.at("error_code");
    if (!candidate.is_null()) {
      if (!candidate.is_string() ||
          !valid_operation_error(candidate.get_ref<const std::string &>())) {
        return std::nullopt;
      }
      error_code = candidate;
    }
  } else {
    if (!result.at("request_id").is_string() ||
        !result.at("method").is_string() ||
        !result.at("container_id").is_string() ||
        !result.at("result").is_string()) {
      return std::nullopt;
    }
    if (state == "failed")
      error_code = "internal";
    else if (state == "indeterminate")
      error_code = "unavailable";
  }
  if ((state == "queued" || state == "running" || state == "succeeded") &&
      !error_code.is_null()) {
    return std::nullopt;
  }
  return nlohmann::json{{"operation_id", result.at("operation_id")},
                        {"state", state},
                        {"error_code", std::move(error_code)}};
}

[[nodiscard]] nlohmann::json invoke_handler(const Handler &handler,
                                            const nlohmann::json &request,
                                            const bool close_connection) {
  const auto &request_id =
      request.at("request_id").get_ref<const std::string &>();
  try {
    auto result = handler(request);
    if (!result.is_object())
      return error_response(request_id, "internal_error", close_connection);
    if (result.contains("ok")) {
      if (!result.at("ok").is_boolean())
        return error_response(request_id, "internal_error", close_connection);
      if (!result.at("ok").get<bool>()) {
        if (result.size() == 2 && result.contains("error_code") &&
            result.at("error_code").is_string() &&
            result.at("error_code").get_ref<const std::string &>() ==
                "server_busy") {
          return error_response(request_id, "server_busy", close_connection);
        }
        if (result.size() == 1 ||
            (result.size() == 2 && result.contains("error"))) {
          return error_response(request_id, "request_failed", close_connection);
        }
        return error_response(request_id, "internal_error", close_connection);
      }
      result.erase("ok");
    }
    result.erase("error");
    const auto &method = request.at("method").get_ref<const std::string &>();
    auto normalized = normalize_handler_result(method, result);
    if (!normalized)
      return error_response(request_id, "internal_error", close_connection);
    return success_response(request_id, std::move(*normalized),
                            close_connection);
  } catch (const std::exception &) {
    return error_response(request_id, "internal_error", close_connection);
  } catch (...) {
    return error_response(request_id, "internal_error", close_connection);
  }
}

class PeerQueue final {
public:
  explicit PeerQueue(const std::size_t capacity) : capacity_(capacity) {}

  [[nodiscard]] bool push(const int fd) {
    std::lock_guard lock(mutex_);
    if (stopping_ || peers_.size() >= capacity_)
      return false;
    peers_.push_back(fd);
    ready_.notify_one();
    return true;
  }

  [[nodiscard]] std::optional<int> pop() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return stopping_ || !peers_.empty(); });
    if (stopping_)
      return std::nullopt;
    const int fd = peers_.front();
    peers_.pop_front();
    return fd;
  }

  void stop() {
    std::lock_guard lock(mutex_);
    if (stopping_)
      return;
    stopping_ = true;
    for (const int fd : peers_)
      ::close(fd);
    peers_.clear();
    ready_.notify_all();
  }

private:
  std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<int> peers_;
  bool stopping_{false};
};

void serve_peer(const int peer, const Handler &handler,
                const std::chrono::milliseconds timeout,
                const std::size_t maximum_requests, const StopPredicate &stop) {
  for (std::size_t count = 0; count < maximum_requests && !stop_requested(stop);
       ++count) {
    auto packet = read_packet(peer, timeout, stop);
    if (!packet)
      break;
    const bool close_connection = count + 1 >= maximum_requests;
    auto request = decode_request(*packet);
    if (!request) {
      const auto request_id = correlation_from_untrusted(*packet);
      if (!write_packet(
              peer,
              error_response(request_id, "invalid_request", close_connection),
              timeout, stop)) {
        break;
      }
      if (close_connection)
        break;
      continue;
    }
    if (!write_packet(peer, invoke_handler(handler, *request, close_connection),
                      timeout, stop)) {
      break;
    }
    if (close_connection)
      break;
  }
  ::close(peer);
}

[[nodiscard]] bool peer_is_authorized(const int peer,
                                      const ServerConfig &config) {
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
      size != sizeof(credentials)) {
    return false;
  }
  const uid_t allowed_uid = config.allowed_uid.value_or(::geteuid());
  return credentials.uid == allowed_uid;
}

[[nodiscard]] bool secure_lock_file(const int fd) {
  struct stat status{};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || status.st_nlink != 1)
    return false;
  if (::fchmod(fd, 0600) != 0 || ::fstat(fd, &status) != 0)
    return false;
  return S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
         status.st_nlink == 1 && (status.st_mode & 07777) == 0600;
}

[[nodiscard]] bool old_socket_is_live(const std::string &path) {
  const int probe =
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (probe < 0)
    return false;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int result =
      ::connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  const bool live = result == 0 || errno == EINPROGRESS || errno == EAGAIN;
  ::close(probe);
  return live;
}
} // namespace

std::optional<ServerLock> ServerLock::acquire(std::string path) {
  const int fd =
      ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return std::nullopt;
  if (!secure_lock_file(fd) || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return std::nullopt;
  }
  return ServerLock(std::move(path), fd);
}

ServerLock::ServerLock(std::string path, const int fd) noexcept
    : path_(std::move(path)), fd_(fd) {}

ServerLock::~ServerLock() {
  if (fd_ >= 0)
    ::close(fd_);
}

ServerLock::ServerLock(ServerLock &&other) noexcept
    : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)) {}

ServerLock &ServerLock::operator=(ServerLock &&other) noexcept {
  if (this == &other)
    return *this;
  if (fd_ >= 0)
    ::close(fd_);
  path_ = std::move(other.path_);
  fd_ = std::exchange(other.fd_, -1);
  return *this;
}

int serve(const ServerConfig &config, const ServerLock &lock,
          const Handler &handler, const StopPredicate &stop) {
  if (!stop || lock.fd_ < 0 || lock.path_ != config.lock_path ||
      config.socket_path.empty() ||
      config.socket_path.size() >= sizeof(sockaddr_un{}.sun_path))
    return 2;

  struct stat status{};
  if (::lstat(config.socket_path.c_str(), &status) == 0) {
    if (!S_ISSOCK(status.st_mode))
      return 4;
    if (old_socket_is_live(config.socket_path) ||
        ::unlink(config.socket_path.c_str()) != 0)
      return 5;
  } else if (errno != ENOENT) {
    return 6;
  }

  const int listener =
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listener < 0)
    return 7;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, config.socket_path.c_str(),
              config.socket_path.size() + 1);
  bool configured = ::bind(listener, reinterpret_cast<sockaddr *>(&address),
                           sizeof(address)) == 0;
  if (configured && config.socket_gid.has_value()) {
    configured = ::chown(config.socket_path.c_str(), static_cast<uid_t>(-1),
                         *config.socket_gid) == 0;
  }
  if (configured) {
    configured = ::chmod(config.socket_path.c_str(),
                         static_cast<mode_t>(config.mode)) == 0;
  }
  if (configured)
    configured = ::listen(listener, 32) == 0;
  if (!configured) {
    ::close(listener);
    (void)::unlink(config.socket_path.c_str());
    return 8;
  }

  const auto timeout = bounded_timeout(config.peer_io_timeout);
  const auto worker_count = std::clamp<std::size_t>(config.worker_count, 2, 32);
  const auto queue_capacity =
      std::clamp<std::size_t>(config.peer_queue_capacity, 1, 1024);
  const auto request_limit =
      std::clamp<std::size_t>(config.max_requests_per_connection, 2, 1024);
  PeerQueue queue(queue_capacity);
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  try {
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers.emplace_back([&] {
        while (auto peer = queue.pop())
          serve_peer(*peer, handler, timeout, request_limit, stop);
      });
    }
  } catch (const std::exception &) {
    queue.stop();
    for (auto &worker : workers) {
      if (worker.joinable())
        worker.join();
    }
    ::close(listener);
    (void)::unlink(config.socket_path.c_str());
    return 9;
  }

  while (!stop_requested(stop)) {
    pollfd descriptor{listener, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
      continue;
    for (;;) {
      const int peer =
          ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (peer < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (!peer_is_authorized(peer, config) || !queue.push(peer))
        ::close(peer);
    }
  }

  ::close(listener);
  queue.stop();
  for (auto &worker : workers)
    worker.join();
  (void)::unlink(config.socket_path.c_str());
  return 0;
}
} // namespace gravesentinel::ipc
