#include "src/hypervisor/linux_affinity_controller.hxx"
#include "src/hypervisor/podman_adapter.hxx"
#include "src/hypervisor/posix_process_runner.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumArgumentCount = 32U;
constexpr std::size_t kMaximumArgumentBytes = 4096U;
constexpr std::size_t kMaximumTotalArgumentBytes = 32U * 1024U;
constexpr std::size_t kMaximumMetadataBytes = 64U * 1024U;
constexpr std::size_t kMaximumMetadataFieldBytes = 4096U;
constexpr std::string_view kMetadataFilename = "checkpoint-metadata.json";
constexpr std::string_view kStagingDirectoryName = "staging";
constexpr std::uint64_t kMetadataSchemaVersion = 1U;
const RuntimeCpuAssignment kCpuAssignment{
    .container_cpus = CpuSet::create({0U}).value(),
    .checkpoint_cpu = 1U,
};

class FileDescriptor final {
public:
  FileDescriptor() = default;
  explicit FileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
  ~FileDescriptor() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) {
        static_cast<void>(::close(descriptor_));
      }
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }

  [[nodiscard]] std::expected<void, std::string> close_checked() {
    if (descriptor_ < 0) {
      return {};
    }
    const int descriptor = std::exchange(descriptor_, -1);
    if (::close(descriptor) != 0) {
      return std::unexpected("close metadata file failed");
    }
    return {};
  }

private:
  int descriptor_{-1};
};

class StderrLogger final : public ILogger {
public:
  void log(LogLevel level, std::string_view message,
           std::span<const LogField>) noexcept override {
    try {
      std::cerr << "adapter[" << level_name(level) << "]: " << message << '\n';
    } catch (...) {
    }
  }

private:
  [[nodiscard]] static std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug:
      return "debug";
    case LogLevel::info:
      return "info";
    case LogLevel::warning:
      return "warning";
    case LogLevel::error:
      return "error";
    }
    return "unknown";
  }
};

struct CommandLine {
  std::string subcommand;
  std::string podman;
  std::string criu;
  std::string staging_root;
  std::string logical_id;
  std::string container;
  std::optional<std::string> image;
  std::optional<std::string> artifact;
  std::optional<std::string> expected_runtime_id;
};

[[nodiscard]] std::expected<CommandLine, std::string>
parse_command_line(int argc, char **argv) {
  if (argc < 2 || static_cast<std::size_t>(argc) > kMaximumArgumentCount) {
    return std::unexpected("invalid argument count");
  }

  std::size_t total_bytes = 0U;
  for (int index = 0; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument.size() > kMaximumArgumentBytes ||
        total_bytes > kMaximumTotalArgumentBytes - argument.size()) {
      return std::unexpected("command line exceeds the configured limits");
    }
    total_bytes += argument.size();
  }

  CommandLine command_line;
  command_line.subcommand = argv[1];
  if (command_line.subcommand != "cold-start" &&
      command_line.subcommand != "capture" &&
      command_line.subcommand != "stop-remove" &&
      command_line.subcommand != "restore" &&
      command_line.subcommand != "cleanup") {
    return std::unexpected("unknown subcommand");
  }

  std::set<std::string> seen_options;
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return std::unexpected("an option is missing its value");
    }
    const std::string option = argv[index];
    const std::string value = argv[index + 1];
    if (!option.starts_with("--") || !seen_options.insert(option).second) {
      return std::unexpected("invalid or repeated option");
    }

    if (option == "--podman") {
      command_line.podman = value;
    } else if (option == "--criu") {
      command_line.criu = value;
    } else if (option == "--staging-root") {
      command_line.staging_root = value;
    } else if (option == "--logical-id") {
      command_line.logical_id = value;
    } else if (option == "--container") {
      command_line.container = value;
    } else if (option == "--image") {
      command_line.image = value;
    } else if (option == "--artifact") {
      command_line.artifact = value;
    } else if (option == "--expected-runtime-id") {
      command_line.expected_runtime_id = value;
    } else {
      return std::unexpected("unknown option");
    }
  }

  if (command_line.podman.empty() || command_line.criu.empty() ||
      command_line.staging_root.empty() || command_line.logical_id.empty() ||
      command_line.container.empty()) {
    return std::unexpected("required operational option is missing");
  }
  if (command_line.logical_id.size() > 255U ||
      command_line.container.size() > 255U) {
    return std::unexpected("container identity exceeds the configured limit");
  }
  if (command_line.subcommand == "cold-start") {
    if (!command_line.image || command_line.image->empty() ||
        command_line.artifact || command_line.expected_runtime_id) {
      return std::unexpected("cold-start requires only --image");
    }
  } else if (command_line.subcommand == "restore") {
    if (!command_line.artifact || command_line.artifact->empty() ||
        command_line.image || command_line.expected_runtime_id) {
      return std::unexpected("restore requires only --artifact");
    }
  } else if (command_line.subcommand == "stop-remove") {
    if (!command_line.expected_runtime_id ||
        command_line.expected_runtime_id->empty() || command_line.image ||
        command_line.artifact) {
      return std::unexpected("stop-remove requires only --expected-runtime-id");
    }
  } else if (command_line.image || command_line.artifact ||
             command_line.expected_runtime_id) {
    return std::unexpected("subcommand received an unsupported option");
  }

  return command_line;
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
canonical_executable(std::string_view value) {
  if (value.empty() || value.size() > kMaximumArgumentBytes) {
    return std::unexpected("executable path exceeds the configured limit");
  }
  const std::filesystem::path path{value};
  if (!path.is_absolute()) {
    return std::unexpected("executable path must be absolute");
  }

  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_regular_file(canonical, error) || error ||
      ::access(canonical.c_str(), X_OK) != 0) {
    return std::unexpected("executable path is unavailable or unsafe");
  }
  return canonical;
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
canonical_directory(std::string_view value) {
  if (value.empty() || value.size() > kMaximumArgumentBytes) {
    return std::unexpected("staging root exceeds the configured limit");
  }
  const std::filesystem::path path{value};
  if (!path.is_absolute()) {
    return std::unexpected("staging root must be absolute");
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || status.type() != std::filesystem::file_type::directory) {
    return std::unexpected("staging root is unavailable or unsafe");
  }
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || canonical != path.lexically_normal()) {
    return std::unexpected("staging root must be canonical");
  }
  return canonical;
}

