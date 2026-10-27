#include "src/client/scheduler_client.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using gravesentinel::ccm::CheckpointConfig;
using gravesentinel::ccm::ColdStartSpec;
using gravesentinel::ccm::ContainerId;
using gravesentinel::ccm::ContainerRegistration;
using gravesentinel::ccm::HealthState;
using gravesentinel::ccm::OperationId;
using gravesentinel::ccm::OperationState;
using gravesentinel::ccm::QueueState;
using gravesentinel::ccm::RequestId;
using gravesentinel::ccm::RpcFailure;
using gravesentinel::ccm::RpcFailureCode;
using gravesentinel::ccm::RuntimeContainerRef;
using gravesentinel::ccm::SchedulerClient;
using Clock = std::chrono::steady_clock;
using TaskAffinities = std::map<pid_t, std::string>;

constexpr std::string_view kLogicalIdLabel = "io.gravesentinel.ccm.logical-id";
constexpr auto kCommandTimeout = 45s;
constexpr auto kObservationTimeout = 60s;
constexpr auto kOperationTimeout = 10min;

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

void require(const bool condition, std::string message) {
  if (!condition)
    fail(std::move(message));
}

template <typename Integer>
[[nodiscard]] std::optional<Integer>
parse_integer(const std::string_view text) {
  Integer value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return std::nullopt;
  return value;
}

[[nodiscard]] std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  value.erase(0, first);
  return value;
}

struct Options {
  std::filesystem::path podman;
  std::filesystem::path gate_directory;
  std::string socket_path;
  std::string fixture_image;
  std::string run_id;
  pid_t daemon_pid{};
};

[[nodiscard]] Options parse_options(const int argc, char **const argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc)
      fail("missing value for " + std::string(argument));
    const std::string value{argv[++index]};
    if (argument == "--socket") {
      options.socket_path = value;
    } else if (argument == "--podman") {
      options.podman = value;
    } else if (argument == "--fixture-image") {
      options.fixture_image = value;
    } else if (argument == "--run-id") {
      options.run_id = value;
    } else if (argument == "--gate-directory") {
      options.gate_directory = value;
    } else if (argument == "--daemon-pid") {
      const auto parsed = parse_integer<long long>(value);
      if (!parsed || *parsed <= 0 ||
          *parsed > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
        fail("--daemon-pid requires a positive pid_t value");
      }
      options.daemon_pid = static_cast<pid_t>(*parsed);
    } else {
      fail("unknown argument: " + std::string(argument));
    }
  }
  require(!options.socket_path.empty(), "--socket is required");
  require(!options.podman.empty(), "--podman is required");
  require(!options.fixture_image.empty(), "--fixture-image is required");
  require(!options.run_id.empty(), "--run-id is required");
  require(!options.gate_directory.empty(), "--gate-directory is required");
  require(options.daemon_pid > 0, "--daemon-pid is required");
  require(options.run_id.size() <= 48U, "--run-id is too long");
  return options;
}

[[nodiscard]] std::string_view rpc_failure_name(const RpcFailureCode code) {
  switch (code) {
  case RpcFailureCode::invalid_argument:
    return "invalid_argument";
  case RpcFailureCode::unavailable:
    return "unavailable";
  case RpcFailureCode::deadline_exceeded:
    return "deadline_exceeded";
  case RpcFailureCode::permission_denied:
    return "permission_denied";
  case RpcFailureCode::protocol_error:
    return "protocol_error";
  case RpcFailureCode::server_busy:
    return "server_busy";
  case RpcFailureCode::request_failed:
    return "request_failed";
  case RpcFailureCode::internal_error:
    return "internal_error";
  }
  return "unknown";
}

[[nodiscard]] std::string rpc_failure_text(const RpcFailure &failure) {
  return std::string(rpc_failure_name(failure.code)) + ": " + failure.message;
}

[[nodiscard]] std::string_view
operation_state_name(const OperationState state) {
  switch (state) {
  case OperationState::queued:
    return "queued";
  case OperationState::running:
    return "running";
  case OperationState::succeeded:
    return "succeeded";
  case OperationState::failed:
    return "failed";
  case OperationState::indeterminate:
    return "indeterminate";
  }
  return "unknown";
}

struct CommandResult {
  int exit_code{};
  std::string standard_output;
  std::string standard_error;
};

void read_pipe(const int descriptor, std::string &output) {
  constexpr std::size_t maximum_captured_bytes = 8U * 1024U * 1024U;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0) {
      const auto byte_count = static_cast<std::size_t>(count);
      if (output.size() < maximum_captured_bytes) {
        const auto retained =
            std::min(byte_count, maximum_captured_bytes - output.size());
        output.append(buffer.data(), retained);
      }
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  static_cast<void>(::close(descriptor));
}

