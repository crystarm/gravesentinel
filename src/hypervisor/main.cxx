#include "src/chunking/snapshot_chunker.hxx"
#include "src/core/blake3_hasher.hxx"
#include "src/core/checkpoint_service.hxx"
#include "src/hypervisor/linux_affinity_controller.hxx"
#include "src/hypervisor/podman_adapter.hxx"
#include "src/hypervisor/posix_process_runner.hxx"
#include "src/hypervisor/rpc_handler.hxx"
#include "src/ipc/operation_store.hxx"
#include "src/ipc/server.hxx"
#include "src/storage/filesystem_storage.hxx"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/types.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

struct Config {
  std::string socket{std::string("/run/gravesentinel/hypervisor.sock")};
  std::string storage{std::string("/var/lib/gravesentinel")};
  std::string podman{std::string("/usr/bin/podman")};
  std::string criu{std::string("/usr/sbin/criu")};
  std::string runtime_profile{std::string("podman-criu-rootful-v1")};
  std::optional<uid_t> scheduler_uid;
  std::optional<gid_t> socket_gid;
  unsigned socket_mode{0660};
  std::chrono::seconds garbage_collection_grace_period{std::chrono::hours{1}};
};

enum class ParseResult { success, help, error };

void print_usage(std::ostream &output) {
  output
      << "Usage: gravesentinel_hypervisor [OPTIONS]\n"
         "  --socket PATH             Unix socket path\n"
         "  --storage DIR            Persistent storage root\n"
         "  --podman PATH            Podman executable (default: "
         "/usr/bin/podman)\n"
         "  --criu PATH              CRIU executable (default: "
         "/usr/sbin/criu)\n"
         "  --scheduler-uid UID      Authorized scheduler UID\n"
         "  --socket-gid GID         Group owner for the Unix socket\n"
         "  --socket-mode OCTAL      Unix socket mode (default: 0660)\n"
         "  --runtime-profile NAME   Must be podman-criu-rootful-v1\n"
         "  --gc-grace-seconds N     Unreferenced chunk grace (default: 3600)\n"
         "  --help                   Show this help\n";
}

[[nodiscard]] std::optional<std::uintmax_t>
parse_unsigned(const std::string_view text, const int base) {
  if (text.empty())
    return std::nullopt;
  std::uintmax_t value{};
  const char *const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value, base);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    return std::nullopt;
  return value;
}

[[nodiscard]] ParseResult parse_arguments(const int argc, char **const argv,
                                          Config &config) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help")
      return ParseResult::help;
    if (index + 1 >= argc)
      return ParseResult::error;
    const std::string value(argv[++index]);

    if (argument == "--socket") {
      config.socket = value;
    } else if (argument == "--storage") {
      config.storage = value;
    } else if (argument == "--podman") {
      config.podman = value;
    } else if (argument == "--criu") {
      config.criu = value;
    } else if (argument == "--runtime-profile") {
      config.runtime_profile = value;
    } else if (argument == "--socket-mode") {
      const auto mode = parse_unsigned(value, 8);
      if (!mode || *mode > 0777U)
        return ParseResult::error;
      config.socket_mode = static_cast<unsigned>(*mode);
    } else if (argument == "--gc-grace-seconds") {
      const auto seconds = parse_unsigned(value, 10);
      const auto maximum_seconds = static_cast<std::uintmax_t>(
          std::numeric_limits<std::chrono::seconds::rep>::max());
      if (!seconds || *seconds > maximum_seconds)
        return ParseResult::error;
      config.garbage_collection_grace_period = std::chrono::seconds{
          static_cast<std::chrono::seconds::rep>(*seconds)};
    } else if (argument == "--scheduler-uid") {
      const auto uid = parse_unsigned(value, 10);
      const auto invalid_uid =
          static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max());
      if (!uid || *uid >= invalid_uid)
        return ParseResult::error;
      config.scheduler_uid = static_cast<uid_t>(*uid);
    } else if (argument == "--socket-gid") {
      const auto gid = parse_unsigned(value, 10);
      const auto invalid_gid =
          static_cast<std::uintmax_t>(std::numeric_limits<gid_t>::max());
      if (!gid || *gid >= invalid_gid)
        return ParseResult::error;
      config.socket_gid = static_cast<gid_t>(*gid);
    } else {
      return ParseResult::error;
    }
  }

  if (config.socket.empty() || config.storage.empty() ||
      config.podman.empty() || config.criu.empty() ||
      config.runtime_profile != "podman-criu-rootful-v1") {
    return ParseResult::error;
  }
  return ParseResult::success;
}

[[nodiscard]] bool ensure_directory(const std::filesystem::path &path,
                                    const std::string_view purpose) {
  if (path.empty())
    return true;
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (!error)
    return true;
  std::cerr << "cannot create " << purpose << ": " << error.message() << '\n';
  return false;
}

[[nodiscard]] std::optional<
    std::vector<gravesentinel::ccm::EnvironmentVariable>>