[[nodiscard]] std::expected<std::vector<EnvironmentVariable>, std::string>
inherited_environment() {
  constexpr std::array<std::string_view, 4> allowed_names{
      "DBUS_SESSION_BUS_ADDRESS",
      "HOME",
      "PATH",
      "XDG_RUNTIME_DIR",
  };

  std::vector<EnvironmentVariable> environment;
  environment.reserve(allowed_names.size());
  for (const auto name : allowed_names) {
    const std::string name_string{name};
    const char *const value = std::getenv(name_string.c_str());
    if (value == nullptr) {
      continue;
    }
    const std::string value_string{value};
    if (value_string.size() > 64U * 1024U) {
      return std::unexpected(
          "an allowlisted environment value exceeds the configured limit");
    }
    environment.push_back(
        EnvironmentVariable{.name = name_string, .value = value_string});
  }
  return environment;
}

[[nodiscard]] Json fingerprint_json(const RuntimeFingerprint &fingerprint) {
  return Json{
      {"runtime_name", fingerprint.runtime_name},
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
      {"command_profile", fingerprint.command_profile},
  };
}

[[nodiscard]] std::expected<void, std::string>
write_all(int descriptor, std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected("write metadata file failed");
    }
    if (written == 0) {
      return std::unexpected("write metadata file made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

[[nodiscard]] std::expected<void, std::string>
write_metadata(const std::filesystem::path &staging_root,
               const CapturedSnapshot &snapshot) {
  const Json document{
      {"schema_version", kMetadataSchemaVersion},
      {"source_image_identity", snapshot.source_image_identity},
      {"runtime_fingerprint", fingerprint_json(snapshot.runtime_fingerprint)},
  };
  std::string payload = document.dump(2);
  payload.push_back('\n');
  if (payload.size() > kMaximumMetadataBytes) {
    return std::unexpected("capture metadata exceeds the configured limit");
  }

  FileDescriptor directory{::open(
      staging_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.get() < 0) {
    return std::unexpected("open metadata directory failed");
  }

  std::string temporary_name;
  FileDescriptor temporary;
  for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
    temporary_name = ".checkpoint-metadata.tmp." +
                     std::to_string(static_cast<long long>(::getpid())) + "." +
                     std::to_string(attempt);
    const int descriptor =
        ::openat(directory.get(), temporary_name.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
      temporary = FileDescriptor{descriptor};
      break;
    }
    if (errno != EEXIST) {
      return std::unexpected("create metadata temporary file failed");
    }
  }
  if (temporary.get() < 0) {
    return std::unexpected("metadata temporary namespace is exhausted");
  }

  const auto remove_temporary = [&]() noexcept {
    static_cast<void>(::unlinkat(directory.get(), temporary_name.c_str(), 0));
  };
  if (const auto written = write_all(temporary.get(), payload); !written) {
    remove_temporary();
    return written;
  }
  if (::fsync(temporary.get()) != 0) {
    remove_temporary();
    return std::unexpected("sync metadata temporary file failed");
  }
  if (const auto closed = temporary.close_checked(); !closed) {
    remove_temporary();
    return closed;
  }

  const std::string metadata_name{kMetadataFilename};
  if (::linkat(directory.get(), temporary_name.c_str(), directory.get(),
               metadata_name.c_str(), 0) != 0) {
    remove_temporary();
    return std::unexpected("publish metadata file failed");
  }
  if (::unlinkat(directory.get(), temporary_name.c_str(), 0) != 0) {
    return std::unexpected("remove metadata temporary name failed");
  }
  if (::fsync(directory.get()) != 0) {
    return std::unexpected("sync metadata directory failed");
  }
  return {};
}

[[nodiscard]] std::expected<std::string, std::string>
read_metadata_bytes(const std::filesystem::path &staging_root) {
  FileDescriptor directory{::open(
      staging_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.get() < 0) {
    return std::unexpected("open metadata directory failed");
  }

  const std::string metadata_name{kMetadataFilename};
  FileDescriptor metadata{::openat(directory.get(), metadata_name.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (metadata.get() < 0) {
    return std::unexpected("open capture metadata failed");
  }

  struct stat status{};
  if (::fstat(metadata.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || status.st_nlink != 1 ||
      (status.st_mode & 0777) != 0600 || status.st_size <= 0 ||
      static_cast<std::uintmax_t>(status.st_size) > kMaximumMetadataBytes) {
    return std::unexpected("capture metadata is unavailable or unsafe");
  }

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096U> buffer{};
  while (true) {
    const auto count = ::read(metadata.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected("read capture metadata failed");
    }
    if (count == 0) {
      break;
    }
    const auto byte_count = static_cast<std::size_t>(count);
    if (bytes.size() > kMaximumMetadataBytes - byte_count) {
      return std::unexpected("capture metadata exceeds the configured limit");
    }
    bytes.append(buffer.data(), byte_count);
  }
  if (bytes.size() != static_cast<std::size_t>(status.st_size)) {
    return std::unexpected("capture metadata changed while being read");
  }
  return bytes;
}

template <std::size_t Size>
[[nodiscard]] bool
has_exact_keys(const Json &object,
               const std::array<std::string_view, Size> &keys) {
  return object.is_object() && object.size() == keys.size() &&
         std::ranges::all_of(keys, [&](std::string_view key) {
           return object.contains(std::string{key});
         });
}

[[nodiscard]] bool read_bounded_string(const Json &object, std::string_view key,
                                       std::string &destination) {
  const auto iterator = object.find(std::string{key});
  if (iterator == object.end() || !iterator->is_string()) {
    return false;
  }
  const auto &value = iterator->get_ref<const std::string &>();
  if (value.empty() || value.size() > kMaximumMetadataFieldBytes ||
      value.find('\0') != std::string::npos) {
    return false;
  }
  destination = value;
  return true;
}

struct SnapshotMetadata {
  std::string source_image_identity;
  RuntimeFingerprint runtime_fingerprint;
};

[[nodiscard]] std::expected<SnapshotMetadata, std::string>
parse_metadata(std::string_view bytes) {
  bool duplicate_key = false;
  std::vector<std::set<std::string>> object_keys;
  const auto callback = [&](int, Json::parse_event_t event,
                            Json &parsed) mutable {
    if (event == Json::parse_event_t::object_start) {
      object_keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (object_keys.empty() ||
          !object_keys.back().insert(parsed.get<std::string>()).second) {
        duplicate_key = true;
      }
    } else if (event == Json::parse_event_t::object_end &&
               !object_keys.empty()) {
      object_keys.pop_back();
    }
    return true;
  };
  const Json document =
      Json::parse(bytes.begin(), bytes.end(), callback, false, false);
  if (document.is_discarded() || duplicate_key) {
    return std::unexpected("capture metadata JSON is malformed");
  }

  constexpr std::array<std::string_view, 3> root_keys{
      "schema_version",
      "source_image_identity",
      "runtime_fingerprint",
  };
  constexpr std::array<std::string_view, 20> fingerprint_keys{
      "runtime_name",         "runtime_version",     "runtime_build_origin",
      "architecture",         "kernel_release",      "criu_version",
      "oci_runtime_name",     "oci_runtime_version", "oci_runtime_commit",
      "oci_runtime_features", "cgroup_version",      "cgroup_manager",
      "cgroup_controllers",   "privilege_mode",      "user_namespace_mode",
      "capability_profile",   "selinux_mode",        "apparmor_profile",
      "seccomp_profile",      "command_profile",
  };
  if (!has_exact_keys(document, root_keys)) {
    return std::unexpected("capture metadata has an invalid root shape");
  }
  const auto &schema = document.at("schema_version");
  const auto &fingerprint = document.at("runtime_fingerprint");
  if (!schema.is_number_unsigned() ||
      schema.get<std::uint64_t>() != kMetadataSchemaVersion ||
      !has_exact_keys(fingerprint, fingerprint_keys)) {
    return std::unexpected("capture metadata has an unsupported schema");
  }

  SnapshotMetadata metadata;
  if (!read_bounded_string(document, "source_image_identity",
                           metadata.source_image_identity) ||
      !read_bounded_string(fingerprint, "runtime_name",
                           metadata.runtime_fingerprint.runtime_name) ||
      !read_bounded_string(fingerprint, "runtime_version",
                           metadata.runtime_fingerprint.runtime_version) ||
      !read_bounded_string(fingerprint, "runtime_build_origin",
                           metadata.runtime_fingerprint.runtime_build_origin) ||
      !read_bounded_string(fingerprint, "architecture",
                           metadata.runtime_fingerprint.architecture) ||
      !read_bounded_string(fingerprint, "kernel_release",
                           metadata.runtime_fingerprint.kernel_release) ||
      !read_bounded_string(fingerprint, "criu_version",
                           metadata.runtime_fingerprint.criu_version) ||
      !read_bounded_string(fingerprint, "oci_runtime_name",
                           metadata.runtime_fingerprint.oci_runtime_name) ||
      !read_bounded_string(fingerprint, "oci_runtime_version",
                           metadata.runtime_fingerprint.oci_runtime_version) ||
      !read_bounded_string(fingerprint, "oci_runtime_commit",
                           metadata.runtime_fingerprint.oci_runtime_commit) ||
      !read_bounded_string(fingerprint, "oci_runtime_features",
                           metadata.runtime_fingerprint.oci_runtime_features) ||
      !read_bounded_string(fingerprint, "cgroup_version",
                           metadata.runtime_fingerprint.cgroup_version) ||
      !read_bounded_string(fingerprint, "cgroup_manager",
                           metadata.runtime_fingerprint.cgroup_manager) ||
      !read_bounded_string(fingerprint, "cgroup_controllers",
                           metadata.runtime_fingerprint.cgroup_controllers) ||
      !read_bounded_string(fingerprint, "privilege_mode",
                           metadata.runtime_fingerprint.privilege_mode) ||
      !read_bounded_string(fingerprint, "user_namespace_mode",
                           metadata.runtime_fingerprint.user_namespace_mode) ||
      !read_bounded_string(fingerprint, "capability_profile",
                           metadata.runtime_fingerprint.capability_profile) ||
      !read_bounded_string(fingerprint, "selinux_mode",
                           metadata.runtime_fingerprint.selinux_mode) ||
      !read_bounded_string(fingerprint, "apparmor_profile",
                           metadata.runtime_fingerprint.apparmor_profile) ||
      !read_bounded_string(fingerprint, "seccomp_profile",
                           metadata.runtime_fingerprint.seccomp_profile) ||
      !read_bounded_string(fingerprint, "command_profile",
                           metadata.runtime_fingerprint.command_profile)) {
    return std::unexpected(
        "capture metadata contains an invalid fingerprint field");
  }
  return metadata;
}

[[nodiscard]] std::expected<SnapshotMetadata, std::string>
read_metadata(const std::filesystem::path &staging_root) {
  const auto bytes = read_metadata_bytes(staging_root);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return parse_metadata(*bytes);
}

void print_usage() {
  std::cerr
      << "usage: podman_adapter_smoke_driver SUBCOMMAND --podman PATH "
         "--criu PATH --staging-root PATH --logical-id ID --container NAME "
         "[--image IMAGE | --artifact PATH | --expected-runtime-id ID]\n"
      << "subcommands: cold-start, capture, stop-remove, restore, cleanup\n";
}

[[nodiscard]] int report_adapter_error(const CheckpointErrorDetail &error) {
  std::cerr << "adapter operation failed (code="
            << static_cast<unsigned int>(error.code)
            << "): " << error.diagnostic << '\n';
  return 1;
}

[[nodiscard]] int run(int argc, char **argv) {
  const auto parsed = parse_command_line(argc, argv);
  if (!parsed) {
    std::cerr << "invalid command line: " << parsed.error() << '\n';
    print_usage();
    return 2;
  }

  const auto podman = canonical_executable(parsed->podman);
  const auto criu = canonical_executable(parsed->criu);
  const auto staging_root = canonical_directory(parsed->staging_root);
  const auto environment = inherited_environment();
  if (!podman || !criu || !staging_root || !environment) {
    const std::string diagnostic = !podman         ? podman.error()
                                   : !criu         ? criu.error()
                                   : !staging_root ? staging_root.error()
                                                   : environment.error();
    std::cerr << "invalid operational configuration: " << diagnostic << '\n';
    return 2;
  }

  const auto container_id = ContainerId::create(parsed->logical_id);
  const auto runtime_container = RuntimeContainerRef::create(parsed->container);
  if (!container_id || !runtime_container) {
    std::cerr << "invalid logical or runtime container identity\n";
    return 2;
  }

  PodmanAdapterConfig config;
  config.podman_executable = *podman;
  config.criu_executable = *criu;
  config.staging_root = *staging_root;
  config.environment = *environment;

  PosixProcessRunner process_runner;
  LinuxAffinityController affinity_controller;
  StderrLogger logger;
  PodmanAdapter adapter(process_runner, affinity_controller, logger,
                        std::move(config));

  if (parsed->subcommand == "cold-start") {
    const ColdStartSpec spec{
        .image_reference = *parsed->image,
        .argv = {"--threads", "2", "--interval-ms", "250"},
        .environment = {},
        .mounts = {},
        .runtime_options = {},
    };
    const auto started = adapter.cold_start(*container_id, *runtime_container,
                                            spec, kCpuAssignment);
    if (!started) {
      return report_adapter_error(started.error());
    }
    std::cout << "runtime_id=" << started->runtime_id << '\n';
    return 0;
  }

  if (parsed->subcommand == "capture") {
    const auto staging_directory = *staging_root / kStagingDirectoryName;
    const auto canonical_staging =
        canonical_directory(staging_directory.string());
    if (!canonical_staging) {
      std::cerr << "invalid capture staging directory: "
                << canonical_staging.error() << '\n';
      return 2;
    }
    const auto captured = adapter.capture(*container_id, *runtime_container,
                                          *canonical_staging, kCpuAssignment);
    if (!captured) {
      return report_adapter_error(captured.error());
    }
    const auto expected_artifact =
        *canonical_staging / PodmanAdapter::checkpoint_artifact_name;
    if (captured->artifact_path != expected_artifact) {
      std::cerr << "adapter returned an unexpected checkpoint artifact path\n";
      return 1;
    }
    if (const auto written = write_metadata(*staging_root, *captured);
        !written) {
      std::cerr << "capture metadata publication failed: " << written.error()
                << '\n';
      return 1;
    }
    std::cout << "artifact=" << captured->artifact_path.string() << '\n';
    return 0;
  }

  if (parsed->subcommand == "stop-remove") {
    const RuntimeIncarnation expected_incarnation{
        .reference = *runtime_container,
        .runtime_id = *parsed->expected_runtime_id,
    };
    const auto removed = adapter.stop_and_remove(
        *container_id, expected_incarnation, kCpuAssignment);
    if (!removed) {
      return report_adapter_error(removed.error());
    }
    return 0;
  }

  if (parsed->subcommand == "cleanup") {
    const auto cleaned = adapter.cleanup_failed_restore(
        *container_id, *runtime_container, kCpuAssignment);
    if (!cleaned) {
      return report_adapter_error(cleaned.error());
    }
    return 0;
  }

  const std::filesystem::path artifact{*parsed->artifact};
  if (!artifact.is_absolute() ||
      artifact.string().size() > kMaximumArgumentBytes ||
      artifact != artifact.lexically_normal()) {
    std::cerr << "restore artifact path is invalid\n";
    return 2;
  }
  const auto metadata = read_metadata(*staging_root);
  if (!metadata) {
    std::cerr << "capture metadata validation failed: " << metadata.error()
              << '\n';
    return 2;
  }
  const auto restored =
      adapter.restore(*container_id, *runtime_container, artifact,
                      metadata->runtime_fingerprint,
                      metadata->source_image_identity, kCpuAssignment);
  if (!restored) {
    return report_adapter_error(restored.error());
  }
  std::cout << "runtime_id=" << restored->runtime_id << '\n';
  return 0;
}

} // namespace
} // namespace gravesentinel::ccm

int main(int argc, char **argv) {
  try {
    return gravesentinel::ccm::run(argc, argv);
  } catch (const std::exception &) {
    std::cerr << "helper failed safely with an unexpected exception\n";
    return 1;
  } catch (...) {
    std::cerr << "helper failed safely with an unknown error\n";
    return 1;
  }
}
