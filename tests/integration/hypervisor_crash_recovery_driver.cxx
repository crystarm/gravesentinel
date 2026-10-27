#include "src/client/scheduler_client.hxx"
#include "src/manifest/codec.hxx"

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
using gravesentinel::ccm::ChunkHash;
using gravesentinel::ccm::ColdStartSpec;
using gravesentinel::ccm::ContainerId;
using gravesentinel::ccm::ContainerRegistration;
using gravesentinel::ccm::HealthState;
using gravesentinel::ccm::ManifestV2;
using gravesentinel::ccm::OperationId;
using gravesentinel::ccm::OperationState;
using gravesentinel::ccm::OperationStatus;
using gravesentinel::ccm::QueueState;
using gravesentinel::ccm::RequestId;
using gravesentinel::ccm::RpcFailure;
using gravesentinel::ccm::RpcFailureCode;
using gravesentinel::ccm::RuntimeContainerRef;
using gravesentinel::ccm::SchedulerClient;
using gravesentinel::ccm::StableErrorCode;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kLogicalIdLabel = "io.gravesentinel.ccm.logical-id";
constexpr std::string_view kConflictLabel =
    "io.gravesentinel.ccm.crash-recovery-conflict";
constexpr std::string_view kInstanceTag = "stage13";
constexpr auto kCommandTimeout = 45s;
constexpr auto kObservationTimeout = 60s;
constexpr auto kOperationTimeout = 10min;

struct Options {
  std::string phase;
  std::string socket_path;
  std::filesystem::path storage_root;
  std::filesystem::path state_directory;
  std::filesystem::path podman;
  std::string fixture_image;
  std::string run_id;
  std::string case_name;
  std::string request_id;
  std::string fault_mode;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

void require(const bool condition, std::string message) {
  if (!condition) {
    fail(std::move(message));
  }
}

[[nodiscard]] bool safe_token(const std::string_view value) {
  return !value.empty() && std::ranges::all_of(value, [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_' || character == '.';
  });
}

[[nodiscard]] Options parse_options(const int argc, char **const argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc) {
      fail("missing value for " + std::string(argument));
    }
    const std::string value{argv[++index]};
    if (argument == "--phase") {
      options.phase = value;
    } else if (argument == "--socket") {
      options.socket_path = value;
    } else if (argument == "--storage") {
      options.storage_root = value;
    } else if (argument == "--state") {
      options.state_directory = value;
    } else if (argument == "--podman") {
      options.podman = value;
    } else if (argument == "--fixture-image") {
      options.fixture_image = value;
    } else if (argument == "--run-id") {
      options.run_id = value;
    } else if (argument == "--case") {
      options.case_name = value;
    } else if (argument == "--request-id") {
      options.request_id = value;
    } else if (argument == "--fault-mode") {
      options.fault_mode = value;
    } else {
      fail("unknown argument: " + std::string(argument));
    }
  }

  require(!options.phase.empty(), "--phase is required");
  require(!options.socket_path.empty(), "--socket is required");
  require(!options.storage_root.empty(), "--storage is required");
  require(!options.state_directory.empty(), "--state is required");
  require(!options.podman.empty(), "--podman is required");
  require(!options.fixture_image.empty(), "--fixture-image is required");
  require(safe_token(options.run_id), "--run-id is unsafe");
  require(options.run_id.size() <= 48U, "--run-id is too long");
  if (!options.case_name.empty()) {
    require(safe_token(options.case_name), "--case is unsafe");
  }
  return options;
}

[[nodiscard]] std::string trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  value.erase(0, first);
  return value;
}