inherited_runtime_environment() {
  constexpr std::array<std::string_view, 4> allowed_names{
      "DBUS_SESSION_BUS_ADDRESS",
      "HOME",
      "PATH",
      "XDG_RUNTIME_DIR",
  };
  constexpr std::size_t maximum_value_bytes = 64U * 1024U;

  std::vector<gravesentinel::ccm::EnvironmentVariable> environment;
  environment.reserve(allowed_names.size());
  for (const auto name : allowed_names) {
    const std::string name_string{name};
    const char *const value = std::getenv(name_string.c_str());
    if (value == nullptr)
      continue;
    const std::string value_string{value};
    if (value_string.size() > maximum_value_bytes) {
      std::cerr << "allowlisted runtime environment value is too large: "
                << name << '\n';
      return std::nullopt;
    }
    environment.push_back(gravesentinel::ccm::EnvironmentVariable{
        .name = name_string,
        .value = value_string,
    });
  }
  return environment;
}

void write_log_value(std::ostream &output, const std::string_view value) {
  output << '"';
  for (const char character : value) {
    switch (character) {
    case '\\':
      output << "\\\\";
      break;
    case '"':
      output << "\\\"";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        output << '?';
      } else {
        output << character;
      }
      break;
    }
  }
  output << '"';
}

class SystemClock final : public gravesentinel::ccm::IClock {
public:
  [[nodiscard]] WallTime wall_now() const noexcept override {
    return std::chrono::system_clock::now();
  }

  [[nodiscard]] MonotonicTime monotonic_now() const noexcept override {
    return std::chrono::steady_clock::now();
  }
};

class IdGenerator final : public gravesentinel::ccm::IIdGenerator {
public:
  [[nodiscard]]
  gravesentinel::ccm::CheckpointResult<gravesentinel::ccm::CheckpointId>
  new_checkpoint_id() override {
    return generate<gravesentinel::ccm::CheckpointId>("checkpoint-");
  }

  [[nodiscard]]
  gravesentinel::ccm::CheckpointResult<gravesentinel::ccm::OperationId>
  new_operation_id() override {
    return generate<gravesentinel::ccm::OperationId>("operation-");
  }

private:
  template <typename Identifier>
  [[nodiscard]] gravesentinel::ccm::CheckpointResult<Identifier>
  generate(const std::string_view prefix) {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count =
          ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        return std::unexpected(gravesentinel::ccm::CheckpointErrorDetail{
            .code = gravesentinel::ccm::StableErrorCode::unavailable,
            .diagnostic = "secure random ID generation is unavailable",
            .retryable = true,
            .correlation_id = std::nullopt,
            .storage_side_effect = gravesentinel::ccm::StorageSideEffect::none,
            .runtime_side_effect = gravesentinel::ccm::RuntimeSideEffect::none,
        });
      }
      offset += static_cast<std::size_t>(count);
    }

    constexpr char digits[] = "0123456789abcdef";
    std::string text(prefix);
    text.reserve(prefix.size() + bytes.size() * 2U);
    for (const auto byte : bytes) {
      text.push_back(digits[byte >> 4U]);
      text.push_back(digits[byte & 0x0fU]);
    }
    auto identifier = Identifier::create(std::move(text));
    if (!identifier) {
      return std::unexpected(gravesentinel::ccm::CheckpointErrorDetail{
          .code = gravesentinel::ccm::StableErrorCode::internal,
          .diagnostic = "secure random ID has an invalid representation",
          .retryable = false,
          .correlation_id = std::nullopt,
          .storage_side_effect = gravesentinel::ccm::StorageSideEffect::none,
          .runtime_side_effect = gravesentinel::ccm::RuntimeSideEffect::none,
      });
    }
    return std::move(*identifier);
  }
};

class StderrLogger final : public gravesentinel::ccm::ILogger {
public:
  void log(const gravesentinel::ccm::LogLevel level,
           const std::string_view message,
           const std::span<const gravesentinel::ccm::LogField> fields) noexcept
      override {
    const char *const prefix =
        level == gravesentinel::ccm::LogLevel::error     ? "ERROR"
        : level == gravesentinel::ccm::LogLevel::warning ? "WARN"
        : level == gravesentinel::ccm::LogLevel::debug   ? "DEBUG"
                                                         : "INFO";
    const std::lock_guard lock(mutex_);
    std::cerr << '[' << prefix << "] " << message;
    for (const auto &field : fields) {
      std::cerr << ' ' << field.key << '=';
      write_log_value(std::cerr, field.value);
    }
    std::cerr << '\n';
  }

private:
  std::mutex mutex_;
};

} // namespace