[[nodiscard]] CommandResult
run_command(const std::filesystem::path &executable,
            const std::span<const std::string> arguments,
            const std::chrono::milliseconds timeout = kCommandTimeout) {
  int output_pipe[2]{};
  int error_pipe[2]{};
  if (::pipe(output_pipe) != 0)
    fail("cannot create subprocess stdout pipe");
  if (::pipe(error_pipe) != 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    fail("cannot create subprocess stderr pipe");
  }

  const pid_t child = ::fork();
  if (child < 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(output_pipe[1]));
    static_cast<void>(::close(error_pipe[0]));
    static_cast<void>(::close(error_pipe[1]));
    fail("cannot fork subprocess");
  }
  if (child == 0) {
    static_cast<void>(::close(output_pipe[0]));
    static_cast<void>(::close(error_pipe[0]));
    if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(error_pipe[1], STDERR_FILENO) < 0) {
      _exit(126);
    }
    static_cast<void>(::close(output_pipe[1]));
    static_cast<void>(::close(error_pipe[1]));

    std::vector<char *> argv;
    const std::string executable_text = executable.string();
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char *>(executable_text.c_str()));
    for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(executable_text.c_str(), argv.data());
    _exit(127);
  }

  static_cast<void>(::close(output_pipe[1]));
  static_cast<void>(::close(error_pipe[1]));
  CommandResult result;
  std::thread output_reader(read_pipe, output_pipe[0],
                            std::ref(result.standard_output));
  std::thread error_reader(read_pipe, error_pipe[0],
                           std::ref(result.standard_error));

  int status{};
  const auto deadline = Clock::now() + timeout;
  bool timed_out = false;
  while (true) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child)
      break;
    if (waited < 0 && errno != EINTR)
      fail("cannot wait for subprocess");
    if (Clock::now() >= deadline) {
      timed_out = true;
      static_cast<void>(::kill(child, SIGKILL));
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    std::this_thread::sleep_for(25ms);
  }
  output_reader.join();
  error_reader.join();

  if (timed_out) {
    result.exit_code = 124;
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  } else {
    result.exit_code = 125;
  }
  return result;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream input{path};
  if (!input.is_open())
    fail("cannot open file: " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad())
    fail("cannot read file: " + path.string());
  return contents.str();
}

[[nodiscard]] std::string read_status_field(const std::filesystem::path &path,
                                            const std::string_view field) {
  std::ifstream input{path};
  if (!input.is_open())
    fail("cannot open proc status: " + path.string());
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(field))
      return trim(line.substr(field.size()));
  }
  fail("missing " + std::string(field) + " in " + path.string());
}

[[nodiscard]] pid_t parse_pid_text(const std::string_view text,
                                   const std::string_view purpose) {
  const auto parsed = parse_integer<long long>(text);
  if (!parsed || *parsed <= 0 ||
      *parsed > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
    fail("invalid " + std::string(purpose) + ": " + std::string(text));
  }
  return static_cast<pid_t>(*parsed);
}

[[nodiscard]] TaskAffinities snapshot_task_affinities(const pid_t process_id) {
  const auto task_root =
      std::filesystem::path{"/proc"} / std::to_string(process_id) / "task";
  std::error_code error;
  require(std::filesystem::is_directory(task_root, error) && !error,
          "process task directory is unavailable: " + task_root.string());

  TaskAffinities affinities;
  for (const auto &entry : std::filesystem::directory_iterator(task_root)) {
    const auto name = entry.path().filename().string();
    const auto parsed = parse_integer<long long>(name);
    if (!parsed || *parsed <= 0 ||
        *parsed > static_cast<long long>(std::numeric_limits<pid_t>::max())) {
      continue;
    }
    const auto task_id = static_cast<pid_t>(*parsed);
    affinities.emplace(task_id, read_status_field(entry.path() / "status",
                                                  "Cpus_allowed_list:"));
  }
  require(!affinities.empty(),
          "process has no observable tasks: " + std::to_string(process_id));
  return affinities;
}

[[nodiscard]] std::string process_affinity(const pid_t process_id) {
  return read_status_field(std::filesystem::path{"/proc"} /
                               std::to_string(process_id) / "status",
                           "Cpus_allowed_list:");
}

[[nodiscard]] pid_t process_parent(const pid_t process_id) {
  const auto parent = read_status_field(
      std::filesystem::path{"/proc"} / std::to_string(process_id) / "status",
      "PPid:");
  return parse_pid_text(parent, "process parent PID");
}

[[nodiscard]] std::string process_cgroup(const pid_t process_id) {
  std::istringstream lines{read_text_file(
      std::filesystem::path{"/proc"} / std::to_string(process_id) / "cgroup")};
  std::string line;
  while (std::getline(lines, line)) {
    if (line.starts_with("0::")) {
      auto path = line.substr(3U);
      require(!path.empty() && path.front() == '/',
              "container cgroup path is not absolute");
      const std::filesystem::path parsed{path};
      for (const auto &component : parsed) {
        require(component != "..", "container cgroup path contains '..'");
      }
      return path;
    }
  }
  fail("unified cgroup entry is missing for PID " + std::to_string(process_id));
}

[[nodiscard]] std::filesystem::path
cgroup_filesystem_path(const std::string_view cgroup_path) {
  require(!cgroup_path.empty() && cgroup_path.front() == '/',
          "unsafe cgroup path");
  return std::filesystem::path{"/sys/fs/cgroup" + std::string(cgroup_path)};
}

