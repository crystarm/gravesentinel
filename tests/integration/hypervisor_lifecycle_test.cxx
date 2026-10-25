#include "src/client/scheduler_client.hxx"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <fcntl.h>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using gravesentinel::ccm::HealthState;
using gravesentinel::ccm::QueueState;
using gravesentinel::ccm::SchedulerClient;

constexpr auto kStartupTimeout = 10s;
constexpr auto kExitTimeout = 8s;
constexpr auto kLockRefusalTimeout = 5s;
constexpr auto kRawExchangeTimeout = 2s;
constexpr auto kFilesystemTimeout = 2s;
constexpr auto kPollInterval = 20ms;
constexpr std::size_t kMaximumTrackedProcessGroups = 4U;

volatile std::sig_atomic_t termination_signal = 0;
volatile std::sig_atomic_t
    tracked_process_groups[kMaximumTrackedProcessGroups]{};

extern "C" void handle_termination_signal(const int signal_number) {
  termination_signal = signal_number;
  for (std::size_t index = 0; index < kMaximumTrackedProcessGroups; ++index) {
    const std::sig_atomic_t process_group = tracked_process_groups[index];
    if (process_group > 0) {
      static_cast<void>(::kill(-static_cast<pid_t>(process_group), SIGKILL));
    }
  }
}

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

void require(const bool condition, std::string message) {
  if (!condition)
    fail(std::move(message));
}

[[nodiscard]] std::string system_error(const std::string_view operation,
                                       const int error_number = errno) {
  return std::string(operation) + ": " + std::strerror(error_number);
}

void check_interrupted() {
  const std::sig_atomic_t signal_number = termination_signal;
  if (signal_number != 0) {
    fail("interrupted by signal " +
         std::to_string(static_cast<int>(signal_number)));
  }
}

[[nodiscard]] sigset_t termination_signal_set() {
  sigset_t signals{};
  if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGINT) != 0 ||
      ::sigaddset(&signals, SIGTERM) != 0 ||
      ::sigaddset(&signals, SIGHUP) != 0) {
    fail(system_error("cannot create termination signal set"));
  }
  return signals;
}

void install_signal_handlers() {
  struct sigaction action{};
  action.sa_handler = handle_termination_signal;
  action.sa_flags = 0;
  if (::sigemptyset(&action.sa_mask) != 0)
    fail(system_error("cannot initialize signal action"));
  for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
    if (::sigaction(signal_number, &action, nullptr) != 0)
      fail(system_error("cannot install termination signal handler"));
  }

  struct sigaction ignore_pipe{};
  ignore_pipe.sa_handler = SIG_IGN;
  if (::sigemptyset(&ignore_pipe.sa_mask) != 0 ||
      ::sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) {
    fail(system_error("cannot ignore SIGPIPE"));
  }
}

