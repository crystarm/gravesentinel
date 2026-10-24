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
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
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
using gravesentinel::ccm::ManifestV1;
using gravesentinel::ccm::OperationId;
using gravesentinel::ccm::OperationState;
using gravesentinel::ccm::QueueState;
using gravesentinel::ccm::RequestId;
using gravesentinel::ccm::RpcFailure;
using gravesentinel::ccm::RpcFailureCode;
using gravesentinel::ccm::RuntimeContainerRef;
using gravesentinel::ccm::SchedulerClient;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kLogicalIdLabel = "io.gravesentinel.ccm.logical-id";
constexpr auto kCommandTimeout = 30s;
constexpr auto kObservationTimeout = 45s;
constexpr auto kOperationTimeout = 10min;

struct Options {
  std::filesystem::path storage_root;
  std::filesystem::path daemon_log;
  std::filesystem::path podman;
  std::string socket_path;
  std::string fixture_image;
  std::string run_id;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

void require(const bool condition, std::string message) {
  if (!condition)
    fail(std::move(message));
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

[[nodiscard]] Options parse_options(const int argc, char **const argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (index + 1 >= argc)
      fail("missing value for " + std::string(argument));
    const std::string value{argv[++index]};
    if (argument == "--socket") {
      options.socket_path = value;
    } else if (argument == "--storage") {
      options.storage_root = value;
    } else if (argument == "--daemon-log") {
      options.daemon_log = value;
    } else if (argument == "--podman") {
      options.podman = value;
    } else if (argument == "--fixture-image") {
      options.fixture_image = value;
    } else if (argument == "--run-id") {
      options.run_id = value;
    } else {
      fail("unknown argument: " + std::string(argument));
    }
  }
  require(!options.socket_path.empty(), "--socket is required");
  require(!options.storage_root.empty(), "--storage is required");
  require(!options.daemon_log.empty(), "--daemon-log is required");
  require(!options.podman.empty(), "--podman is required");
  require(!options.fixture_image.empty(), "--fixture-image is required");
  require(!options.run_id.empty(), "--run-id is required");
  require(options.run_id.size() <= 48U, "--run-id is too long");
  return options;
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
  if (::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0)
    fail("cannot create subprocess pipes");

  const pid_t child = ::fork();
  if (child < 0)
    fail("cannot fork subprocess");
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

void collect_nonempty_lines(std::set<std::string> &values,
                            const std::string_view text) {
  std::istringstream lines{std::string(text)};
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(std::move(line));
    if (!line.empty())
      values.insert(std::move(line));
  }
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
    } else if (key == "nonce") {
      sample.nonce = value;
      has_nonce = !sample.nonce.empty();
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

  void require_logs_contain(const std::string_view name,
                            const std::string_view marker) const {
    const auto logs = command({"logs", std::string(name)}, kCommandTimeout);
    if (logs.exit_code != 0) {
      fail("podman logs failed for " + std::string(name) + ": " +
           logs.standard_error);
    }
    const std::string combined =
        logs.standard_output + "\n" + logs.standard_error;
    require(combined.find(marker) != std::string::npos,
            "fixture logs do not contain marker: " + std::string(marker));
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

private:
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
  if (!identifier)
    fail("invalid " + std::string(purpose) + ": " +
         identifier.error().diagnostic);
  return std::move(*identifier);
}

class SchedulerHarness final {
public:
  SchedulerHarness(std::string socket_path, const uid_t server_uid,
                   std::string request_prefix)
      : client_(std::move(socket_path), 3s, kOperationTimeout, server_uid),
        request_prefix_(std::move(request_prefix)) {}

  void connect() {
    const auto deadline = Clock::now() + 15s;
    std::string last_failure = "daemon did not accept a connection";
    while (Clock::now() < deadline) {
      const auto connected = client_.connect();
      if (!connected) {
        last_failure = rpc_failure_text(connected.error());
        std::this_thread::sleep_for(100ms);
        continue;
      }
      const auto health = client_.get_health();
      if (!health) {
        last_failure = rpc_failure_text(health.error());
        std::this_thread::sleep_for(100ms);
        continue;
      }
      require(health->state == HealthState::healthy,
              "hypervisor health is not healthy");
      require(health->queue == QueueState::available,
              "hypervisor operation queue is saturated");
      std::cout << "INFO: handshake protocol_version="
                << health->protocol_version << '\n';
      return;
    }
    fail("scheduler client handshake failed: " + last_failure);
  }

  void register_container(const ContainerRegistration &registration) {
    require(client_.register_container(registration, next_request("register")),
            "Register failed for " + registration.container_id.value());
  }

  [[nodiscard]] bool try_unregister_container(const ContainerId &container_id) {
    return client_.unregister_container(container_id,
                                        next_request("unregister"));
  }

  [[nodiscard]] OperationId create_checkpoint(const ContainerId &container_id) {
    auto operation = client_.submit_create_checkpoint(
        container_id, next_request("create-checkpoint"));
    if (!operation)
      fail("SubmitCreateCheckpoint failed: " +
           rpc_failure_text(operation.error()));
    wait_for_success(*operation);
    return *operation;
  }

  [[nodiscard]] OperationId
  restore_or_restart(const ContainerId &container_id) {
    auto operation = client_.submit_restore_or_restart(
        container_id, next_request("restore-or-restart"));
    if (!operation)
      fail("SubmitRestoreOrRestart failed: " +
           rpc_failure_text(operation.error()));
    wait_for_success(*operation);
    return *operation;
  }

private:
  [[nodiscard]] RequestId next_request(const std::string_view operation) {
    ++sequence_;
    return make_identifier<RequestId>(request_prefix_ + "-" +
                                          std::string(operation) + "-" +
                                          std::to_string(sequence_),
                                      "request ID");
  }

  void wait_for_success(const OperationId &operation_id) {
    const auto deadline = Clock::now() + kOperationTimeout;
    while (Clock::now() < deadline) {
      auto status = client_.get_operation(operation_id);
      if (!status)
        fail("GetOperation failed for " + operation_id.value() + ": " +
             rpc_failure_text(status.error()));
      if (status->state == OperationState::succeeded) {
        std::cout << "INFO: operation=" << operation_id.value()
                  << " state=succeeded\n";
        return;
      }
      if (status->state == OperationState::failed ||
          status->state == OperationState::indeterminate) {
        fail("operation " + operation_id.value() + " ended in " +
             std::string(operation_state_name(status->state)) +
             (status->error
                  ? " with stable error " +
                        std::to_string(static_cast<unsigned>(*status->error))
                  : ""));
      }
      std::this_thread::sleep_for(100ms);
    }
    fail("operation timed out: " + operation_id.value());
  }

  SchedulerClient client_;
  std::string request_prefix_;
  std::uint64_t sequence_{};
};

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

[[nodiscard]] std::vector<std::byte>
read_binary_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    fail("cannot open file: " + path.string());
  const auto end = input.tellg();
  if (end < 0)
    fail("cannot determine file size: " + path.string());
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
  if (!input)
    fail("cannot read file: " + path.string());
  return bytes;
}

void write_binary_file(const std::filesystem::path &path,
                       const std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    fail("cannot open file for writing: " + path.string());
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  if (!output)
    fail("cannot write file: " + path.string());
}

struct StoredGeneration {
  std::uint64_t generation{};
  std::filesystem::path manifest_path;
  ManifestV1 manifest;
  std::set<std::string> chunks;
};

class ScopedFileLock final {
public:
  explicit ScopedFileLock(const std::filesystem::path &path) {
    descriptor_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor_ < 0)
      fail("cannot open storage namespace lock: " + path.string());
    while (::flock(descriptor_, LOCK_EX) != 0) {
      if (errno != EINTR) {
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
        fail("cannot acquire storage namespace lock: " + path.string());
      }
    }
  }