void wait_for_cgroup_absence(const std::string_view cgroup_path) {
  const auto path = cgroup_filesystem_path(cgroup_path);
  const auto deadline = Clock::now() + 30s;
  std::error_code error;
  while (Clock::now() < deadline) {
    if (!std::filesystem::exists(path, error) && !error)
      return;
    error.clear();
    std::this_thread::sleep_for(100ms);
  }
  fail("removed container cgroup remains: " + path.string());
}

struct FixtureSample {
  std::uint64_t counter{};
  std::uint64_t cpu_mask{};
  std::uint64_t file_records{};
  std::uint64_t file_counter{};
  std::string nonce;
  std::string instance_tag;
};

[[nodiscard]] std::optional<FixtureSample>
parse_fixture_sample(const std::string_view line) {
  FixtureSample sample;
  bool has_counter = false;
  bool has_cpu_mask = false;
  bool has_file_records = false;
  bool has_file_counter = false;
  bool has_nonce = false;
  bool has_tag = false;
  std::istringstream fields{std::string(line)};
  std::string field;
  while (fields >> field) {
    const auto separator = field.find('=');
    if (separator == std::string::npos)
      continue;
    const std::string_view key{field.data(), separator};
    const std::string_view value{field.data() + separator + 1U,
                                 field.size() - separator - 1U};
    if (key == "counter") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed)
        return std::nullopt;
      sample.counter = *parsed;
      has_counter = true;
    } else if (key == "cpu_mask") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed)
        return std::nullopt;
      sample.cpu_mask = *parsed;
      has_cpu_mask = true;
    } else if (key == "file_records") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed)
        return std::nullopt;
      sample.file_records = *parsed;
      has_file_records = true;
    } else if (key == "file_counter") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed)
        return std::nullopt;
      sample.file_counter = *parsed;
      has_file_counter = true;
    } else if (key == "nonce") {
      sample.nonce = value;
      has_nonce = !sample.nonce.empty();
    } else if (key == "instance_tag") {
      sample.instance_tag = value;
      has_tag = !sample.instance_tag.empty();
    }
  }
  if (!has_counter || !has_cpu_mask || !has_file_records || !has_file_counter ||
      !has_nonce || !has_tag) {
    return std::nullopt;
  }
  return sample;
}

struct ContainerObservation {
  std::string runtime_id;
  pid_t process_id{};
  std::string cgroup_path;
};

class PodmanOracle final {
public:
  explicit PodmanOracle(std::filesystem::path executable)
      : executable_(std::move(executable)) {}

  [[nodiscard]] std::string container_id(const std::string_view name) const {
    return inspect(name, "{{.Id}}");
  }

  [[nodiscard]] std::string owner(const std::string_view name) const {
    return inspect(
        name, "{{index .Config.Labels \"io.gravesentinel.ccm.logical-id\"}}");
  }

  [[nodiscard]] bool running(const std::string_view name) const {
    return inspect(name, "{{.State.Running}}") == "true";
  }

  [[nodiscard]] std::string cpuset(const std::string_view name) const {
    return inspect(name, "{{.HostConfig.CpusetCpus}}");
  }

  [[nodiscard]] pid_t process_id(const std::string_view name) const {
    return parse_pid_text(inspect(name, "{{.State.Pid}}"), "container PID");
  }

  void require_single_instance(const std::string_view logical_id) const {
    const std::string filter = std::string("label=") +
                               std::string(kLogicalIdLabel) + "=" +
                               std::string(logical_id);
    const auto ordinary =
        checked({"ps", "--all", "--filter", filter, "--format", "{{.ID}}"});
    const auto external = checked({"ps", "--all", "--external", "--filter",
                                   filter, "--format", "{{.ID}}"});
    std::set<std::string> runtime_ids;
    collect_nonempty_lines(runtime_ids, ordinary);
    collect_nonempty_lines(runtime_ids, external);
    require(runtime_ids.size() == 1U,
            "expected exactly one runtime instance for " +
                std::string(logical_id) + ", ordinary:\n" + ordinary +
                "external:\n" + external);
  }

  [[nodiscard]] FixtureSample latest_sample(const std::string_view name) const {
    const auto logs = command({"logs", std::string(name)}, kCommandTimeout);
    if (logs.exit_code != 0) {
      fail("podman logs failed for " + std::string(name) + ": " +
           logs.standard_error);
    }
    const std::string combined =
        logs.standard_output + "\n" + logs.standard_error;
    std::istringstream lines{combined};
    std::string line;
    std::optional<FixtureSample> latest;
    while (std::getline(lines, line)) {
      if (auto sample = parse_fixture_sample(line); sample)
        latest = std::move(*sample);
    }
    if (!latest)
      fail("fixture emitted no complete sample for " + std::string(name));
    return *latest;
  }