class UniqueFd final {
public:
  explicit UniqueFd(const int descriptor = -1) noexcept
      : descriptor_(descriptor) {}
  ~UniqueFd() {
    if (descriptor_ >= 0)
      static_cast<void>(::close(descriptor_));
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this == &other)
      return *this;
    if (descriptor_ >= 0)
      static_cast<void>(::close(descriptor_));
    descriptor_ = std::exchange(other.descriptor_, -1);
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
  int descriptor_;
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/gravesentinel-lifecycle.XXXXXX";
    char *const created = ::mkdtemp(pattern.data());
    if (created == nullptr)
      fail(system_error("cannot create temporary directory"));
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string read_log(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return "<unavailable>";

  constexpr std::size_t maximum_bytes = 64U * 1024U;
  std::string content;
  content.reserve(4096U);
  std::array<char, 4096> buffer{};
  while (input && content.size() < maximum_bytes) {
    const auto remaining = maximum_bytes - content.size();
    const auto requested =
        static_cast<std::streamsize>(std::min(remaining, buffer.size()));
    input.read(buffer.data(), requested);
    const auto count = input.gcount();
    if (count > 0)
      content.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (input.peek() != std::char_traits<char>::eof())
    content.append("\n<truncated>");
  return content;
}

[[nodiscard]] std::string process_status_text(const int status) {
  if (WIFEXITED(status))
    return "exit " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    return "signal " + std::to_string(WTERMSIG(status));
  return "unknown wait status " + std::to_string(status);
}

class ChildProcess final {
public:
  [[nodiscard]] static std::unique_ptr<ChildProcess>
  spawn(const std::filesystem::path &executable,
        const std::vector<std::string> &arguments,
        const std::filesystem::path &log_path) {
    std::size_t tracking_slot = kMaximumTrackedProcessGroups;
    for (std::size_t index = 0; index < kMaximumTrackedProcessGroups; ++index) {
      if (tracked_process_groups[index] == 0) {
        tracking_slot = index;
        break;
      }
    }
    if (tracking_slot == kMaximumTrackedProcessGroups)
      fail("no free process-group tracking slot");

    const UniqueFd log_fd{::open(
        log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600)};
    if (log_fd.get() < 0)
      fail(system_error("cannot open child log"));

    const std::string executable_text = executable.string();
    std::vector<char *> child_argv;
    child_argv.reserve(arguments.size() + 2U);
    child_argv.push_back(const_cast<char *>(executable_text.c_str()));
    for (const auto &argument : arguments)
      child_argv.push_back(const_cast<char *>(argument.c_str()));
    child_argv.push_back(nullptr);

    const sigset_t signals = termination_signal_set();
    sigset_t previous_mask{};
    if (::sigprocmask(SIG_BLOCK, &signals, &previous_mask) != 0)
      fail(system_error("cannot block termination signals around fork"));

    const pid_t child = ::fork();
    if (child < 0) {
      const int fork_error = errno;
      static_cast<void>(::sigprocmask(SIG_SETMASK, &previous_mask, nullptr));
      fail(system_error("cannot fork daemon", fork_error));
    }
    if (child == 0) {
      if (::setpgid(0, 0) != 0 || ::dup2(log_fd.get(), STDOUT_FILENO) < 0 ||
          ::dup2(log_fd.get(), STDERR_FILENO) < 0) {
        ::_exit(126);
      }
      if (log_fd.get() != STDOUT_FILENO && log_fd.get() != STDERR_FILENO)
        static_cast<void>(::close(log_fd.get()));

      struct sigaction default_action{};
      default_action.sa_handler = SIG_DFL;
      if (::sigemptyset(&default_action.sa_mask) != 0) {
        ::_exit(126);
      }
      for (const int signal_number : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
        if (::sigaction(signal_number, &default_action, nullptr) != 0)
          ::_exit(126);
      }
      if (::sigprocmask(SIG_SETMASK, &previous_mask, nullptr) != 0)
        ::_exit(126);
      ::execv(executable_text.c_str(), child_argv.data());
      ::_exit(127);
    }

    if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
      const int group_error = errno;
      static_cast<void>(::kill(child, SIGKILL));
      int ignored_status = 0;
      while (::waitpid(child, &ignored_status, 0) < 0 && errno == EINTR) {
      }
      static_cast<void>(::sigprocmask(SIG_SETMASK, &previous_mask, nullptr));
      fail(system_error("cannot create daemon process group", group_error));
    }

    tracked_process_groups[tracking_slot] =
        static_cast<std::sig_atomic_t>(child);
    if (::sigprocmask(SIG_SETMASK, &previous_mask, nullptr) != 0) {
      const int mask_error = errno;
      static_cast<void>(::kill(-child, SIGKILL));
      int ignored_status = 0;
      while (::waitpid(child, &ignored_status, 0) < 0 && errno == EINTR) {
      }
      tracked_process_groups[tracking_slot] = 0;
      fail(system_error("cannot restore signal mask after fork", mask_error));
    }

    return std::unique_ptr<ChildProcess>(
        new ChildProcess(child, tracking_slot, log_path));
  }

  ~ChildProcess() { cleanup(); }

  ChildProcess(const ChildProcess &) = delete;
  ChildProcess &operator=(const ChildProcess &) = delete;

  [[nodiscard]] std::optional<int> poll_status() {
    if (status_)
      return status_;
    for (;;) {
      int status = 0;
      const pid_t result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        record_exit(status);
        return status_;
      }
      if (result == 0)
        return std::nullopt;
      if (errno == EINTR) {
        check_interrupted();
        continue;
      }
      fail(system_error("cannot inspect daemon status"));
    }
  }