  ~ScopedFileLock() {
    if (descriptor_ >= 0) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      static_cast<void>(::close(descriptor_));
    }
  }

  ScopedFileLock(const ScopedFileLock &) = delete;
  ScopedFileLock &operator=(const ScopedFileLock &) = delete;

private:
  int descriptor_{-1};
};

class StorageProbe final {
public:
  StorageProbe(std::filesystem::path checkpoint_root, std::string container_id)
      : checkpoint_root_(std::move(checkpoint_root)),
        container_id_(std::move(container_id)) {}

  [[nodiscard]] std::vector<StoredGeneration> generations() const {
    const auto directory = container_directory();
    const ScopedFileLock lock{lock_path(directory)};
    return load_generations(directory);
  }

  template <typename Function>
  decltype(auto) with_namespace_lock(Function function) const {
    const auto directory = container_directory();
    const ScopedFileLock lock{lock_path(directory)};
    return std::invoke(std::move(function));
  }

  [[nodiscard]] std::filesystem::path
  chunk_path(const std::string_view hash) const {
    require(hash.size() == 64U, "invalid chunk hash length");
    return container_directory() / "chunks" / std::string(hash.substr(0, 2)) /
           std::string(hash);
  }

  [[nodiscard]] std::filesystem::path
  candidate_path(const std::string_view hash) const {
    return container_directory() / "gc-candidates" / std::string(hash);
  }