  template <typename Predicate>
  [[nodiscard]] FixtureSample wait_for_sample(
      const std::string_view name, Predicate predicate,
      const std::chrono::milliseconds timeout = kObservationTimeout) const {
    const auto deadline = Clock::now() + timeout;
    std::optional<FixtureSample> latest;
    while (Clock::now() < deadline) {
      try {
        latest = latest_sample(name);
        if (predicate(*latest))
          return *latest;
      } catch (const std::runtime_error &) {
      }
      std::this_thread::sleep_for(200ms);
    }
    fail("timed out waiting for fixture sample from " + std::string(name) +
         (latest ? "; last counter=" + std::to_string(latest->counter) : ""));
  }

  [[nodiscard]] ContainerObservation
  observe(const std::string_view name) const {
    const auto pid = process_id(name);
    return ContainerObservation{
        .runtime_id = container_id(name),
        .process_id = pid,
        .cgroup_path = process_cgroup(pid),
    };
  }

  [[nodiscard]] std::string
  stop_and_remove(const std::string_view name,
                  const std::string_view expected_runtime_id,
                  const std::string_view expected_owner) const {
    require(owner(name) == expected_owner,
            "refusing to remove container with unexpected owner: " +
                std::string(name));
    require(container_id(name) == expected_runtime_id,
            "refusing to remove a replaced runtime incarnation: " +
                std::string(name));
    const auto pid = process_id(name);
    const auto cgroup_path = process_cgroup(pid);

    const auto stopped = command(
        {"stop", "--time", "10", std::string(expected_runtime_id)}, 45s);
    require(stopped.exit_code == 0, "podman stop failed for " +
                                        std::string(name) + ": " +
                                        stopped.standard_error);
    const auto removed = command({"rm", std::string(expected_runtime_id)}, 45s);
    require(removed.exit_code == 0, "podman rm failed for " +
                                        std::string(name) + ": " +
                                        removed.standard_error);

    const auto deadline = Clock::now() + 30s;
    while (Clock::now() < deadline) {
      if (absent(name, expected_owner))
        return cgroup_path;
      std::this_thread::sleep_for(100ms);
    }
    fail("container absence was not proven after controlled removal: " +
         std::string(name));
  }

private:
  static void collect_nonempty_lines(std::set<std::string> &values,
                                     const std::string_view text) {
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
      line = trim(std::move(line));
      if (!line.empty())
        values.insert(std::move(line));
    }
  }

  [[nodiscard]] bool absent(const std::string_view name,
                            const std::string_view logical_id) const {
    const auto exists =
        command({"container", "exists", std::string(name)}, 10s);
    if (exists.exit_code != 1)
      return false;
    const std::string filter = std::string("label=") +
                               std::string(kLogicalIdLabel) + "=" +
                               std::string(logical_id);
    const auto ordinary = command(
        {"ps", "--all", "--filter", filter, "--format", "{{.ID}}"}, 10s);
    const auto external = command({"ps", "--all", "--external", "--filter",
                                   filter, "--format", "{{.ID}}"},
                                  10s);
    return ordinary.exit_code == 0 && external.exit_code == 0 &&
           trim(ordinary.standard_output).empty() &&
           trim(external.standard_output).empty();
  }

  [[nodiscard]] CommandResult
  command(std::vector<std::string> arguments,
          const std::chrono::milliseconds timeout) const {
    return run_command(executable_, arguments, timeout);
  }

  [[nodiscard]] std::string
  checked(std::vector<std::string> arguments,
          const std::chrono::milliseconds timeout = kCommandTimeout) const {
    const auto result = command(std::move(arguments), timeout);
    if (result.exit_code != 0) {
      fail("podman command failed with exit " +
           std::to_string(result.exit_code) + ": " + result.standard_error);
    }
    return result.standard_output;
  }

  [[nodiscard]] std::string inspect(const std::string_view name,
                                    const std::string_view format) const {
    return trim(checked(
        {"inspect", "--format", std::string(format), std::string(name)}));
  }

  std::filesystem::path executable_;
};

template <typename Identifier>
[[nodiscard]] Identifier make_identifier(std::string value,
                                         const std::string_view purpose) {
  auto identifier = Identifier::create(std::move(value));
  if (!identifier) {
    fail("invalid " + std::string(purpose) + ": " +
         identifier.error().diagnostic);
  }
  return std::move(*identifier);
}

class SchedulerHarness final {
public:
  SchedulerHarness(std::string socket_path, const uid_t server_uid,
                   std::string request_prefix)
      : client_(std::move(socket_path), 3s, kOperationTimeout, server_uid),
        request_prefix_(std::move(request_prefix)) {}

  void connect() {
    const auto deadline = Clock::now() + 20s;
    std::string last_failure = "daemon did not accept a connection";
    while (Clock::now() < deadline) {
      const auto connected = client_.connect();
      if (!connected) {
        last_failure = rpc_failure_text(connected.error());
        std::this_thread::sleep_for(100ms);
        continue;
      }
      try {
        require_healthy();
        return;
      } catch (const std::runtime_error &error) {
        last_failure = error.what();
        std::this_thread::sleep_for(100ms);
      }
    }
    fail("scheduler client handshake failed: " + last_failure);
  }