  [[nodiscard]] std::optional<int>
  wait_for(const std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      check_interrupted();
      if (auto status = poll_status())
        return status;
      std::this_thread::sleep_for(kPollInterval);
    }
    return poll_status();
  }

  void send_signal(const int signal_number) {
    require(!status_.has_value(), "daemon has already exited");
    if (::kill(pid_, signal_number) != 0 && errno != ESRCH)
      fail(system_error("cannot signal daemon"));
  }

  [[nodiscard]] const std::filesystem::path &log_path() const noexcept {
    return log_path_;
  }

private:
  ChildProcess(const pid_t pid, const std::size_t tracking_slot,
               std::filesystem::path log_path)
      : pid_(pid), tracking_slot_(tracking_slot),
        log_path_(std::move(log_path)) {}

  void record_exit(const int status) noexcept {
    status_ = status;
    static_cast<void>(::kill(-pid_, SIGKILL));
    if (tracking_slot_ < kMaximumTrackedProcessGroups) {
      tracked_process_groups[tracking_slot_] = 0;
      tracking_slot_ = kMaximumTrackedProcessGroups;
    }
  }

  void cleanup() noexcept {
    if (!status_) {
      static_cast<void>(::kill(-pid_, SIGTERM));
      const auto deadline = Clock::now() + 500ms;
      while (Clock::now() < deadline) {
        int status = 0;
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
          record_exit(status);
          break;
        }
        if (result < 0 && errno != EINTR) {
          if (errno == ECHILD)
            status_ = 0;
          break;
        }
        std::this_thread::sleep_for(kPollInterval);
      }
    }
    if (!status_) {
      static_cast<void>(::kill(-pid_, SIGKILL));
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      status_ = status;
    }
    static_cast<void>(::kill(-pid_, SIGKILL));
    if (tracking_slot_ < kMaximumTrackedProcessGroups) {
      tracked_process_groups[tracking_slot_] = 0;
      tracking_slot_ = kMaximumTrackedProcessGroups;
    }
  }

  pid_t pid_;
  std::size_t tracking_slot_;
  std::filesystem::path log_path_;
  std::optional<int> status_;
};

[[noreturn]] void fail_with_log(const std::string_view message,
                                const ChildProcess &process) {
  fail(std::string(message) + "\ndaemon log (" + process.log_path().string() +
       "):\n" + read_log(process.log_path()));
}

[[nodiscard]] std::vector<std::string>
daemon_arguments(const std::filesystem::path &socket_path,
                 const std::filesystem::path &storage_path,
                 const std::filesystem::path &temporary_root, const uid_t uid) {
  return {
      "--socket",        socket_path.string(),
      "--storage",       storage_path.string(),
      "--scheduler-uid", std::to_string(uid),
      "--socket-mode",   "0600",
      "--podman",        (temporary_root / "nonexistent-podman").string(),
      "--criu",          (temporary_root / "nonexistent-criu").string(),
  };
}

[[nodiscard]] unsigned wait_for_healthy(ChildProcess &process,
                                        const std::string &socket_path,
                                        const uid_t expected_uid) {
  const auto deadline = Clock::now() + kStartupTimeout;
  std::string last_failure = "daemon did not accept a connection";
  while (Clock::now() < deadline) {
    check_interrupted();
    if (const auto status = process.poll_status(); status) {
      fail_with_log("daemon exited during startup with " +
                        process_status_text(*status),
                    process);
    }

    SchedulerClient client(socket_path, 300ms, 1s,
                           std::optional<uid_t>{expected_uid});
    const auto connected = client.connect();
    if (connected) {
      const auto health = client.get_health();
      if (health && health->state == HealthState::healthy &&
          health->queue == QueueState::available) {
        return health->protocol_version;
      }
      if (health) {
        last_failure = "daemon returned a non-healthy status";
      } else {
        last_failure = "health RPC failed: " + health.error().message;
      }
    } else {
      last_failure = "connect failed: " + connected.error().message;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  fail_with_log("daemon readiness timeout: " + last_failure, process);
}

[[nodiscard]] unsigned require_fresh_health(const std::string &socket_path,
                                            const uid_t expected_uid) {
  SchedulerClient client(socket_path, 1s, 1s,
                         std::optional<uid_t>{expected_uid});
  const auto connected = client.connect();
  require(connected.has_value(), connected ? std::string{}
                                           : "fresh client connect failed: " +
                                                 connected.error().message);
  const auto health = client.get_health();
  require(health.has_value(),
          health ? std::string{}
                 : "fresh client health RPC failed: " + health.error().message);
  require(health->state == HealthState::healthy,
          "fresh client observed a non-healthy daemon");
  require(health->queue == QueueState::available,
          "fresh client observed an unavailable operation queue");
  return health->protocol_version;
}

[[nodiscard]] int poll_timeout(const Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline)
    return 0;
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(std::min(remaining, 100ms).count());
}

[[nodiscard]] short wait_for_events(const int descriptor, const short events,
                                    const Clock::time_point deadline) {
  while (true) {
    check_interrupted();
    const int timeout = poll_timeout(deadline);
    if (timeout == 0)
      return 0;
    pollfd polled{descriptor, events, 0};
    const int result = ::poll(&polled, 1, timeout);
    if (result > 0)
      return polled.revents;
    if (result < 0 && errno != EINTR)
      fail(system_error("poll failed"));
  }
}

[[nodiscard]] UniqueFd connect_raw(const std::string &socket_path,
                                   const Clock::time_point deadline) {
  UniqueFd socket{
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
  if (socket.get() < 0)
    fail(system_error("cannot create raw protocol socket"));
  require(socket_path.size() < sizeof(sockaddr_un{}.sun_path),
          "temporary socket path is too long");

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
  if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) == 0) {
    return socket;
  }
  if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK)
    fail(system_error("raw protocol connect failed"));

  const short ready = wait_for_events(socket.get(), POLLOUT, deadline);
  require((ready & POLLOUT) != 0,
          "raw protocol connect did not complete before its deadline");
  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_size) != 0) {
    fail(system_error("cannot inspect raw protocol connect"));
  }
  require(socket_error == 0,
          system_error("raw protocol connect failed", socket_error));
  return socket;
}