template <typename Integer>
[[nodiscard]] std::optional<Integer>
parse_integer(const std::string_view text) {
  Integer value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
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

[[nodiscard]] std::string_view stable_error_name(const StableErrorCode code) {
  switch (code) {
  case StableErrorCode::invalid_argument:
    return "invalid_argument";
  case StableErrorCode::not_found:
    return "not_found";
  case StableErrorCode::already_exists:
    return "already_exists";
  case StableErrorCode::resource_exhausted:
    return "resource_exhausted";
  case StableErrorCode::conflict:
    return "conflict";
  case StableErrorCode::permission_denied:
    return "permission_denied";
  case StableErrorCode::capture_failed:
    return "capture_failed";
  case StableErrorCode::storage_error:
    return "storage_error";
  case StableErrorCode::corrupted_snapshot:
    return "corrupted_snapshot";
  case StableErrorCode::incompatible_runtime:
    return "incompatible_runtime";
  case StableErrorCode::restore_failed:
    return "restore_failed";
  case StableErrorCode::cold_start_failed:
    return "cold_start_failed";
  case StableErrorCode::deadline_exceeded:
    return "deadline_exceeded";
  case StableErrorCode::unavailable:
    return "unavailable";
  case StableErrorCode::internal:
    return "internal";
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
    if (count < 0 && errno == EINTR) {
      continue;
    }
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
  if (::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0) {
    fail("cannot create subprocess pipes");
  }

  const pid_t child = ::fork();
  if (child < 0) {
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

    const std::string executable_text = executable.string();
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char *>(executable_text.c_str()));
    for (const auto &argument : arguments) {
      argv.push_back(const_cast<char *>(argument.c_str()));
    }
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
  bool timed_out = false;
  const auto deadline = Clock::now() + timeout;
  while (true) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      fail("cannot wait for subprocess");
    }
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
    if (separator == std::string::npos) {
      continue;
    }
    const std::string_view key{field.data(), separator};
    const std::string_view value{field.data() + separator + 1U,
                                 field.size() - separator - 1U};
    if (key == "counter") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed) {
        return std::nullopt;
      }
      sample.counter = *parsed;
      has_counter = true;
    } else if (key == "cpu_mask") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed) {
        return std::nullopt;
      }
      sample.cpu_mask = *parsed;
      has_cpu_mask = true;
    } else if (key == "file_records") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed) {
        return std::nullopt;
      }
      sample.file_records = *parsed;
      has_file_records = true;
    } else if (key == "file_counter") {
      const auto parsed = parse_integer<std::uint64_t>(value);
      if (!parsed) {
        return std::nullopt;
      }
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

class StateStore final {
public:
  explicit StateStore(std::filesystem::path directory)
      : directory_(std::move(directory)) {
    require(std::filesystem::is_directory(directory_),
            "persistent state directory is unavailable");
  }

  void write(const std::string_view name, const std::string_view value) const {
    require(safe_token(name), "unsafe persistent state key");
    const auto path = directory_ / std::string(name);
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
      fail("cannot write persistent state: " + path.string());
    }
    output << value << '\n';
    output.close();
    if (!output) {
      fail("cannot close persistent state: " + path.string());
    }
  }

  [[nodiscard]] std::string read(const std::string_view name) const {
    require(safe_token(name), "unsafe persistent state key");
    const auto path = directory_ / std::string(name);
    std::ifstream input(path);
    if (!input) {
      fail("cannot read persistent state: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
      fail("cannot consume persistent state: " + path.string());
    }
    return trim(contents.str());
  }

  void append_cgroup(const std::string_view path) const {
    require(!path.empty() && path.starts_with('/') &&
                path.find('\n') == path.npos,
            "invalid cgroup path");
    std::ofstream output(directory_ / "cgroups", std::ios::out | std::ios::app);
    if (!output) {
      fail("cannot append persistent cgroup state");
    }
    output << path << '\n';
    output.close();
    if (!output) {
      fail("cannot close persistent cgroup state");
    }
  }

  void write_baseline(const FixtureSample &sample) const {
    std::ostringstream encoded;
    encoded << "counter=" << sample.counter << " nonce=" << sample.nonce
            << " cpu_mask=" << sample.cpu_mask
            << " file_records=" << sample.file_records
            << " file_counter=" << sample.file_counter
            << " instance_tag=" << sample.instance_tag;
    write("baseline", encoded.str());
  }

  [[nodiscard]] FixtureSample baseline() const {
    auto parsed = parse_fixture_sample(read("baseline"));
    require(parsed.has_value(), "persistent fixture baseline is malformed");
    return std::move(*parsed);
  }

private:
  std::filesystem::path directory_;
};

class PodmanOracle final {
public:
  PodmanOracle(std::filesystem::path executable, std::string logical_id,
               const StateStore &state)
      : executable_(std::move(executable)), logical_id_(std::move(logical_id)),
        state_(state) {}

  [[nodiscard]] std::set<std::string> runtime_ids() const {
    const std::string filter = std::string("label=") +
                               std::string(kLogicalIdLabel) + "=" + logical_id_;
    const auto ordinary =
        checked({"ps", "--all", "--filter", filter, "--format", "{{.ID}}"});
    const auto external = checked({"ps", "--all", "--external", "--filter",
                                   filter, "--format", "{{.ID}}"});
    std::set<std::string> ids;
    collect_lines(ids, ordinary);
    collect_lines(ids, external);
    return ids;
  }

  void require_at_most_one() const {
    const auto ids = runtime_ids();
    require(ids.size() <= 1U,
            "more than one ordinary/external runtime exists for " +
                logical_id_);
  }

  [[nodiscard]] std::string require_exactly_one() const {
    const auto ids = runtime_ids();
    require(ids.size() == 1U,
            "expected exactly one ordinary/external runtime for " +
                logical_id_);
    require(owner(logical_id_) == logical_id_,
            "runtime logical ownership label does not match");
    record_cgroup(logical_id_);
    return *ids.begin();
  }

  void require_absent() const {
    require(runtime_ids().empty(),
            "runtime is present when operator reconciliation requires absence");
    const auto exists = command({"container", "exists", logical_id_});
    require(exists.exit_code == 1,
            "target name absence was not proven; status=" +
                std::to_string(exists.exit_code));
  }

  [[nodiscard]] bool running(const std::string_view name) const {
    return inspect(name, "{{.State.Running}}") == "true";
  }

  [[nodiscard]] std::string runtime_id(const std::string_view name) const {
    return inspect(name, "{{.Id}}");
  }

  [[nodiscard]] std::string owner(const std::string_view name) const {
    return inspect(
        name, "{{index .Config.Labels \"io.gravesentinel.ccm.logical-id\"}}");
  }

  [[nodiscard]] std::string label(const std::string_view name,
                                  const std::string_view key) const {
    return inspect(name,
                   "{{index .Config.Labels \"" + std::string(key) + "\"}}");
  }

  [[nodiscard]] FixtureSample latest_sample(const std::string_view name) const {
    const auto logs = command({"logs", std::string(name)});
    if (logs.exit_code != 0) {
      fail("podman logs failed: " + logs.standard_error);
    }
    std::istringstream lines{logs.standard_output + "\n" + logs.standard_error};
    std::string line;
    std::optional<FixtureSample> latest;
    while (std::getline(lines, line)) {
      if (auto sample = parse_fixture_sample(line); sample) {
        latest = std::move(*sample);
      }
    }
    if (!latest) {
      fail("fixture emitted no complete sample");
    }
    return *latest;
  }

  template <typename Predicate>
  [[nodiscard]] FixtureSample wait_for_sample(Predicate predicate) const {
    const auto deadline = Clock::now() + kObservationTimeout;
    std::optional<FixtureSample> latest;
    while (Clock::now() < deadline) {
      try {
        latest = latest_sample(logical_id_);
        if (predicate(*latest)) {
          return *latest;
        }
      } catch (const std::runtime_error &) {
      }
      std::this_thread::sleep_for(200ms);
    }
    fail("timed out waiting for fixture continuity" +
         (latest ? "; last counter=" + std::to_string(latest->counter) : ""));
  }

  void remove_owned(const bool require_conflict_label) const {
    static_cast<void>(require_exactly_one());
    require(owner(logical_id_) == logical_id_,
            "refusing to remove a container with mismatched ownership");
    if (require_conflict_label) {
      require(label(logical_id_, kConflictLabel) == logical_id_,
              "refusing to remove a container without the test conflict label");
    }
    const auto removed = command({"rm", "--force", logical_id_}, 90s);
    if (removed.exit_code != 0) {
      fail("owner-checked operator removal failed: " + removed.standard_error);
    }
    require_absent();
  }

private:
  static void collect_lines(std::set<std::string> &values,
                            const std::string_view text) {
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
      line = trim(std::move(line));
      if (!line.empty()) {
        values.insert(std::move(line));
      }
    }
  }

  void record_cgroup(const std::string_view name) const {
    const auto pid_text = inspect(name, "{{.State.Pid}}");
    const auto process_id = parse_integer<pid_t>(pid_text);
    if (!process_id || *process_id <= 0) {
      return;
    }
    std::ifstream input("/proc/" + std::to_string(*process_id) + "/cgroup");
    std::string line;
    while (std::getline(input, line)) {
      const auto first = line.find(':');
      const auto second = first == std::string::npos
                              ? std::string::npos
                              : line.find(':', first + 1U);
      if (first == 1U && line[0] == '0' && second != std::string::npos) {
        const auto path = line.substr(second + 1U);
        if (!path.empty() && path.starts_with('/')) {
          state_.append_cgroup(path);
        }
        return;
      }
    }
  }

  [[nodiscard]] CommandResult
  command(std::vector<std::string> arguments,
          const std::chrono::milliseconds timeout = kCommandTimeout) const {
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
  std::string logical_id_;
  const StateStore &state_;
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
  explicit SchedulerHarness(const Options &options)
      : client_(options.socket_path, 3s, kOperationTimeout, ::geteuid()) {}

  void connect() {
    const auto deadline = Clock::now() + 20s;
    std::string last_failure = "daemon did not accept a connection";
    while (Clock::now() < deadline) {
      auto connected = client_.connect();
      if (!connected) {
        last_failure = rpc_failure_text(connected.error());
        std::this_thread::sleep_for(100ms);
        continue;
      }
      auto health = client_.get_health();
      if (!health) {
        last_failure = rpc_failure_text(health.error());
        std::this_thread::sleep_for(100ms);
        continue;
      }
      require(health->state == HealthState::healthy,
              "hypervisor health is not healthy");
      require(health->queue == QueueState::available,
              "hypervisor queue is saturated");
      return;
    }
    fail("SchedulerClient handshake failed: " + last_failure);
  }

  void register_container(const ContainerRegistration &registration,
                          const RequestId &request_id) {
    require(client_.register_container(registration, request_id),
            "Register failed for " + registration.container_id.value());
  }

  void unregister_container(const ContainerId &container_id,
                            const RequestId &request_id) {
    require(client_.unregister_container(container_id, request_id),
            "Unregister failed for " + container_id.value());
  }

  [[nodiscard]] OperationId submit_create(const ContainerId &container_id,
                                          const RequestId &request_id) {
    auto submitted = client_.submit_create_checkpoint(container_id, request_id);
    if (!submitted) {
      fail("SubmitCreateCheckpoint failed: " +
           rpc_failure_text(submitted.error()));
    }
    return std::move(*submitted);
  }

  [[nodiscard]] OperationId submit_restore(const ContainerId &container_id,
                                           const RequestId &request_id) {
    auto submitted =
        client_.submit_restore_or_restart(container_id, request_id);
    if (!submitted) {
      fail("SubmitRestoreOrRestart failed: " +
           rpc_failure_text(submitted.error()));
    }
    return std::move(*submitted);
  }

  [[nodiscard]] OperationStatus get(const OperationId &operation_id) {
    auto status = client_.get_operation(operation_id);
    if (!status) {
      fail("GetOperation failed for " + operation_id.value() + ": " +
           rpc_failure_text(status.error()));
    }
    return std::move(*status);
  }

  [[nodiscard]] OperationStatus wait_terminal(const OperationId &operation_id) {
    const auto deadline = Clock::now() + kOperationTimeout;
    while (Clock::now() < deadline) {
      auto status = get(operation_id);
      if (status.state == OperationState::succeeded ||
          status.state == OperationState::failed ||
          status.state == OperationState::indeterminate) {
        return status;
      }
      std::this_thread::sleep_for(100ms);
    }
    fail("operation timed out: " + operation_id.value());
  }

  void wait_success(const OperationId &operation_id) {
    const auto status = wait_terminal(operation_id);
    require(status.state == OperationState::succeeded,
            "operation " + operation_id.value() + " ended in " +
                std::string(operation_state_name(status.state)) +
                (status.error
                     ? " with " + std::string(stable_error_name(*status.error))
                     : ""));
  }

private:
  SchedulerClient client_;
};

[[nodiscard]] std::vector<std::byte>
read_binary_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    fail("cannot open file: " + path.string());
  }
  const auto end = input.tellg();
  if (end < 0) {
    fail("cannot determine file size: " + path.string());
  }
  const auto size = static_cast<std::uintmax_t>(end);
  require(size <= static_cast<std::uintmax_t>(
                      std::numeric_limits<std::size_t>::max()),
          "file is too large: " + path.string());
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) {
    fail("cannot read file: " + path.string());
  }
  return bytes;
}