int main(const int argc, char **const argv) {
  Config config;
  switch (parse_arguments(argc, argv, config)) {
  case ParseResult::help:
    print_usage(std::cout);
    return 0;
  case ParseResult::error:
    std::cerr << "invalid arguments (use --help)\n";
    return 2;
  case ParseResult::success:
    break;
  }

  const std::filesystem::path socket_path(config.socket);
  const std::filesystem::path storage_root(config.storage);
  const auto checkpoint_data = storage_root / "checkpoint-data";
  if (!ensure_directory(socket_path.parent_path(), "socket directory") ||
      !ensure_directory(storage_root, "storage directory")) {
    return 3;
  }

  gravesentinel::ipc::ServerConfig server;
  server.socket_path = config.socket;
  server.lock_path = (storage_root / "hypervisor.lock").string();
  server.mode = config.socket_mode;
  server.allowed_uid = config.scheduler_uid;
  server.socket_gid = config.socket_gid;
  server.peer_io_timeout = std::chrono::seconds(3);
  server.worker_count = 4;
  server.peer_queue_capacity = 32;
  server.max_requests_per_connection = 64;

  auto server_lock = gravesentinel::ipc::ServerLock::acquire(server.lock_path);
  if (!server_lock) {
    std::cerr << "cannot acquire hypervisor process lock\n";
    return 4;
  }
  if (!ensure_directory(checkpoint_data, "checkpoint data directory"))
    return 5;

  using namespace gravesentinel::ccm;
  SystemClock clock;
  auto storage_result = FilesystemStorage::open(checkpoint_data);
  if (!storage_result) {
    std::cerr << "cannot initialize checkpoint storage: "
              << storage_result.error().diagnostic << '\n';
    return 6;
  }
  auto storage = std::move(*storage_result);
  constexpr std::size_t kMaximumStartupRecoveryPasses = 1024U;
  bool recovery_complete = false;
  for (std::size_t pass = 0; pass < kMaximumStartupRecoveryPasses; ++pass) {
    auto recovered = storage->recover(StartupRecoveryOptions{
        .now = clock.wall_now(),
        .grace_period = config.garbage_collection_grace_period,
        .maximum_chunk_deletions_per_container = 128,
    });
    if (!recovered) {
      std::cerr << "cannot recover checkpoint storage: "
                << recovered.error().diagnostic << '\n';
      return 6;
    }
    if (recovered->containers_deferred != 0U) {
      std::cerr << "cannot recover checkpoint storage: a container namespace "
                   "is still leased\n";
      return 6;
    }
    if (!recovered->more_work) {
      recovery_complete = true;
      break;
    }
  }
  if (!recovery_complete) {
    std::cerr << "cannot recover checkpoint storage within the startup work "
                 "bound\n";
    return 6;
  }

  SteadyTimer timer;
  IdGenerator ids;
  StderrLogger logger;
  LinuxAffinityController affinity;
  PosixProcessRunner runner;
  const auto runtime_environment = inherited_runtime_environment();
  if (!runtime_environment) {
    std::cerr << "cannot prepare Podman/CRIU process environment\n";
    return 6;
  }
  PodmanAdapterConfig runtime_config{
      .podman_executable = config.podman,
      .criu_executable = config.criu,
      .staging_root = checkpoint_data,
      .environment = std::move(*runtime_environment),
  };
  PodmanAdapter podman(runner, affinity, logger, std::move(runtime_config));
  Blake3Hasher hasher;
  SnapshotChunker chunker(hasher);
  CheckpointCreator creator(*storage, podman, chunker, clock, ids, logger);
  RestoreCoordinator restore(*storage, podman, hasher, logger);
  CheckpointMaintenance maintenance{
      *storage, clock,
      CheckpointMaintenanceConfig{
          .garbage_collection_grace_period =
              config.garbage_collection_grace_period,
          .maximum_garbage_deletions_per_pass = 128,
      }};
  CheckpointOperations operations(creator, restore, maintenance);
  CheckpointService service(operations, clock, timer, ids, affinity, logger);

  gravesentinel::ipc::OperationStoreOptions store_options;
  store_options.worker_count = 4;
  auto operation_store_result = gravesentinel::ipc::OperationStore::create(
      (storage_root / "operations.jsonl").string(),
      gravesentinel::hypervisor::make_operation_executor(service),
      store_options);
  if (!operation_store_result) {
    std::cerr << "cannot initialize operation journal ("
              << gravesentinel::ipc::error_code_name(
                     operation_store_result.error().code)
              << "): " << operation_store_result.error().message << '\n';
    (void)service.shutdown();
    return 7;
  }
  auto operation_store = std::move(*operation_store_result);
  gravesentinel::hypervisor::RpcHandler rpc_handler(service, *operation_store);
  const gravesentinel::ipc::Handler handler =
      [&rpc_handler](const nlohmann::json &request) {
        return rpc_handler(request);
      };

  std::signal(SIGTERM, request_stop);
  std::signal(SIGINT, request_stop);
  std::cerr << "gravesentinel_hypervisor: startup ready; listening on "
            << config.socket << '\n';
  const int result = gravesentinel::ipc::serve(
      server, *server_lock, handler, [] { return stop_requested != 0; });
  operation_store->stop();
  (void)service.shutdown();
  return result;
}