  void require_healthy() {
    const auto health = client_.get_health();
    if (!health)
      fail("GetHealth failed: " + rpc_failure_text(health.error()));
    require(health->state == HealthState::healthy,
            "hypervisor health is not healthy");
    require(health->queue == QueueState::available,
            "hypervisor operation queue is saturated");
  }

  void register_container(const ContainerRegistration &registration) {
    require(client_.register_container(registration, next_request("register")),
            "Register failed for " + registration.container_id.value());
  }

  [[nodiscard]] bool
  try_register_container(const ContainerRegistration &registration) {
    return client_.register_container(registration,
                                      next_request("register-fifth"));
  }

  void unregister_container(const ContainerId &container_id) {
    const auto deadline = Clock::now() + 30s;
    while (Clock::now() < deadline) {
      if (client_.unregister_container(container_id,
                                       next_request("unregister"))) {
        return;
      }
      std::this_thread::sleep_for(100ms);
    }
    fail("Unregister did not reach quiescence for " + container_id.value());
  }

  [[nodiscard]] OperationId
  submit_create_checkpoint(const ContainerId &container_id) {
    auto operation = client_.submit_create_checkpoint(
        container_id, next_request("create-checkpoint"));
    if (!operation) {
      fail("SubmitCreateCheckpoint failed for " + container_id.value() + ": " +
           rpc_failure_text(operation.error()));
    }
    return std::move(*operation);
  }

  [[nodiscard]] OperationId
  submit_restore_or_restart(const ContainerId &container_id) {
    auto operation = client_.submit_restore_or_restart(
        container_id, next_request("restore-or-restart"));
    if (!operation) {
      fail("SubmitRestoreOrRestart failed for " + container_id.value() + ": " +
           rpc_failure_text(operation.error()));
    }
    return std::move(*operation);
  }

  void require_all_running(const std::span<const OperationId> operations) {
    for (const auto &operation : operations) {
      const auto status = get_operation(operation);
      require(status.state == OperationState::running,
              "operation is not concurrently running: " + operation.value() +
                  " state=" + std::string(operation_state_name(status.state)));
    }
  }

  void wait_all_success(const std::span<const OperationId> operations) {
    std::vector<bool> completed(operations.size(), false);
    std::size_t completed_count = 0;
    const auto deadline = Clock::now() + kOperationTimeout;
    while (Clock::now() < deadline) {
      for (std::size_t index = 0; index < operations.size(); ++index) {
        if (completed[index])
          continue;
        const auto status = get_operation(operations[index]);
        if (status.state == OperationState::succeeded) {
          completed[index] = true;
          ++completed_count;
          continue;
        }
        if (status.state == OperationState::failed ||
            status.state == OperationState::indeterminate) {
          fail("operation " + operations[index].value() + " ended in " +
               std::string(operation_state_name(status.state)) +
               (status.error
                    ? " with stable error " +
                          std::to_string(static_cast<unsigned>(*status.error))
                    : ""));
        }
      }
      if (completed_count == operations.size())
        return;
      std::this_thread::sleep_for(100ms);
    }
    fail("timed out waiting for asynchronous operations");
  }

private:
  [[nodiscard]] gravesentinel::ccm::OperationStatus
  get_operation(const OperationId &operation_id) {
    auto status = client_.get_operation(operation_id);
    if (!status) {
      fail("GetOperation failed for " + operation_id.value() + ": " +
           rpc_failure_text(status.error()));
    }
    return std::move(*status);
  }

  [[nodiscard]] RequestId next_request(const std::string_view operation) {
    ++sequence_;
    return make_identifier<RequestId>(request_prefix_ + "-" +
                                          std::string(operation) + "-" +
                                          std::to_string(sequence_),
                                      "request ID");
  }

  SchedulerClient client_;
  std::string request_prefix_;
  std::uint64_t sequence_{};
};

struct Scenario {
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  std::string instance_tag;
  std::uint32_t container_cpu{};
  std::uint32_t checkpoint_cpu{};
};

[[nodiscard]] Scenario make_named_scenario(const Options &options,
                                           const std::string_view suffix,
                                           const std::uint32_t container_cpu,
                                           const std::uint32_t checkpoint_cpu) {
  const std::string logical_id = options.run_id + "-" + std::string(suffix);
  return Scenario{
      .container_id = make_identifier<ContainerId>(logical_id, "container ID"),
      .runtime_container =
          make_identifier<RuntimeContainerRef>(logical_id, "runtime name"),
      .instance_tag = std::string(suffix),
      .container_cpu = container_cpu,
      .checkpoint_cpu = checkpoint_cpu,
  };
}

[[nodiscard]] Scenario make_scenario(const Options &options,
                                     const std::uint32_t container_cpu,
                                     const std::uint32_t checkpoint_cpu) {
  return make_named_scenario(options, "cpu" + std::to_string(container_cpu),
                             container_cpu, checkpoint_cpu);
}

[[nodiscard]] std::array<Scenario, 4> make_scenarios(const Options &options) {
  return {
      make_scenario(options, 0U, 1U),
      make_scenario(options, 2U, 3U),
      make_scenario(options, 4U, 5U),
      make_scenario(options, 6U, 7U),
  };
}