[[nodiscard]] std::string hash_to_hex(const ChunkHash &hash) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string text;
  text.reserve(ChunkHash::digest_size * 2U);
  for (const auto byte : hash.bytes()) {
    const auto value = std::to_integer<unsigned>(byte);
    text.push_back(digits[(value >> 4U) & 0x0fU]);
    text.push_back(digits[value & 0x0fU]);
  }
  return text;
}

[[nodiscard]] bool canonical_manifest_name(const std::string_view name) {
  constexpr std::size_t digits = 20U;
  constexpr std::string_view suffix = ".json";
  return name.size() == digits + suffix.size() && name.ends_with(suffix) &&
         std::ranges::all_of(name.substr(0, digits), [](const char character) {
           return character >= '0' && character <= '9';
         });
}

struct StoredGeneration {
  std::filesystem::path path;
  ManifestV2 manifest;
};

class StorageProbe final {
public:
  StorageProbe(std::filesystem::path checkpoint_root, std::string container_id)
      : checkpoint_root_(std::move(checkpoint_root)),
        container_id_(std::move(container_id)) {}

  [[nodiscard]] std::vector<StoredGeneration> generations() const {
    const auto directory = container_directory();
    std::vector<StoredGeneration> stored;
    for (const auto &entry :
         std::filesystem::directory_iterator(directory / "manifests")) {
      require(entry.is_regular_file(),
              "manifest root contains a non-regular entry");
      const auto name = entry.path().filename().string();
      require(canonical_manifest_name(name),
              "manifest root contains a non-canonical entry: " + name);
      auto bytes = read_binary_file(entry.path());
      auto decoded = gravesentinel::ccm::decode_manifest_v2(bytes);
      if (!decoded) {
        fail("committed manifest is not valid canonical Manifest v1: " +
             decoded.error().diagnostic);
      }
      require(decoded->container_id.value() == container_id_,
              "committed manifest has the wrong logical container ID");
      const auto parsed_generation =
          parse_integer<std::uint64_t>(std::string_view(name).substr(0, 20U));
      require(parsed_generation.has_value() &&
                  *parsed_generation == decoded->generation,
              "committed manifest filename and generation disagree");
      verify_chunks(directory, *decoded);
      stored.push_back(StoredGeneration{entry.path(), std::move(*decoded)});
    }
    std::ranges::sort(stored, {}, [](const StoredGeneration &entry) {
      return entry.manifest.generation;
    });
    for (std::size_t index = 1; index < stored.size(); ++index) {
      require(stored[index - 1U].manifest.generation <
                  stored[index].manifest.generation,
              "committed manifest generations are duplicated");
    }
    return stored;
  }