  void corrupt_chunk(const std::string_view hash) const {
    require(hash.size() == 64U, "invalid chunk hash length");
    const auto directory = container_directory();
    const ScopedFileLock lock{lock_path(directory)};
    const auto path = directory / "chunks" / std::string(hash.substr(0, 2)) /
                      std::string(hash);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file)
      fail("cannot open chunk for corruption: " + path.string());
    char byte{};
    file.read(&byte, 1);
    if (file.gcount() != 1)
      fail("cannot read chunk byte: " + path.string());
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
    file.seekp(0);
    file.write(&byte, 1);
    file.close();
    if (!file)
      fail("cannot corrupt chunk: " + path.string());
  }

  void make_newest_runtime_incompatible() const {
    const auto directory = container_directory();
    const ScopedFileLock lock{lock_path(directory)};
    auto stored = load_generations(directory);
    require(!stored.empty(),
            "no manifest exists for incompatibility injection");
    auto manifest = std::move(stored.back().manifest);
    manifest.runtime_fingerprint.kernel_release += "-stage12-incompatible";
    auto encoded = gravesentinel::ccm::encode_manifest_v1(manifest);
    if (!encoded)
      fail("cannot re-encode incompatible manifest: " +
           encoded.error().diagnostic);
    write_binary_file(stored.back().manifest_path, *encoded);
  }

private:
  [[nodiscard]] std::vector<StoredGeneration>
  load_generations(const std::filesystem::path &directory) const {
    std::vector<StoredGeneration> stored;
    for (const auto &entry :
         std::filesystem::directory_iterator(directory / "manifests")) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      auto bytes = read_binary_file(entry.path());
      auto manifest = gravesentinel::ccm::decode_manifest_v1(bytes);
      if (!manifest) {
        fail("cannot decode manifest " + entry.path().string() + ": " +
             manifest.error().diagnostic);
      }
      std::set<std::string> chunks;
      for (const auto &artifact : manifest->artifacts) {
        for (const auto &chunk : artifact.chunks)
          chunks.insert(hash_to_hex(chunk.hash));
      }
      stored.push_back(StoredGeneration{
          .generation = manifest->generation,
          .manifest_path = entry.path(),
          .manifest = std::move(*manifest),
          .chunks = std::move(chunks),
      });
    }
    std::ranges::sort(stored, {}, &StoredGeneration::generation);
    return stored;
  }

  [[nodiscard]] std::filesystem::path
  lock_path(const std::filesystem::path &directory) const {
    return checkpoint_root_ / "locks" /
           (directory.filename().string() + ".lock");
  }

  [[nodiscard]] std::filesystem::path container_directory() const {
    const auto containers = checkpoint_root_ / "containers";
    for (const auto &entry : std::filesystem::directory_iterator(containers)) {
      if (!entry.is_directory())
        continue;
      const auto identity = read_binary_file(entry.path() / "identity");
      const std::string text(reinterpret_cast<const char *>(identity.data()),
                             identity.size());
      if (text == container_id_)
        return entry.path();
    }
    fail("storage namespace not found for " + container_id_);
  }

  std::filesystem::path checkpoint_root_;
  std::string container_id_;
};

[[nodiscard]] std::string
unique_chunk(const StoredGeneration &target,
             const std::span<const StoredGeneration> other_generations) {
  for (const auto &hash : target.chunks) {
    const bool shared = std::ranges::any_of(
        other_generations, [&hash](const StoredGeneration &other) {
          return other.chunks.contains(hash);
        });
    if (!shared)
      return hash;
  }
  fail("checkpoint has no generation-unique chunk for fault injection");
}

void wait_for_path_state(
    const std::filesystem::path &path, const bool should_exist,
    const std::chrono::milliseconds timeout = kObservationTimeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (!error && exists == should_exist)
      return;
    std::this_thread::sleep_for(100ms);
  }
  fail("timed out waiting for path to become " +
       std::string(should_exist ? "present: " : "absent: ") + path.string());
}