[[nodiscard]] ContainerRegistration
make_registration(const Options &options, const Scenario &scenario) {
  return ContainerRegistration{
      .container_id = scenario.container_id,
      .runtime_container = scenario.runtime_container,
      .checkpoint =
          CheckpointConfig{
              .interval = std::chrono::hours{24},
              .keep_last = 3,
              .container_cpus =
                  gravesentinel::ccm::CpuSet::create({scenario.container_cpu})
                      .value(),
              .checkpoint_cpu = scenario.checkpoint_cpu,
          },
      .cold_start =
          ColdStartSpec{
              .image_reference = options.fixture_image,
              .argv =
                  {
                      "--threads",
                      "2",
                      "--interval-ms",
                      "250",
                      "--state-file",
                      "/dev/shm/gravesentinel-four-cpu.state",
                      "--instance-tag",
                      scenario.instance_tag,
                  },
              .environment = {},
              .mounts = {},
              .runtime_options = {},
          },
      .runtime_fingerprint = std::nullopt,
  };
}

void register_all(const Options &options,
                  const std::span<const Scenario> scenarios,
                  SchedulerHarness &scheduler) {
  for (const auto &scenario : scenarios)
    scheduler.register_container(make_registration(options, scenario));
}

void unregister_all(const std::span<const Scenario> scenarios,
                    SchedulerHarness &scheduler) {
  for (const auto &scenario : scenarios)
    scheduler.unregister_container(scenario.container_id);
}

[[nodiscard]] std::vector<OperationId>
submit_cold_starts(const std::span<const Scenario> scenarios,
                   SchedulerHarness &scheduler) {
  std::vector<OperationId> operations;
  operations.reserve(scenarios.size());
  for (const auto &scenario : scenarios) {
    operations.push_back(
        scheduler.submit_restore_or_restart(scenario.container_id));
  }
  return operations;
}

[[nodiscard]] std::vector<OperationId>
submit_checkpoints(const std::span<const Scenario> scenarios,
                   SchedulerHarness &scheduler) {
  std::vector<OperationId> operations;
  operations.reserve(scenarios.size());
  for (const auto &scenario : scenarios)
    operations.push_back(
        scheduler.submit_create_checkpoint(scenario.container_id));
  return operations;
}

[[nodiscard]] std::set<pid_t> require_daemon_workers(
    const pid_t daemon_pid, const TaskAffinities &baseline,
    const std::optional<std::set<pid_t>> &expected_workers = std::nullopt) {
  const auto current = snapshot_task_affinities(daemon_pid);
  for (const auto &[task_id, affinity] : baseline) {
    const auto found = current.find(task_id);
    require(found != current.end(),
            "baseline hypervisor task disappeared: " + std::to_string(task_id));
    require(found->second == affinity,
            "baseline hypervisor task affinity changed: tid=" +
                std::to_string(task_id) + " before=" + affinity +
                " after=" + found->second);
  }

  std::set<pid_t> workers;
  std::multiset<std::string> worker_affinities;
  for (const auto &[task_id, affinity] : current) {
    if (!baseline.contains(task_id)) {
      workers.insert(task_id);
      worker_affinities.insert(affinity);
    }
  }
  require(workers.size() == 4U,
          "expected exactly four persistent checkpoint worker tasks, found " +
              std::to_string(workers.size()));
  const std::multiset<std::string> expected_affinities{"1", "3", "5", "7"};
  require(worker_affinities == expected_affinities,
          "checkpoint worker affinity set is not exactly 1/3/5/7");
  if (expected_workers) {
    require(workers == *expected_workers,
            "persistent checkpoint worker task identities changed");
  }
  return workers;
}

void require_no_daemon_workers(const pid_t daemon_pid,
                               const TaskAffinities &baseline) {
  const auto current = snapshot_task_affinities(daemon_pid);
  require(current == baseline,
          "hypervisor task set or baseline affinity was not restored after "
          "unregister");
}

void require_driver_affinity(const std::string_view expected) {
  const auto observed = process_affinity(::getpid());
  require(observed == expected,
          "driver/SchedulerClient affinity changed: before=" +
              std::string(expected) + " after=" + observed);
}

[[nodiscard]] ContainerObservation
require_container_affinity(const Scenario &scenario,
                           const PodmanOracle &podman) {
  const auto name = scenario.runtime_container.value();
  require(podman.owner(name) == scenario.container_id.value(),
          "container has the wrong logical ID label: " + name);
  require(podman.running(name), "container is not running: " + name);
  const auto expected_cpu = std::to_string(scenario.container_cpu);
  require(podman.cpuset(name) == expected_cpu,
          "Podman HostConfig.CpusetCpus mismatch for " + name);

  const auto observation = podman.observe(name);
  const auto tasks = snapshot_task_affinities(observation.process_id);
  require(tasks.size() >= 3U,
          "fixture does not expose its main thread and two worker threads: " +
              name);
  for (const auto &[task_id, affinity] : tasks) {
    require(affinity == expected_cpu,
            "container task escaped assigned CPU: container=" + name +
                " tid=" + std::to_string(task_id) + " allowed=" + affinity +
                " expected=" + expected_cpu);
  }

  const auto cpuset_path =
      cgroup_filesystem_path(observation.cgroup_path) / "cpuset.cpus.effective";
  const auto effective = trim(read_text_file(cpuset_path));
  require(effective == expected_cpu,
          "cgroup cpuset.cpus.effective mismatch for " + name + ": " +
              effective + " expected=" + expected_cpu);
  podman.require_single_instance(scenario.container_id.value());
  return observation;
}