  void require_recovered_clean() const {
    const auto directory = container_directory();
    const auto staging = directory / "staging";
    require(std::filesystem::is_directory(staging),
            "storage staging directory is missing");
    require(std::filesystem::directory_iterator(staging) ==
                std::filesystem::directory_iterator{},
            "startup recovery left an incomplete operation staging entry");
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(directory)) {
      const auto name = entry.path().filename().string();
      require(!name.starts_with(".tmp-"),
              "startup recovery left a publication temporary: " +
                  entry.path().string());
    }
    static_cast<void>(generations());
  }

  void require_no_orphan_chunks() const {
    const auto directory = container_directory();
    const auto stored = generations();
    std::set<std::string> referenced;
    for (const auto &generation : stored) {
      for (const auto &artifact : generation.manifest.artifacts) {
        for (const auto &chunk : artifact.chunks) {
          referenced.insert(hash_to_hex(chunk.hash));
        }
      }
    }

    const auto chunks = directory / "chunks";
    require(std::filesystem::is_directory(chunks),
            "storage chunk directory is missing");
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(chunks)) {
      if (entry.is_directory()) {
        continue;
      }
      require(entry.is_regular_file(),
              "chunk store contains a non-regular entry: " +
                  entry.path().string());
      const auto hash = entry.path().filename().string();
      require(referenced.contains(hash),
              "startup garbage collection left an orphan chunk: " + hash);
    }
  }