void require_log_line(const std::filesystem::path &path,
                      const std::span<const std::string> fragments) {
  const auto deadline = Clock::now() + 5s;
  while (Clock::now() < deadline) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      const bool matches =
          std::ranges::all_of(fragments, [&line](const std::string &fragment) {
            return line.find(fragment) != std::string::npos;
          });
      if (matches)
        return;
    }
    std::this_thread::sleep_for(50ms);
  }
  std::string expected;
  for (const auto &fragment : fragments)
    expected += " [" + fragment + "]";
  fail("daemon log has no line containing:" + expected);
}

struct Scenario {
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  std::string instance_tag;
};

[[nodiscard]] Scenario make_scenario(const Options &options,
                                     const std::string_view name) {
  const std::string logical_id = options.run_id + "-" + std::string(name);
  return Scenario{
      .container_id = make_identifier<ContainerId>(logical_id, "container ID"),
      .runtime_container =
          make_identifier<RuntimeContainerRef>(logical_id, "runtime name"),
      .instance_tag = std::string(name),
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
              .container_cpu = 0,
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
                      "/dev/shm/gravesentinel-stage12.state",
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

[[nodiscard]] FixtureSample start_scenario(const Options &options,
                                           const Scenario &scenario,
                                           SchedulerHarness &scheduler,
                                           const PodmanOracle &podman) {
  scheduler.register_container(make_registration(options, scenario));
  static_cast<void>(scheduler.restore_or_restart(scenario.container_id));
  const auto sample = podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &candidate) {
        return candidate.instance_tag == scenario.instance_tag &&
               candidate.file_records >= 3U && candidate.file_counter > 0U;
      });
  require(podman.owner(scenario.runtime_container.value()) ==
              scenario.container_id.value(),
          "cold-started container has the wrong logical ID label");
  require(podman.running(scenario.runtime_container.value()),
          "cold-started fixture is not running");
  podman.require_logs_contain(scenario.runtime_container.value(),
                              "fixture-start pid=");
  require(sample.cpu_mask != 0U && (sample.cpu_mask & ~std::uint64_t{1}) == 0U,
          "fixture escaped container CPU 0");
  podman.require_single_instance(scenario.container_id.value());
  return sample;
}

void finish_scenario(const Options &options, const Scenario &scenario,
                     SchedulerHarness &scheduler) {
  const StorageProbe storage{options.storage_root / "checkpoint-data",
                             scenario.container_id.value()};
  const auto deadline = Clock::now() + 30s;
  while (Clock::now() < deadline) {
    storage.with_namespace_lock([] {});
    if (scheduler.try_unregister_container(scenario.container_id))
      return;
    std::this_thread::sleep_for(100ms);
  }
  fail("Unregister did not reach storage quiescence for " +
       scenario.container_id.value());
}

void run_smoke_scenario(const Options &options, SchedulerHarness &scheduler,
                        const PodmanOracle &podman) {
  std::cout << "SCENARIO: checkpoint leave-running and restore continuity\n";
  const auto scenario = make_scenario(options, "smoke");
  const auto before = start_scenario(options, scenario, scheduler, podman);
  const auto original_runtime_id =
      podman.container_id(scenario.runtime_container.value());

  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
  const auto after_checkpoint =
      podman.latest_sample(scenario.runtime_container.value());
  const auto continued = podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.nonce == before.nonce &&
               sample.counter >= after_checkpoint.counter + 4U &&
               sample.file_records > after_checkpoint.file_records;
      });
  require(podman.container_id(scenario.runtime_container.value()) ==
              original_runtime_id,
          "checkpoint replaced the source runtime instance");
  require(podman.running(scenario.runtime_container.value()),
          "source stopped after --leave-running checkpoint");
  podman.require_single_instance(scenario.container_id.value());

  static_cast<void>(scheduler.restore_or_restart(scenario.container_id));
  const auto restored_runtime_id =
      podman.container_id(scenario.runtime_container.value());
  require(restored_runtime_id != original_runtime_id,
          "restore did not create a new runtime incarnation");
  const auto restore_baseline =
      podman.latest_sample(scenario.runtime_container.value());
  const auto restored = podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.nonce == before.nonce &&
               sample.instance_tag == scenario.instance_tag &&
               sample.counter >= restore_baseline.counter + 4U &&
               sample.file_records > restore_baseline.file_records &&
               sample.file_counter > 0U;
      });
  require(podman.running(scenario.runtime_container.value()),
          "restored fixture is not running");
  require(restored.file_records > 1U,
          "restored fixture did not preserve local file history");
  require(restored.cpu_mask != 0U &&
              (restored.cpu_mask & ~std::uint64_t{1}) == 0U,
          "restored fixture escaped container CPU 0");
  podman.require_single_instance(scenario.container_id.value());
  std::cout << "PASS: leave-running source advanced from counter="
            << after_checkpoint.counter << " to " << continued.counter
            << "; restored nonce=" << restored.nonce
            << " file_records=" << restored.file_records << '\n';
  finish_scenario(options, scenario, scheduler);
}