void require_fixture_cpu(const Scenario &scenario,
                         const FixtureSample &sample) {
  const auto expected_mask = std::uint64_t{1}
                             << static_cast<unsigned>(scenario.container_cpu);
  require(sample.cpu_mask == expected_mask,
          "fixture CPU mask mismatch for " + scenario.container_id.value() +
              ": observed=" + std::to_string(sample.cpu_mask) +
              " expected=" + std::to_string(expected_mask));
}

[[nodiscard]] std::map<std::string, pid_t>
wait_for_checkpoint_gate(const Options &options,
                         const std::span<const Scenario> scenarios) {
  std::map<std::string, pid_t> process_ids;
  const auto deadline = Clock::now() + 2min;
  while (Clock::now() < deadline && process_ids.size() < scenarios.size()) {
    for (const auto &scenario : scenarios) {
      const auto &name = scenario.runtime_container.value();
      if (process_ids.contains(name))
        continue;
      const auto marker =
          options.gate_directory / ("checkpoint." + name + ".ready");
      std::error_code error;
      if (!std::filesystem::is_regular_file(marker, error) || error)
        continue;
      const auto process_id =
          parse_pid_text(trim(read_text_file(marker)), "Podman gate PID");
      require(process_parent(process_id) == options.daemon_pid,
              "Podman checkpoint gate is not a direct hypervisor child: " +
                  std::to_string(process_id));
      const auto tasks = snapshot_task_affinities(process_id);
      require(tasks.size() == 1U,
              "Podman checkpoint gate unexpectedly has multiple tasks");
      require(tasks.begin()->second == std::to_string(scenario.checkpoint_cpu),
              "Podman checkpoint gate affinity mismatch for " + name);
      process_ids.emplace(name, process_id);
    }
    if (process_ids.size() < scenarios.size())
      std::this_thread::sleep_for(20ms);
  }
  require(
      process_ids.size() == scenarios.size(),
      "not all four Podman checkpoint commands reached the concurrency gate");
  return process_ids;
}

void release_checkpoint_gate(const Options &options) {
  const auto release = options.gate_directory / "release";
  std::ofstream output{release, std::ios::out | std::ios::trunc};
  require(output.is_open(), "cannot create checkpoint gate release file");
  output << "release\n";
  output.close();
  require(!output.fail(), "cannot publish checkpoint gate release file");
}

struct SavedState {
  FixtureSample sample;
  std::string runtime_id;
  std::string cgroup_path;
};