private:
  void verify_chunks(const std::filesystem::path &directory,
                     const ManifestV2 &manifest) const {
    for (const auto &artifact : manifest.artifacts) {
      for (const auto &chunk : artifact.chunks) {
        const auto hash = hash_to_hex(chunk.hash);
        const auto path = directory / "chunks" / hash.substr(0, 2) / hash;
        require(std::filesystem::is_regular_file(path),
                "committed manifest references a missing chunk: " + hash);
        require(std::filesystem::file_size(path) == chunk.size,
                "committed manifest references a chunk with the wrong size: " +
                    hash);
      }
    }
  }

  [[nodiscard]] std::filesystem::path container_directory() const {
    const auto containers = checkpoint_root_ / "containers";
    for (const auto &entry : std::filesystem::directory_iterator(containers)) {
      if (!entry.is_directory()) {
        continue;
      }
      const auto identity = read_binary_file(entry.path() / "identity");
      const std::string text(reinterpret_cast<const char *>(identity.data()),
                             identity.size());
      if (text == container_id_) {
        return entry.path();
      }
    }
    fail("storage namespace not found for " + container_id_);
  }

  std::filesystem::path checkpoint_root_;
  std::string container_id_;
};

[[nodiscard]] std::string
encode_generations(const std::span<const StoredGeneration> stored) {
  std::ostringstream output;
  for (std::size_t index = 0; index < stored.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << stored[index].manifest.generation;
  }
  return output.str();
}

[[nodiscard]] std::vector<std::uint64_t>
decode_generations(const std::string_view encoded) {
  std::vector<std::uint64_t> generations;
  if (encoded.empty()) {
    return generations;
  }
  std::size_t offset = 0;
  while (offset < encoded.size()) {
    const auto separator = encoded.find(',', offset);
    const auto part = encoded.substr(
        offset, separator == encoded.npos ? encoded.npos : separator - offset);
    const auto parsed = parse_integer<std::uint64_t>(part);
    require(parsed.has_value() && *parsed != 0U,
            "persistent generation set is malformed");
    generations.push_back(*parsed);
    if (separator == encoded.npos) {
      break;
    }
    offset = separator + 1U;
  }
  return generations;
}

[[nodiscard]] std::vector<std::uint64_t>
generation_numbers(const std::span<const StoredGeneration> stored) {
  std::vector<std::uint64_t> values;
  values.reserve(stored.size());
  for (const auto &entry : stored) {
    values.push_back(entry.manifest.generation);
  }
  return values;
}

struct Context {
  explicit Context(const Options &options)
      : logical_id(options.run_id + "-recovery"),
        container_id(make_identifier<ContainerId>(logical_id, "container ID")),
        runtime_container(make_identifier<RuntimeContainerRef>(
            logical_id, "runtime container reference")),
        state(options.state_directory),
        podman(options.podman, logical_id, state),
        storage(options.storage_root / "checkpoint-data", logical_id) {}

  std::string logical_id;
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  StateStore state;
  PodmanOracle podman;
  StorageProbe storage;
};

[[nodiscard]] ContainerRegistration make_registration(const Options &options,
                                                      const Context &context) {
  return ContainerRegistration{
      .container_id = context.container_id,
      .runtime_container = context.runtime_container,
      .checkpoint =
          CheckpointConfig{
              .interval = std::chrono::hours{24},
              .keep_last = 3,
              .container_cpus =
                  gravesentinel::ccm::CpuSet::create({0U}).value(),
              .checkpoint_cpu = 1,
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
                      "/dev/shm/gravesentinel-stage13.state",
                      "--instance-tag",
                      std::string(kInstanceTag),
                  },
              .environment = {},
              .mounts = {},
              .runtime_options = {},
          },
      .runtime_fingerprint = std::nullopt,
  };
}

[[nodiscard]] RequestId request(const std::string_view value) {
  return make_identifier<RequestId>(std::string(value), "request ID");
}

[[nodiscard]] OperationId stored_operation(const Options &options,
                                           const Context &context) {
  require(!options.case_name.empty(), "--case is required for operation state");
  return make_identifier<OperationId>(
      context.state.read("operation-" + options.case_name), "operation ID");
}

void write_operation(const Options &options, const Context &context,
                     const OperationId &operation_id) {
  require(!options.case_name.empty(), "--case is required for operation state");
  context.state.write("operation-" + options.case_name, operation_id.value());
}

void require_indeterminate(SchedulerHarness &scheduler,
                           const OperationId &operation_id) {
  const auto status = scheduler.get(operation_id);
  require(status.state == OperationState::indeterminate,
          "recovered running operation is not indeterminate: " +
              std::string(operation_state_name(status.state)));
  require(status.error == StableErrorCode::unavailable,
          "indeterminate operation does not expose stable unavailable error");
}

[[nodiscard]] FixtureSample require_live_continuity(const Context &context) {
  const auto baseline = context.state.baseline();
  static_cast<void>(context.podman.require_exactly_one());
  require(context.podman.running(context.logical_id),
          "the sole logical runtime is not running");
  const auto sample =
      context.podman.wait_for_sample([&](const FixtureSample &candidate) {
        return candidate.nonce == baseline.nonce &&
               candidate.instance_tag == kInstanceTag &&
               candidate.file_records >= baseline.file_records &&
               candidate.file_counter >= baseline.file_counter;
      });
  require(sample.cpu_mask != 0U && (sample.cpu_mask & ~std::uint64_t{1}) == 0U,
          "fixture escaped logical CPU 0");
  return sample;
}