void run_fallback_scenario(const Options &options, SchedulerHarness &scheduler,
                           const PodmanOracle &podman) {
  std::cout << "SCENARIO: corrupted newest chunk falls back to generation 1\n";
  const auto scenario = make_scenario(options, "fallback");
  const auto initial = start_scenario(options, scenario, scheduler, podman);
  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
  const auto after_first =
      podman.latest_sample(scenario.runtime_container.value());
  static_cast<void>(podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.counter >= after_first.counter + 8U &&
               sample.file_records > after_first.file_records;
      }));
  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));

  StorageProbe storage{options.storage_root / "checkpoint-data",
                       scenario.container_id.value()};
  const auto generations = storage.generations();
  require(generations.size() == 2U && generations[0].generation == 1U &&
              generations[1].generation == 2U,
          "fallback setup did not publish generations 1 and 2");
  const std::array older{generations[0]};
  const auto corrupted_hash = unique_chunk(generations[1], older);
  storage.corrupt_chunk(corrupted_hash);

  const auto source_runtime_id =
      podman.container_id(scenario.runtime_container.value());
  const auto operation = scheduler.restore_or_restart(scenario.container_id);
  require(podman.container_id(scenario.runtime_container.value()) !=
              source_runtime_id,
          "fallback restore did not replace the source incarnation");
  const auto restore_baseline =
      podman.latest_sample(scenario.runtime_container.value());
  const auto restored = podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.nonce == initial.nonce &&
               sample.counter >= restore_baseline.counter + 4U &&
               sample.file_records > restore_baseline.file_records;
      });
  require(podman.running(scenario.runtime_container.value()),
          "fallback-restored fixture is not running");
  require(restored.cpu_mask != 0U &&
              (restored.cpu_mask & ~std::uint64_t{1}) == 0U,
          "fallback-restored fixture escaped container CPU 0");
  podman.require_single_instance(scenario.container_id.value());
  const std::array rejected_fragments{
      std::string("operation_id=\"") + operation.value() + "\"",
      std::string("container_id=\"") + scenario.container_id.value() + "\"",
      std::string("generation=\"2\""),
      std::string("error_code=\"corrupted_snapshot\""),
  };
  require_log_line(options.daemon_log, rejected_fragments);
  const std::array restored_fragments{
      std::string("operation_id=\"") + operation.value() + "\"",
      std::string("container_id=\"") + scenario.container_id.value() + "\"",
      std::string("to_state=\"RESTORED\""),
      std::string("generation=\"1\""),
  };
  require_log_line(options.daemon_log, restored_fragments);
  std::cout << "PASS: corrupted generation=2 chunk=" << corrupted_hash
            << "; observed restored generation=1 nonce=" << restored.nonce
            << " file_records=" << restored.file_records << '\n';
  finish_scenario(options, scenario, scheduler);
}