void run_acceptance(const Options &options) {
  require(::kill(options.daemon_pid, 0) == 0,
          "hypervisor PID is not alive at driver startup");
  const std::string initial_driver_affinity = process_affinity(::getpid());

  SchedulerHarness scheduler{options.socket_path, ::geteuid(), options.run_id};
  scheduler.connect();
  scheduler.require_healthy();
  const auto daemon_baseline = snapshot_task_affinities(options.daemon_pid);
  const auto scenarios = make_scenarios(options);
  const PodmanOracle podman{options.podman};

  std::cout << "SCENARIO: register four standard CPU pairs and reject fifth\n";
  register_all(options, scenarios, scheduler);
  require_driver_affinity(initial_driver_affinity);

  const Scenario fifth = make_named_scenario(options, "fifth", 0U, 1U);
  require(!scheduler.try_register_container(make_registration(options, fifth)),
          "fifth registration was unexpectedly accepted");
  scheduler.unregister_container(scenarios.front().container_id);
  scheduler.register_container(make_registration(options, fifth));
  scheduler.unregister_container(fifth.container_id);
  scheduler.register_container(make_registration(options, scenarios.front()));
  scheduler.require_healthy();
  auto worker_tasks =
      require_daemon_workers(options.daemon_pid, daemon_baseline);
  std::cout
      << "PASS: fifth valid registration rejected at capacity and accepted "
         "after a slot was released\n";

  std::cout << "SCENARIO: submit four asynchronous cold starts\n";
  const auto cold_starts = submit_cold_starts(scenarios, scheduler);
  scheduler.wait_all_success(cold_starts);

  std::array<SavedState, 4> saved{};
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto &scenario = scenarios[index];
    const auto sample = podman.wait_for_sample(
        scenario.runtime_container.value(),
        [&](const FixtureSample &candidate) {
          return candidate.instance_tag == scenario.instance_tag &&
                 candidate.file_records >= 3U && candidate.file_counter > 0U;
        });
    require_fixture_cpu(scenario, sample);
    const auto observation = require_container_affinity(scenario, podman);
    saved[index] = SavedState{
        .sample = sample,
        .runtime_id = observation.runtime_id,
        .cgroup_path = observation.cgroup_path,
    };
  }
  static_cast<void>(require_daemon_workers(options.daemon_pid, daemon_baseline,
                                           worker_tasks));
  require_driver_affinity(initial_driver_affinity);
  std::cout << "PASS: four cold starts preserve container CPU affinity\n";

  std::cout << "SCENARIO: four concurrent checkpoint operations\n";
  std::error_code release_error;
  require(!std::filesystem::exists(options.gate_directory / "release",
                                   release_error) &&
              !release_error,
          "checkpoint gate release file exists before submissions");
  const auto checkpoints = submit_checkpoints(scenarios, scheduler);
  const auto gate_processes = wait_for_checkpoint_gate(options, scenarios);
  require(gate_processes.size() == scenarios.size(),
          "checkpoint gate did not retain four processes");
  scheduler.require_all_running(checkpoints);
  static_cast<void>(require_daemon_workers(options.daemon_pid, daemon_baseline,
                                           worker_tasks));
  require_driver_affinity(initial_driver_affinity);
  std::cout << "PASS: all four checkpoint operations concurrently running; "
               "Podman children pinned to CPU1/3/5/7\n";
  release_checkpoint_gate(options);
  scheduler.wait_all_success(checkpoints);

  for (const auto &scenario : scenarios) {
    const auto current =
        podman.latest_sample(scenario.runtime_container.value());
    const auto progressed = podman.wait_for_sample(
        scenario.runtime_container.value(),
        [&](const FixtureSample &candidate) {
          return candidate.nonce == current.nonce &&
                 candidate.counter >= current.counter + 4U &&
                 candidate.file_records > current.file_records;
        });
    require_fixture_cpu(scenario, progressed);
    static_cast<void>(require_container_affinity(scenario, podman));
  }
  static_cast<void>(require_daemon_workers(options.daemon_pid, daemon_baseline,
                                           worker_tasks));
  require_driver_affinity(initial_driver_affinity);
  std::cout << "PASS: four checkpoint operations succeeded and sources kept "
               "running\n";

  std::cout << "SCENARIO: controlled source stop/remove and four restores\n";
  unregister_all(scenarios, scheduler);
  require_no_daemon_workers(options.daemon_pid, daemon_baseline);
  require_driver_affinity(initial_driver_affinity);

  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto &scenario = scenarios[index];
    const auto removed_cgroup = podman.stop_and_remove(
        scenario.runtime_container.value(), saved[index].runtime_id,
        scenario.container_id.value());
    require(removed_cgroup == saved[index].cgroup_path,
            "source cgroup changed before controlled removal");
    wait_for_cgroup_absence(removed_cgroup);
  }

  register_all(options, scenarios, scheduler);
  worker_tasks = require_daemon_workers(options.daemon_pid, daemon_baseline);
  const auto restores = submit_cold_starts(scenarios, scheduler);
  scheduler.wait_all_success(restores);

  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto &scenario = scenarios[index];
    const auto restored = podman.wait_for_sample(
        scenario.runtime_container.value(),
        [&](const FixtureSample &candidate) {
          return candidate.nonce == saved[index].sample.nonce &&
                 candidate.instance_tag == scenario.instance_tag &&
                 candidate.counter >= saved[index].sample.counter &&
                 candidate.file_records >= saved[index].sample.file_records;
        });
    require_fixture_cpu(scenario, restored);
    const auto observation = require_container_affinity(scenario, podman);
    require(observation.runtime_id != saved[index].runtime_id,
            "restore did not create a new runtime incarnation for " +
                scenario.container_id.value());

    const auto progressed = podman.wait_for_sample(
        scenario.runtime_container.value(),
        [&](const FixtureSample &candidate) {
          return candidate.nonce == restored.nonce &&
                 candidate.counter >= restored.counter + 4U &&
                 candidate.file_records > restored.file_records;
        });
    require_fixture_cpu(scenario, progressed);
    static_cast<void>(require_container_affinity(scenario, podman));
  }
  static_cast<void>(require_daemon_workers(options.daemon_pid, daemon_baseline,
                                           worker_tasks));
  require_driver_affinity(initial_driver_affinity);
  std::cout << "PASS: all four containers restored with state continuity, new "
               "runtime IDs, singleton ownership, and CPU affinity\n";

  unregister_all(scenarios, scheduler);
  require_no_daemon_workers(options.daemon_pid, daemon_baseline);
  require_driver_affinity(initial_driver_affinity);
  scheduler.require_healthy();
  std::cout << "PASS: hypervisor baseline tasks and SchedulerClient affinity "
               "remained unchanged\n";
}

} // namespace

int main(const int argc, char **const argv) {
  try {
    const auto options = parse_options(argc, argv);
    run_acceptance(options);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: four-container CPU acceptance driver: " << error.what()
              << '\n';
    return 1;
  }
}