void phase_initialize(const Options &options, Context &context) {
  std::cout << "PHASE: initialize CPU0/1 fixture and three baselines\n";
  SchedulerHarness scheduler(options);
  scheduler.connect();
  scheduler.register_container(
      make_registration(options, context),
      request(options.run_id + "-register-initialize"));
  const auto started = scheduler.submit_restore(
      context.container_id, request(options.run_id + "-initialize-start"));
  scheduler.wait_success(started);

  const auto initial =
      context.podman.wait_for_sample([](const FixtureSample &sample) {
        return sample.instance_tag == kInstanceTag && sample.file_records >= 3U;
      });
  require(initial.cpu_mask != 0U &&
              (initial.cpu_mask & ~std::uint64_t{1}) == 0U,
          "fixture escaped logical CPU 0 during initialization");
  const auto original_runtime = context.podman.runtime_id(context.logical_id);
  FixtureSample baseline = initial;

  for (std::uint64_t generation = 1; generation <= 3U; ++generation) {
    const auto before = context.podman.latest_sample(context.logical_id);
    if (generation == 3U) {
      baseline = before;
    }
    const auto operation = scheduler.submit_create(
        context.container_id, request(options.run_id + "-baseline-create-" +
                                      std::to_string(generation)));
    scheduler.wait_success(operation);
    const auto stored = context.storage.generations();
    require(stored.size() == generation &&
                stored.back().manifest.generation == generation,
            "baseline checkpoint generation set is unexpected");
    require(context.podman.runtime_id(context.logical_id) == original_runtime,
            "leave-running baseline checkpoint replaced the runtime");
    const auto progressed =
        context.podman.wait_for_sample([&](const FixtureSample &sample) {
          return sample.nonce == initial.nonce &&
                 sample.counter >= before.counter + 4U &&
                 sample.file_records > before.file_records;
        });
    require(progressed.cpu_mask != 0U &&
                (progressed.cpu_mask & ~std::uint64_t{1}) == 0U,
            "baseline runtime escaped logical CPU 0");
  }

  context.state.write_baseline(baseline);
  context.storage.require_recovered_clean();
  static_cast<void>(require_live_continuity(context));
  std::cout << "PASS: committed baseline generations=1,2,3 nonce="
            << baseline.nonce << " with leave-running continuity\n";
}

void phase_register(const Options &options, Context &context) {
  require(!options.case_name.empty(), "register phase requires --case");
  SchedulerHarness scheduler(options);
  scheduler.connect();
  scheduler.register_container(
      make_registration(options, context),
      request(options.run_id + "-register-" + options.case_name));
  context.podman.require_at_most_one();
  if (!context.podman.runtime_ids().empty()) {
    require(context.podman.owner(context.logical_id) == context.logical_id,
            "registered runtime has the wrong logical ownership label");
  }
  std::cout << "PASS: re-registered logical runtime for " << options.case_name
            << '\n';
}

void phase_submit_create_crash(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty(),
          "submit-create-crash requires --case and --request-id");
  static_cast<void>(require_live_continuity(context));
  context.storage.require_recovered_clean();
  const auto before = context.storage.generations();
  context.state.write("generations-" + options.case_name,
                      encode_generations(before));

  SchedulerHarness scheduler(options);
  scheduler.connect();
  const auto operation = scheduler.submit_create(context.container_id,
                                                 request(options.request_id));
  write_operation(options, context, operation);
  std::cout << "ACCEPTED: create request=" << options.request_id
            << " operation=" << operation.value()
            << "; submitting client now exits\n";
}

void phase_reconcile_create_crash(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty() &&
              !options.fault_mode.empty(),
          "reconcile-create-crash requires case, request and fault mode");
  SchedulerHarness scheduler(options);
  scheduler.connect();
  const auto operation = stored_operation(options, context);
  require_indeterminate(scheduler, operation);
  const auto duplicate = scheduler.submit_create(context.container_id,
                                                 request(options.request_id));
  require(duplicate == operation,
          "duplicate create request returned a different OperationId");
  require_indeterminate(scheduler, operation);

  const auto before = decode_generations(
      context.state.read("generations-" + options.case_name));
  const auto after = context.storage.generations();
  const auto after_numbers = generation_numbers(after);
  if (options.fault_mode == "manifest-after-rename-block") {
    require(!before.empty(), "after-rename case has no prior generations");
    auto expected = before;
    expected.push_back(before.back() + 1U);
    require(after_numbers == expected,
            "after-rename crash did not expose exactly the durable generation");
  } else {
    require(options.fault_mode == "chunk-write-block" ||
                options.fault_mode == "manifest-before-rename-block",
            "unsupported create crash fault mode");
    require(after_numbers == before,
            "pre-publication crash changed the committed generation set");
  }
  context.storage.require_recovered_clean();
  const auto runtime_before_duplicate =
      context.podman.runtime_id(context.logical_id);
  static_cast<void>(require_live_continuity(context));
  std::this_thread::sleep_for(1s);
  require(generation_numbers(context.storage.generations()) == after_numbers,
          "duplicate create request executed after recovery");
  require(context.podman.runtime_id(context.logical_id) ==
              runtime_before_duplicate,
          "duplicate create request changed the runtime");

  context.podman.remove_owned(false);
  const auto reconciled = scheduler.submit_restore(
      context.container_id,
      request(options.request_id + "-operator-reconcile"));
  scheduler.wait_success(reconciled);
  static_cast<void>(require_live_continuity(context));
  context.podman.require_at_most_one();
  std::cout << "PASS: create crash mode=" << options.fault_mode
            << " remained indeterminate, duplicate was inert, and explicit "
               "operator restore preserved continuity\n";
}