void run_cold_start_scenario(const Options &options,
                             SchedulerHarness &scheduler,
                             const PodmanOracle &podman) {
  std::cout
      << "SCENARIO: incompatible snapshot uses explicit cold-start spec\n";
  const auto scenario = make_scenario(options, "cold");
  const auto initial = start_scenario(options, scenario, scheduler, podman);
  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
  const auto source_runtime_id =
      podman.container_id(scenario.runtime_container.value());

  StorageProbe storage{options.storage_root / "checkpoint-data",
                       scenario.container_id.value()};
  const auto before = storage.generations();
  require(before.size() == 1U && before.front().generation == 1U,
          "cold-start setup did not publish generation 1");
  storage.make_newest_runtime_incompatible();

  const auto operation = scheduler.restore_or_restart(scenario.container_id);
  require(podman.container_id(scenario.runtime_container.value()) !=
              source_runtime_id,
          "cold start did not replace the source incarnation");
  const auto restart_baseline =
      podman.latest_sample(scenario.runtime_container.value());
  const auto restarted = podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.nonce != initial.nonce &&
               sample.instance_tag == scenario.instance_tag &&
               sample.counter >= restart_baseline.counter + 4U &&
               sample.file_records > restart_baseline.file_records;
      });
  require(podman.running(scenario.runtime_container.value()),
          "cold-started replacement fixture is not running");
  require(restarted.cpu_mask != 0U &&
              (restarted.cpu_mask & ~std::uint64_t{1}) == 0U,
          "cold-started replacement escaped container CPU 0");
  podman.require_single_instance(scenario.container_id.value());
  const std::array rejected_fragments{
      std::string("operation_id=\"") + operation.value() + "\"",
      std::string("container_id=\"") + scenario.container_id.value() + "\"",
      std::string("generation=\"1\""),
      std::string("error_code=\"incompatible_runtime\""),
  };
  require_log_line(options.daemon_log, rejected_fragments);
  const std::array restarted_fragments{
      std::string("operation_id=\"") + operation.value() + "\"",
      std::string("container_id=\"") + scenario.container_id.value() + "\"",
      std::string("to_state=\"RESTORED\""),
  };
  require_log_line(options.daemon_log, restarted_fragments);
  std::cout << "PASS: generation=1 rejected as incompatible; explicit image="
            << options.fixture_image << " argv_tag=" << scenario.instance_tag
            << " fresh_nonce=" << restarted.nonce
            << " file_records=" << restarted.file_records << '\n';
  finish_scenario(options, scenario, scheduler);
}

void wait_for_progress(const Scenario &scenario, const PodmanOracle &podman) {
  const auto current = podman.latest_sample(scenario.runtime_container.value());
  static_cast<void>(podman.wait_for_sample(
      scenario.runtime_container.value(), [&](const FixtureSample &sample) {
        return sample.counter >= current.counter + 8U &&
               sample.file_records > current.file_records;
      }));
}

void run_retention_scenario(const Options &options, SchedulerHarness &scheduler,
                            const PodmanOracle &podman) {
  std::cout << "SCENARIO: keep last three and collect grace-expired chunks\n";
  const auto scenario = make_scenario(options, "retention");
  static_cast<void>(start_scenario(options, scenario, scheduler, podman));
  StorageProbe storage{options.storage_root / "checkpoint-data",
                       scenario.container_id.value()};

  for (std::uint64_t generation = 1; generation <= 3U; ++generation) {
    static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
    const auto stored = storage.generations();
    require(stored.size() == generation &&
                stored.back().generation == generation,
            "unexpected manifest set before retention");
    wait_for_progress(scenario, podman);
  }

  const auto first_three = storage.generations();
  require(first_three.size() == 3U, "retention setup requires three manifests");
  const std::span<const StoredGeneration> newer_generations{
      first_three.data() + 1, first_three.size() - 1U};
  const auto old_hash = unique_chunk(first_three.front(), newer_generations);
  const auto old_chunk_path = storage.chunk_path(old_hash);
  require(std::filesystem::exists(old_chunk_path),
          "old generation chunk is absent before retention");

  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
  const auto after_four = storage.generations();
  require(after_four.size() == 3U && after_four.front().generation == 2U &&
              after_four.back().generation == 4U,
          "retention did not keep generations 2, 3 and 4");
  wait_for_path_state(storage.candidate_path(old_hash), true);
  std::this_thread::sleep_for(2s);
  wait_for_progress(scenario, podman);
  static_cast<void>(scheduler.create_checkpoint(scenario.container_id));
  wait_for_path_state(old_chunk_path, false);

  const auto after_five = storage.generations();
  require(after_five.size() == 3U && after_five[0].generation == 3U &&
              after_five[1].generation == 4U && after_five[2].generation == 5U,
          "retention did not keep the newest three generations");
  podman.require_single_instance(scenario.container_id.value());
  std::cout << "PASS: retained generations=3,4,5; GC removed old chunk="
            << old_hash << " after grace/quiescence\n";
  finish_scenario(options, scenario, scheduler);
}

} // namespace

int main(const int argc, char **const argv) {
  try {
    const auto options = parse_options(argc, argv);
    SchedulerHarness scheduler{options.socket_path, ::geteuid(),
                               options.run_id};
    scheduler.connect();
    const PodmanOracle podman{options.podman};

    run_smoke_scenario(options, scheduler, podman);
    run_fallback_scenario(options, scheduler, podman);
    run_cold_start_scenario(options, scheduler, podman);
    run_retention_scenario(options, scheduler, podman);
    std::cout
        << "PASS: all SchedulerClient/process-boundary scenarios passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: hypervisor E2E driver: " << error.what() << '\n';
    return 1;
  }
}