void send_packet(const int descriptor, const std::string_view packet,
                 const Clock::time_point deadline) {
  while (true) {
    check_interrupted();
    const auto sent = ::send(descriptor, packet.data(), packet.size(),
                             MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(packet.size()))
      return;
    if (sent >= 0)
      fail("raw protocol send was partial");
    if (errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      fail(system_error("raw protocol send failed"));
    const short ready = wait_for_events(descriptor, POLLOUT, deadline);
    require((ready & POLLOUT) != 0,
            "raw protocol send did not complete before its deadline");
  }
}

[[nodiscard]] std::string receive_packet(const int descriptor,
                                         const Clock::time_point deadline) {
  while (true) {
    const short ready = wait_for_events(descriptor, POLLIN, deadline);
    require(ready != 0,
            "raw protocol rejection did not arrive before its deadline");
    if ((ready & POLLIN) == 0) {
      fail("raw protocol connection closed without a rejection response");
    }

    std::array<char, 16U * 1024U> buffer{};
    iovec part{buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    const auto received = ::recvmsg(descriptor, &message, MSG_DONTWAIT);
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (received < 0)
      fail(system_error("raw protocol receive failed"));
    require(received > 0,
            "raw protocol connection closed without a rejection response");
    require((message.msg_flags & MSG_TRUNC) == 0,
            "raw protocol rejection was unexpectedly truncated");
    return std::string(buffer.data(), static_cast<std::size_t>(received));
  }
}

void expect_invalid_request(const std::string &socket_path,
                            const std::string_view request,
                            const std::string_view expected_request_id,
                            const unsigned protocol_version) {
  const auto deadline = Clock::now() + kRawExchangeTimeout;
  const auto socket = connect_raw(socket_path, deadline);
  send_packet(socket.get(), request, deadline);
  const auto packet = receive_packet(socket.get(), deadline);

  nlohmann::json response;
  try {
    response = nlohmann::json::parse(packet);
  } catch (const std::exception &error) {
    fail("raw protocol rejection is not JSON: " + std::string(error.what()));
  }
  require(response.is_object() && response.size() == 5U,
          "raw protocol rejection has an invalid envelope");
  require(response.contains("version") &&
              response.at("version").is_number_unsigned() &&
              response.at("version").get<unsigned>() == protocol_version,
          "raw protocol rejection has an unexpected version");
  require(response.contains("request_id") &&
              response.at("request_id").is_string() &&
              response.at("request_id").get_ref<const std::string &>() ==
                  expected_request_id,
          "raw protocol rejection has an unexpected request_id");
  require(response.contains("ok") && response.at("ok").is_boolean() &&
              !response.at("ok").get<bool>(),
          "raw protocol rejection unexpectedly succeeded");
  require(response.contains("error_code") &&
              response.at("error_code").is_string() &&
              response.at("error_code").get_ref<const std::string &>() ==
                  "invalid_request",
          "raw protocol rejection has an unexpected error code");
  require(response.contains("close_connection") &&
              response.at("close_connection").is_boolean() &&
              !response.at("close_connection").get<bool>(),
          "raw protocol rejection has an invalid connection policy");
}

class SocketReplacementWatch final {
public:
  SocketReplacementWatch(const std::filesystem::path &directory,
                         std::string socket_name)
      : descriptor_(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK)),
        socket_name_(std::move(socket_name)) {
    if (descriptor_.get() < 0)
      fail(system_error("cannot create inotify instance"));
    watch_descriptor_ =
        ::inotify_add_watch(descriptor_.get(), directory.c_str(),
                            IN_CREATE | IN_DELETE | IN_Q_OVERFLOW);
    if (watch_descriptor_ < 0)
      fail(system_error("cannot watch socket directory"));
  }

  ~SocketReplacementWatch() {
    if (watch_descriptor_ >= 0) {
      static_cast<void>(
          ::inotify_rm_watch(descriptor_.get(), watch_descriptor_));
    }
  }

  SocketReplacementWatch(const SocketReplacementWatch &) = delete;
  SocketReplacementWatch &operator=(const SocketReplacementWatch &) = delete;

  void require_delete_then_create(
      const std::chrono::milliseconds timeout = kFilesystemTimeout) const {
    const auto deadline = Clock::now() + timeout;
    bool saw_delete = false;
    while (Clock::now() < deadline) {
      const short ready = wait_for_events(descriptor_.get(), POLLIN, deadline);
      require(ready != 0,
              "did not observe stale socket replacement before deadline");
      if ((ready & POLLIN) == 0)
        fail("socket directory watch failed during restart");

      alignas(inotify_event) std::array<char, 4096> buffer{};
      const auto count =
          ::read(descriptor_.get(), buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (count < 0)
        fail(system_error("cannot read socket directory events"));

      std::size_t offset = 0;
      const auto byte_count = static_cast<std::size_t>(count);
      while (offset + sizeof(inotify_event) <= byte_count) {
        const auto *const event =
            reinterpret_cast<const inotify_event *>(buffer.data() + offset);
        const auto event_size =
            sizeof(inotify_event) + static_cast<std::size_t>(event->len);
        require(event_size <= byte_count - offset,
                "truncated socket directory event");
        if ((event->mask & IN_Q_OVERFLOW) != 0)
          fail("socket directory event queue overflowed");
        const std::string_view name = event->len == 0U
                                          ? std::string_view{}
                                          : std::string_view{event->name};
        if (event->wd == watch_descriptor_ && name == socket_name_) {
          if ((event->mask & IN_DELETE) != 0)
            saw_delete = true;
          if ((event->mask & IN_CREATE) != 0 && saw_delete)
            return;
        }
        offset += event_size;
      }
    }
    fail("did not observe stale socket deletion followed by recreation");
  }

private:
  UniqueFd descriptor_;
  std::string socket_name_;
  int watch_descriptor_{-1};
};

void require_socket(const std::filesystem::path &socket_path,
                    const std::string_view context) {
  struct stat status{};
  if (::lstat(socket_path.c_str(), &status) != 0)
    fail(std::string(context) + ": " + system_error("lstat failed"));
  require(S_ISSOCK(status.st_mode),
          std::string(context) + ": path is not a Unix socket");
}

void require_socket_absent(const std::filesystem::path &socket_path) {
  const auto deadline = Clock::now() + kFilesystemTimeout;
  while (Clock::now() < deadline) {
    check_interrupted();
    struct stat status{};
    if (::lstat(socket_path.c_str(), &status) != 0) {
      if (errno == ENOENT)
        return;
      fail(system_error("cannot inspect stopped daemon socket"));
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  fail("daemon socket remains after graceful shutdown");
}

void require_clean_stop(ChildProcess &process,
                        const std::filesystem::path &socket_path) {
  process.send_signal(SIGTERM);
  const auto status = process.wait_for(kExitTimeout);
  if (!status)
    fail_with_log("daemon did not stop before the SIGTERM deadline", process);
  if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
    fail_with_log("daemon did not exit 0 after SIGTERM; observed " +
                      process_status_text(*status),
                  process);
  }
  require_socket_absent(socket_path);
}

[[nodiscard]] std::filesystem::path
resolve_hypervisor(const std::string_view argument) {
  require(!argument.empty(), "--hypervisor path is empty");
  std::error_code error;
  const auto resolved = std::filesystem::canonical(argument, error);
  require(!error, "cannot resolve --hypervisor: " + error.message());
  require(::access(resolved.c_str(), X_OK) == 0,
          system_error("--hypervisor is not executable"));
  return resolved;
}

void run_acceptance(const std::filesystem::path &hypervisor) {
  TemporaryDirectory temporary_directory;
  const auto socket_path = temporary_directory.path() / "hypervisor.sock";
  const auto storage_path = temporary_directory.path() / "storage";
  const uid_t current_uid = ::geteuid();
  const auto arguments = daemon_arguments(
      socket_path, storage_path, temporary_directory.path(), current_uid);

  auto first = ChildProcess::spawn(hypervisor, arguments,
                                   temporary_directory.path() / "first.log");
  const unsigned protocol_version =
      wait_for_healthy(*first, socket_path.string(), current_uid);
  require(protocol_version > 0U, "daemon reported protocol version zero");

  auto contender = ChildProcess::spawn(
      hypervisor, arguments, temporary_directory.path() / "contender.log");
  const auto contender_status = contender->wait_for(kLockRefusalTimeout);
  if (!contender_status) {
    fail_with_log("second daemon did not reject the held storage lock",
                  *contender);
  }
  if (!WIFEXITED(*contender_status) || WEXITSTATUS(*contender_status) != 4) {
    fail_with_log("second daemon did not fail with process-lock exit 4; "
                  "observed " +
                      process_status_text(*contender_status),
                  *contender);
  }
  require(require_fresh_health(socket_path.string(), current_uid) ==
              protocol_version,
          "first daemon became unhealthy after lock contention");

  expect_invalid_request(socket_path.string(), R"({"version":)",
                         "invalid-request", protocol_version);
  const unsigned unsupported_version = protocol_version + 1U;
  const std::string unsupported_request =
      std::string{"{\"version\":"} + std::to_string(unsupported_version) +
      ",\"method\":\"Handshake\",\"request_id\":\"unsupported-version\"," +
      "\"params\":{}}";
  expect_invalid_request(socket_path.string(), unsupported_request,
                         "unsupported-version", protocol_version);
  require(require_fresh_health(socket_path.string(), current_uid) ==
              protocol_version,
          "daemon was not healthy after malformed protocol traffic");

  first->send_signal(SIGKILL);
  const auto killed_status = first->wait_for(kExitTimeout);
  if (!killed_status)
    fail_with_log("first daemon did not die after SIGKILL", *first);
  require(WIFSIGNALED(*killed_status) && WTERMSIG(*killed_status) == SIGKILL,
          "first daemon did not report SIGKILL termination; observed " +
              process_status_text(*killed_status));
  require_socket(socket_path, "SIGKILL did not leave a stale socket");

  SocketReplacementWatch replacement_watch(temporary_directory.path(),
                                           socket_path.filename().string());
  auto restarted = ChildProcess::spawn(
      hypervisor, arguments, temporary_directory.path() / "restarted.log");
  require(wait_for_healthy(*restarted, socket_path.string(), current_uid) ==
              protocol_version,
          "restarted daemon reported a different protocol version");
  replacement_watch.require_delete_then_create();
  require_socket(socket_path, "restarted daemon socket is absent");
  require_clean_stop(*restarted, socket_path);

  auto final_restart = ChildProcess::spawn(
      hypervisor, arguments, temporary_directory.path() / "final-restart.log");
  require(wait_for_healthy(*final_restart, socket_path.string(), current_uid) ==
              protocol_version,
          "final restart reported a different protocol version");
  require_clean_stop(*final_restart, socket_path);

  std::cout << "PASS: hypervisor lifecycle/protocol acceptance scenarios "
               "completed\n";
}

} // namespace

int main(const int argc, char **const argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--hypervisor" ||
      std::string_view{argv[2]}.empty()) {
    std::cerr << "usage: hypervisor_lifecycle_test --hypervisor PATH\n";
    return 2;
  }

  try {
    install_signal_handlers();
    run_acceptance(resolve_hypervisor(argv[2]));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: hypervisor lifecycle/protocol acceptance: "
              << error.what() << '\n';
    return 1;
  }
}