void phase_prepare_restore_crash(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty(),
          "prepare-restore-crash requires --case and --request-id");
  static_cast<void>(require_live_continuity(context));

  SchedulerHarness scheduler(options);
  scheduler.connect();
  const auto prepared = scheduler.submit_create(
      context.container_id,
      request(options.request_id + "-source-incarnation"));
  scheduler.wait_success(prepared);
  context.storage.require_recovered_clean();
  require(context.storage.generations().size() <= 3U,
          "restore crash preparation violated retention");
  const auto source_runtime = context.podman.runtime_id(context.logical_id);
  context.state.write("source-runtime-" + options.case_name, source_runtime);

  const auto operation = scheduler.submit_restore(context.container_id,
                                                  request(options.request_id));
  write_operation(options, context, operation);
  std::cout << "ACCEPTED: restore request=" << options.request_id
            << " operation=" << operation.value()
            << "; submitting client now exits\n";
}

void reconcile_restore_identity(const Options &options, Context &context,
                                SchedulerHarness &scheduler) {
  const auto operation = stored_operation(options, context);
  require_indeterminate(scheduler, operation);
  const auto duplicate = scheduler.submit_restore(context.container_id,
                                                  request(options.request_id));
  require(duplicate == operation,
          "duplicate restore request returned a different OperationId");
  require_indeterminate(scheduler, operation);
}

void phase_reconcile_restore_before(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty(),
          "restore-before reconciliation requires case and request");
  SchedulerHarness scheduler(options);
  scheduler.connect();
  reconcile_restore_identity(options, context, scheduler);
  context.podman.require_absent();
  context.storage.require_recovered_clean();
  std::this_thread::sleep_for(1s);
  context.podman.require_absent();

  const auto reconciled = scheduler.submit_restore(
      context.container_id,
      request(options.request_id + "-operator-reconcile"));
  scheduler.wait_success(reconciled);
  static_cast<void>(require_live_continuity(context));
  context.podman.require_at_most_one();
  std::cout
      << "PASS: before-restore crash had no runtime, duplicate was inert, "
         "and a new request restored explicitly\n";
}

void phase_reconcile_restore_after(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty(),
          "restore-after reconciliation requires case and request");
  SchedulerHarness scheduler(options);
  scheduler.connect();
  reconcile_restore_identity(options, context, scheduler);
  const auto source = context.state.read("source-runtime-" + options.case_name);
  const auto restored = context.podman.runtime_id(context.logical_id);
  require(restored != source,
          "after-restore crash did not leave a new runtime incarnation");
  static_cast<void>(require_live_continuity(context));
  std::this_thread::sleep_for(1s);
  require(context.podman.runtime_id(context.logical_id) == restored,
          "indeterminate restore was retried automatically");
  context.podman.require_at_most_one();
  context.storage.require_recovered_clean();
  std::cout << "PASS: after-restore crash remained indeterminate with one "
               "continuous runtime and no automatic retry\n";
}

void phase_reconcile_restore_conflict(const Options &options,
                                      Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty(),
          "restore-conflict reconciliation requires case and request");
  SchedulerHarness scheduler(options);
  scheduler.connect();
  reconcile_restore_identity(options, context, scheduler);
  const auto conflict = context.podman.require_exactly_one();
  require(context.podman.label(context.logical_id, kConflictLabel) ==
              context.logical_id,
          "test-owned conflict marker is missing");
  std::this_thread::sleep_for(1s);
  require(context.podman.require_exactly_one() == conflict,
          "duplicate restore created another conflict/runtime");

  context.podman.remove_owned(true);
  const auto reconciled = scheduler.submit_restore(
      context.container_id,
      request(options.request_id + "-operator-reconcile"));
  scheduler.wait_success(reconciled);
  static_cast<void>(require_live_continuity(context));
  context.podman.require_at_most_one();
  context.storage.require_recovered_clean();
  std::cout << "PASS: conflicting target remained singular until owner-checked "
               "cleanup and explicit reconciliation restore\n";
}

void phase_storage_fault(const Options &options, Context &context) {
  require(!options.case_name.empty() && !options.request_id.empty() &&
              !options.fault_mode.empty(),
          "storage-fault requires case, request and fault mode");
  static_cast<void>(require_live_continuity(context));
  context.storage.require_recovered_clean();
  const auto before = context.storage.generations();
  const auto before_numbers = generation_numbers(before);
  require(!before.empty(),
          "storage fault test requires a published checkpoint");

  SchedulerHarness scheduler(options);
  scheduler.connect();
  const auto operation = scheduler.submit_create(context.container_id,
                                                 request(options.request_id));
  const auto status = scheduler.wait_terminal(operation);
  if (options.fault_mode == "chunk-enospc") {
    require(status.state == OperationState::failed &&
                status.error == StableErrorCode::resource_exhausted,
            "chunk ENOSPC did not produce failed/resource_exhausted");
  } else if (options.fault_mode == "chunk-eacces") {
    require(status.state == OperationState::failed &&
                status.error == StableErrorCode::permission_denied,
            "chunk EACCES did not produce failed/permission_denied");
  } else if (options.fault_mode == "chunk-eintr-once") {
    require(status.state == OperationState::succeeded && !status.error,
            "single chunk EINTR was not retried to success");
  } else {
    fail("unsupported controlled storage fault mode");
  }

  context.storage.require_recovered_clean();
  const auto after = context.storage.generations();
  if (options.fault_mode == "chunk-eintr-once") {
    require(after.size() <= 3U && after.back().manifest.generation ==
                                      before.back().manifest.generation + 1U,
            "successful EINTR retry did not publish one retained generation");
    const auto restored = scheduler.submit_restore(
        context.container_id,
        request(options.request_id + "-verify-published-restore"));
    scheduler.wait_success(restored);
  } else {
    require(generation_numbers(after) == before_numbers,
            "controlled storage failure changed published generations");
    context.podman.remove_owned(false);
    const auto restored = scheduler.submit_restore(
        context.container_id,
        request(options.request_id + "-restore-previous"));
    scheduler.wait_success(restored);
  }
  static_cast<void>(require_live_continuity(context));
  context.podman.require_at_most_one();
  require(context.storage.generations().size() <= 3U,
          "retention exceeded three after storage fault verification");
  std::cout << "PASS: storage mode=" << options.fault_mode
            << " produced the required terminal result and preserved a "
               "restorable published state\n";
}

void phase_stress(const Options &options, Context &context) {
  std::cout << "PHASE: bounded client reconnect/create/restore stress\n";
  static_cast<void>(require_live_continuity(context));
  constexpr std::uint64_t cycles = 4U;
  for (std::uint64_t cycle = 1; cycle <= cycles; ++cycle) {
    {
      SchedulerHarness scheduler(options);
      scheduler.connect();
      const auto operation = scheduler.submit_create(
          context.container_id,
          request(options.run_id + "-stress-create-" + std::to_string(cycle)));
      scheduler.wait_success(operation);
    }
    context.storage.require_recovered_clean();
    require(context.storage.generations().size() <= 3U,
            "stress create violated keep_last=3");
    static_cast<void>(require_live_continuity(context));

    {
      SchedulerHarness scheduler(options);
      scheduler.connect();
      const auto operation = scheduler.submit_restore(
          context.container_id,
          request(options.run_id + "-stress-restore-" + std::to_string(cycle)));
      scheduler.wait_success(operation);
    }
    static_cast<void>(require_live_continuity(context));
    context.podman.require_at_most_one();
  }

  const auto quiescent = generation_numbers(context.storage.generations());
  std::this_thread::sleep_for(2s);
  context.storage.require_recovered_clean();
  require(generation_numbers(context.storage.generations()) == quiescent &&
              quiescent.size() <= 3U,
          "retention changed or exceeded three after stress quiescence");
  std::cout << "PASS: " << cycles
            << " fresh-client create/restore cycles retained at most three "
               "generations\n";
}

void phase_finalize(const Options &options, Context &context) {
  static_cast<void>(require_live_continuity(context));
  context.podman.require_at_most_one();
  context.storage.require_recovered_clean();
  require(context.storage.generations().size() <= 3U,
          "final retention exceeds keep_last=3 after startup GC");
  context.storage.require_no_orphan_chunks();
  SchedulerHarness scheduler(options);
  scheduler.connect();
  scheduler.unregister_container(context.container_id,
                                 request(options.run_id + "-final-unregister"));
  std::cout
      << "PASS: final storage/runtime invariants and quiescent unregister\n";
}

void run_phase(const Options &options) {
  Context context(options);
  if (options.phase == "initialize") {
    phase_initialize(options, context);
  } else if (options.phase == "register") {
    phase_register(options, context);
  } else if (options.phase == "submit-create-crash") {
    phase_submit_create_crash(options, context);
  } else if (options.phase == "reconcile-create-crash") {
    phase_reconcile_create_crash(options, context);
  } else if (options.phase == "prepare-restore-crash") {
    phase_prepare_restore_crash(options, context);
  } else if (options.phase == "reconcile-restore-before") {
    phase_reconcile_restore_before(options, context);
  } else if (options.phase == "reconcile-restore-after") {
    phase_reconcile_restore_after(options, context);
  } else if (options.phase == "reconcile-restore-conflict") {
    phase_reconcile_restore_conflict(options, context);
  } else if (options.phase == "storage-fault") {
    phase_storage_fault(options, context);
  } else if (options.phase == "stress") {
    phase_stress(options, context);
  } else if (options.phase == "finalize") {
    phase_finalize(options, context);
  } else {
    fail("unknown phase: " + options.phase);
  }
}

} // namespace

int main(const int argc, char **const argv) {
  try {
    const auto options = parse_options(argc, argv);
    run_phase(options);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: crash/recovery driver: " << error.what() << '\n';
    return 1;
  }
}
