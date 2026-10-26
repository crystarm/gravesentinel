#include "src/hypervisor/podman_adapter.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kPodmanVersion = "5.4.2";
constexpr std::string_view kArchitecture = "amd64";
constexpr std::string_view kOsArchitecture = "linux/amd64";
constexpr std::string_view kKernelRelease = "6.12.111+deb13-amd64";
constexpr std::string_view kCriuVersion = "4.1.1";
constexpr std::string_view kOciRuntimeVersion = "1.21";
constexpr std::string_view kOciRuntimeCommit =
    "10269840aa07fb7e6b7e1acff6198692d8ff5c88";
constexpr std::string_view kCapabilityProfile = "CapEff=000001ffffffffff";
constexpr std::string_view kCanonicalCgroupControllers =
    "cpu cpuset hugetlb io memory misc pids rdma";
constexpr std::string_view kCanonicalOciFeatures =
    "+APPARMOR +CAP +CRIU +SECCOMP +SELINUX";

[[nodiscard]] CheckpointErrorDetail
make_error(StableErrorCode code, std::string diagnostic, bool retryable = false,
           RuntimeSideEffect runtime_side_effect = RuntimeSideEffect::none) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = runtime_side_effect,
  };
}

class TransientEnvironmentFile final {
public:
  [[nodiscard]] static CheckpointResult<TransientEnvironmentFile>
  create(const std::filesystem::path &staging_root, std::string_view contents) {
    auto path_template = (staging_root / ".gravesentinel-env-XXXXXX").string();
    std::vector<char> path_buffer(path_template.begin(), path_template.end());
    path_buffer.push_back('\0');

    const auto descriptor = ::mkstemp(path_buffer.data());
    if (descriptor < 0) {
      return std::unexpected(
          make_error(StableErrorCode::storage_error,
                     "transient container environment could not be created"));
    }
    const std::filesystem::path path(path_buffer.data());
    const auto fail = [&](std::string diagnostic)
        -> CheckpointResult<TransientEnvironmentFile> {
      const auto saved_errno = errno;
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(path.c_str()));
      errno = saved_errno;
      return std::unexpected(
          make_error(StableErrorCode::storage_error, std::move(diagnostic)));
    };

    struct stat status{};
    const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_nlink != 1 || ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0 ||
        descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      return fail("transient container environment is unsafe");
    }

    std::size_t offset = 0U;
    while (offset < contents.size()) {
      const auto remaining = contents.size() - offset;
      const auto maximum_write =
          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
      const auto requested = std::min(remaining, maximum_write);
      const auto written =
          ::write(descriptor, contents.data() + offset, requested);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        return fail("transient container environment could not be written");
      }
      offset += static_cast<std::size_t>(written);
    }
    if (::close(descriptor) != 0) {
      static_cast<void>(::unlink(path.c_str()));
      return std::unexpected(
          make_error(StableErrorCode::storage_error,
                     "transient container environment could not be finalized"));
    }
    return TransientEnvironmentFile(path);
  }

  ~TransientEnvironmentFile() {
    if (present_) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  TransientEnvironmentFile(const TransientEnvironmentFile &) = delete;
  TransientEnvironmentFile &
  operator=(const TransientEnvironmentFile &) = delete;

  TransientEnvironmentFile(TransientEnvironmentFile &&other) noexcept
      : path_(std::move(other.path_)),
        present_(std::exchange(other.present_, false)) {}
  TransientEnvironmentFile &operator=(TransientEnvironmentFile &&) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

  [[nodiscard]] CheckpointResult<void> remove() {
    if (!present_) {
      return {};
    }
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
      return std::unexpected(
          make_error(StableErrorCode::storage_error,
                     "transient container environment could not be removed"));
    }
    present_ = false;
    return {};
  }

private:
  explicit TransientEnvironmentFile(std::filesystem::path path)
      : path_(std::move(path)) {}

  std::filesystem::path path_;
  bool present_{true};
};

[[nodiscard]] bool has_nul(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool is_ascii_alphanumeric(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return std::isalnum(byte) != 0;
}

[[nodiscard]] bool is_valid_environment_name(std::string_view name) noexcept {
  if (name.empty() || has_nul(name)) {
    return false;
  }
  const auto first = static_cast<unsigned char>(name.front());
  if (std::isalpha(first) == 0 && name.front() != '_') {
    return false;
  }
  return std::all_of(name.begin() + 1, name.end(), [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) != 0 || value == '_';
  });
}

[[nodiscard]] bool is_valid_runtime_reference(std::string_view value) noexcept {
  if (value.empty() || value.size() > 255U || has_nul(value) ||
      !is_ascii_alphanumeric(value.front())) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return is_ascii_alphanumeric(character) || character == '_' ||
           character == '.' || character == '-';
  });
}

[[nodiscard]] bool is_valid_image_reference(std::string_view value) noexcept {
  if (value.empty() || value.size() > 4096U || has_nul(value) ||
      !is_ascii_alphanumeric(value.front())) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    return is_ascii_alphanumeric(character) || character == '_' ||
           character == '.' || character == '-' || character == ':' ||
           character == '/' || character == '@' || character == '+';
  });
}

[[nodiscard]] bool is_lower_hex(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

[[nodiscard]] bool is_valid_runtime_id(std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), is_lower_hex);
}

[[nodiscard]] std::optional<std::string>
canonical_image_id(std::string_view value) {
  constexpr std::string_view prefix = "sha256:";
  if (value.starts_with(prefix)) {
    value.remove_prefix(prefix.size());
  }
  if (value.size() != 64U ||
      !std::all_of(value.begin(), value.end(), is_lower_hex)) {
    return std::nullopt;
  }
  return std::string(prefix) + std::string(value);
}

[[nodiscard]] std::optional<std::string>
parse_runtime_id_output(std::string output) {
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  if (!is_valid_runtime_id(output)) {
    return std::nullopt;
  }
  return output;
}

[[nodiscard]] const Json *member(const Json &object,
                                 std::string_view name) noexcept {
  if (!object.is_object()) {
    return nullptr;
  }
  const auto iterator = object.find(name);
  if (iterator == object.end()) {
    return nullptr;
  }
  return &*iterator;
}

[[nodiscard]] std::optional<std::string> string_member(const Json &object,
                                                       std::string_view name) {
  const auto *value = member(object, name);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return value->get<std::string>();
}

[[nodiscard]] std::optional<bool> bool_member(const Json &object,
                                              std::string_view name) {
  const auto *value = member(object, name);
  if (value == nullptr || !value->is_boolean()) {
    return std::nullopt;
  }
  return value->get<bool>();
}

[[nodiscard]] std::optional<int> int_member(const Json &object,
                                            std::string_view name) {
  const auto *value = member(object, name);
  if (value == nullptr || !value->is_number_integer()) {
    return std::nullopt;
  }
  const auto number = value->get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() ||
      number > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(number);
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view value) {
  std::vector<std::string> lines;
  std::size_t begin = 0U;
  while (begin <= value.size()) {
    const auto end = value.find('\n', begin);
    auto line = std::string(value.substr(begin, end == std::string_view::npos
                                                    ? value.size() - begin
                                                    : end - begin));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return lines;
}

[[nodiscard]] std::set<std::string> whitespace_tokens(std::string_view value) {
  std::set<std::string> tokens;
  std::size_t begin = 0U;
  while (begin < value.size()) {
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
      ++begin;
    }
    auto end = begin;
    while (end < value.size() &&
           std::isspace(static_cast<unsigned char>(value[end])) == 0) {
      ++end;
    }
    if (begin < end) {
      tokens.emplace(value.substr(begin, end - begin));
    }
    begin = end;
  }
  return tokens;
}

[[nodiscard]] bool has_qualified_capabilities(const Json &security) {
  const auto *capabilities = member(security, "capabilities");
  if (capabilities == nullptr) {
    return false;
  }
  if (capabilities->is_string() &&
      capabilities->get<std::string>() == kCapabilityProfile) {
    return true;
  }

  std::set<std::string> observed;
  if (capabilities->is_array()) {
    for (const auto &capability : *capabilities) {
      if (!capability.is_string()) {
        return false;
      }
      observed.insert(capability.get<std::string>());
    }
  } else if (capabilities->is_string()) {
    auto text = capabilities->get<std::string>();
    std::replace(text.begin(), text.end(), ',', ' ');
    observed = whitespace_tokens(text);
  } else {
    return false;
  }

  static const std::set<std::string> required{
      "CAP_CHOWN",   "CAP_DAC_OVERRIDE",     "CAP_FOWNER",     "CAP_FSETID",
      "CAP_KILL",    "CAP_NET_BIND_SERVICE", "CAP_SETFCAP",    "CAP_SETGID",
      "CAP_SETPCAP", "CAP_SETUID",           "CAP_SYS_CHROOT",
  };
  return observed == required;
}

[[nodiscard]] bool path_is_within(const std::filesystem::path &child,
                                  const std::filesystem::path &parent) {
  auto child_part = child.begin();
  auto parent_part = parent.begin();
  for (; parent_part != parent.end(); ++parent_part, ++child_part) {
    if (child_part == child.end() || *child_part != *parent_part) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] CheckpointErrorDetail
with_runtime_side_effect(CheckpointErrorDetail error) {
  error.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
  return error;
}

struct ContainerInspection {
  std::string runtime_id;
  std::string image_identity;
  bool running;
  int exit_code;
  std::string cpuset;
  std::string managed;
  std::string logical_id;
};

struct ContainerPresence {
  bool normal;
  bool external;
};

} // namespace

class PodmanAdapter::Impl {
public:
  Impl(IProcessRunner &process_runner, IAffinityController &affinity_controller,
       ILogger &logger, PodmanAdapterConfig config)
      : process_runner_(process_runner),
        affinity_controller_(affinity_controller), logger_(logger),
        config_(std::move(config)) {}

  [[nodiscard]] CheckpointResult<void>
  validate_restore_compatibility(const RuntimeFingerprint &snapshot_fingerprint,
                                 std::string_view source_image_identity,
                                 RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<void>(cpu_assignment, "validate compatibility",
                               [&]() -> CheckpointResult<void> {
                                 return validate_restore_compatibility_unbound(
                                     snapshot_fingerprint,
                                     source_image_identity, cpu_assignment);
                               });
  }

  [[nodiscard]] CheckpointResult<CapturedSnapshot>
  capture(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &staging_directory,
          RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<CapturedSnapshot>(
        cpu_assignment, "capture", [&]() -> CheckpointResult<CapturedSnapshot> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(reference.error())};
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(logical_id.error())};
          }
          const auto staging = validate_staging_directory(staging_directory);
          if (!staging) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(staging.error())};
          }
          const auto artifact =
              *staging / std::string(checkpoint_artifact_name);
          if (const auto available = validate_new_artifact_path(artifact);
              !available) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(available.error())};
          }

          const auto fingerprint = inspect_runtime_profile(cpu_assignment);
          if (!fingerprint) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(fingerprint.error())};
          }
          const auto before = inspect_container(runtime_container.value(),
                                                StableErrorCode::capture_failed,
                                                cpu_assignment);
          if (!before) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(before.error())};
          }
          if (const auto valid = validate_owned_container(
                  *before, container_id, cpu_assignment.container_cpu, true);
              !valid) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(valid.error())};
          }

          const auto checkpoint = run_checked(
              config_.podman_executable,
              {"container", "checkpoint", "--compress=none", "--leave-running",
               "--export", artifact.string(), runtime_container.value()},
              config_.operation_timeout, StableErrorCode::capture_failed,
              "checkpoint command", cpu_assignment.checkpoint_cpu,
              config_.environment);
          if (!checkpoint) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(checkpoint.error())};
          }
          if (const auto exported = validate_existing_artifact(artifact);
              !exported) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(exported.error())};
          }

          const auto after = inspect_container(runtime_container.value(),
                                               StableErrorCode::capture_failed,
                                               cpu_assignment);
          if (!after) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(after.error())};
          }
          if (const auto valid = validate_owned_container(
                  *after, container_id, cpu_assignment.container_cpu, true);
              !valid) {
            return CheckpointResult<CapturedSnapshot>{
                std::unexpected(valid.error())};
          }
          if (after->runtime_id != before->runtime_id ||
              after->image_identity != before->image_identity) {
            return CheckpointResult<CapturedSnapshot>{std::unexpected(
                make_error(StableErrorCode::capture_failed,
                           "source incarnation changed while checkpointing"))};
          }

          return CapturedSnapshot{
              .artifact_path = artifact,
              .source_image_identity = before->image_identity,
              .runtime_fingerprint = *fingerprint,
              .source_incarnation =
                  RuntimeIncarnation{
                      .reference = runtime_container,
                      .runtime_id = before->runtime_id,
                  },
          };
        });
  }

  [[nodiscard]] CheckpointResult<RuntimeObservation>
  inspect_runtime(const ContainerId &container_id,
                  const RuntimeContainerRef &runtime_container,
                  RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<RuntimeObservation>(
        cpu_assignment, "inspect runtime",
        [&]() -> CheckpointResult<RuntimeObservation> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return std::unexpected(reference.error());
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return std::unexpected(logical_id.error());
          }
          const auto presence =
              container_presence(runtime_container.value(), cpu_assignment,
                                 StableErrorCode::unavailable);
          if (!presence) {
            return std::unexpected(presence.error());
          }
          if (!presence->normal) {
            if (presence->external) {
              return std::unexpected(make_error(
                  StableErrorCode::conflict,
                  "an external container prevents runtime reconciliation"));
            }
            return RuntimeObservation{
                .status = RuntimeStatus{.state = RuntimeState::absent,
                                        .exit_code = std::nullopt},
                .incarnation = std::nullopt,
            };
          }
          const auto inspection =
              inspect_container(runtime_container.value(),
                                StableErrorCode::unavailable, cpu_assignment);
          if (!inspection) {
            return std::unexpected(inspection.error());
          }
          if (const auto valid =
                  validate_owned_container(*inspection, container_id,
                                           cpu_assignment.container_cpu, false);
              !valid) {
            return std::unexpected(valid.error());
          }
          return RuntimeObservation{
              .status =
                  RuntimeStatus{
                      .state = inspection->running ? RuntimeState::running
                                                   : RuntimeState::exited,
                      .exit_code =
                          inspection->running
                              ? std::nullopt
                              : std::optional<int>{inspection->exit_code},
                  },
              .incarnation =
                  RuntimeIncarnation{
                      .reference = runtime_container,
                      .runtime_id = inspection->runtime_id,
                  },
          };
        });
  }

  [[nodiscard]] CheckpointResult<void>
  stop_and_remove(const ContainerId &container_id,
                  const RuntimeIncarnation &expected_incarnation,
                  RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<void>(
        cpu_assignment, "stop and remove", [&]() -> CheckpointResult<void> {
          if (const auto reference =
                  validate_runtime_reference(expected_incarnation.reference);
              !reference) {
            return reference;
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return logical_id;
          }
          if (!is_valid_runtime_id(expected_incarnation.runtime_id)) {
            return std::unexpected(make_error(
                StableErrorCode::invalid_argument,
                "expected runtime incarnation identifier is invalid"));
          }

          const auto before =
              inspect_container(expected_incarnation.reference.value(),
                                StableErrorCode::not_found, cpu_assignment);
          if (!before) {
            return CheckpointResult<void>{std::unexpected(before.error())};
          }
          if (const auto valid = validate_owned_container(
                  *before, container_id, cpu_assignment.container_cpu, false);
              !valid) {
            return valid;
          }
          if (before->runtime_id != expected_incarnation.runtime_id) {
            return std::unexpected(make_error(
                StableErrorCode::conflict,
                "runtime incarnation was replaced before container stop"));
          }
          const auto indeterminate =
              [](CheckpointErrorDetail error) -> CheckpointResult<void> {
            error.runtime_side_effect =
                RuntimeSideEffect::incarnation_may_exist;
            return std::unexpected(std::move(error));
          };

          auto stopped_container = *before;
          if (before->running) {
            if (const auto stopped =
                    stop_container(before->runtime_id, cpu_assignment);
                !stopped) {
              return indeterminate(stopped.error());
            }

            const auto inspected =
                inspect_container(before->runtime_id,
                                  StableErrorCode::unavailable, cpu_assignment);
            if (!inspected) {
              return indeterminate(inspected.error());
            }
            stopped_container = *inspected;
            if (const auto valid = validate_owned_container(
                    stopped_container, container_id,
                    cpu_assignment.container_cpu, false);
                !valid) {
              return indeterminate(valid.error());
            }
            if (stopped_container.runtime_id != before->runtime_id) {
              return indeterminate(make_error(
                  StableErrorCode::conflict,
                  "runtime incarnation changed before container removal"));
            }
          }
          if (const auto removed =
                  remove_container(before->runtime_id, cpu_assignment);
              !removed) {
            return indeterminate(removed.error());
          }
          const auto absent =
              prove_absence(expected_incarnation.reference.value(),
                            cpu_assignment, StableErrorCode::unavailable);
          if (!absent) {
            return indeterminate(absent.error());
          }
          return {};
        });
  }

  [[nodiscard]] CheckpointResult<void>
  confirm_absence(const ContainerId &container_id,
                  const RuntimeContainerRef &runtime_container,
                  RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<void>(
        cpu_assignment, "confirm container absence",
        [&]() -> CheckpointResult<void> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return reference;
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return logical_id;
          }
          return prove_absence(runtime_container.value(), cpu_assignment,
                               StableErrorCode::conflict);
        });
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  restore(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &snapshot_artifact,
          const RuntimeFingerprint &snapshot_fingerprint,
          std::string_view source_image_identity,
          RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<RuntimeIncarnation>(
        cpu_assignment, "restore",
        [&]() -> CheckpointResult<RuntimeIncarnation> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(reference.error())};
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(logical_id.error())};
          }
          const auto artifact = validate_snapshot_artifact(snapshot_artifact);
          if (!artifact) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(artifact.error())};
          }
          if (const auto compatible = validate_restore_compatibility_unbound(
                  snapshot_fingerprint, source_image_identity, cpu_assignment);
              !compatible) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(compatible.error())};
          }
          const auto presence =
              container_presence(runtime_container.value(), cpu_assignment,
                                 StableErrorCode::restore_failed);
          if (!presence) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(presence.error())};
          }
          if (presence->normal || presence->external) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(make_error(StableErrorCode::already_exists,
                                           "restore target already exists"))};
          }

          const auto restored = run_checked(
              config_.podman_executable,
              {"container", "restore", "--import", artifact->string(), "--name",
               runtime_container.value()},
              config_.operation_timeout, StableErrorCode::restore_failed,
              "restore command", cpu_assignment.checkpoint_cpu,
              config_.environment);
          if (!restored) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(restored.error()))};
          }
          const auto restored_id =
              parse_runtime_id_output(restored->standard_output);
          if (!restored_id) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(make_error(
                    StableErrorCode::restore_failed,
                    "restore returned a malformed runtime identifier")))};
          }

          const auto target = inspect_container(runtime_container.value(),
                                                StableErrorCode::restore_failed,
                                                cpu_assignment);
          if (!target) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(target.error()))};
          }
          if (const auto valid = validate_owned_container(
                  *target, container_id, cpu_assignment.container_cpu, true);
              !valid) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(valid.error()))};
          }
          const auto expected_image = canonical_image_id(source_image_identity);
          if (!expected_image || target->image_identity != *expected_image ||
              target->runtime_id != *restored_id) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(
                    make_error(StableErrorCode::restore_failed,
                               "restored incarnation does not match the "
                               "requested snapshot")))};
          }

          return RuntimeIncarnation{
              .reference = runtime_container,
              .runtime_id = target->runtime_id,
          };
        });
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  cold_start(const ContainerId &container_id,
             const RuntimeContainerRef &runtime_container,
             const ColdStartSpec &spec, RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<RuntimeIncarnation>(
        cpu_assignment, "cold start",
        [&]() -> CheckpointResult<RuntimeIncarnation> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(reference.error())};
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(logical_id.error())};
          }
          if (!spec.mounts.empty() || !spec.runtime_options.empty()) {
            return CheckpointResult<RuntimeIncarnation>{std::unexpected(
                make_error(StableErrorCode::invalid_argument,
                           "command profile v1 does not support mounts or "
                           "runtime options"))};
          }
          if (!is_valid_image_reference(spec.image_reference)) {
            return CheckpointResult<RuntimeIncarnation>{std::unexpected(
                make_error(StableErrorCode::invalid_argument,
                           "cold-start image reference is invalid"))};
          }
          const auto environment_file_contents =
              cold_start_environment_file_contents(spec);
          if (!environment_file_contents) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(environment_file_contents.error())};
          }
          if (const auto arguments = validate_workload_arguments(
                  spec.argv, !spec.environment.empty());
              !arguments) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(arguments.error())};
          }

          const auto fingerprint = inspect_runtime_profile(cpu_assignment);
          if (!fingerprint) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(fingerprint.error())};
          }
          const auto image = inspect_image(spec.image_reference, cpu_assignment,
                                           StableErrorCode::cold_start_failed);
          if (!image) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(image.error())};
          }
          const auto presence =
              container_presence(runtime_container.value(), cpu_assignment,
                                 StableErrorCode::cold_start_failed);
          if (!presence) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(presence.error())};
          }
          if (presence->normal || presence->external) {
            return CheckpointResult<RuntimeIncarnation>{std::unexpected(
                make_error(StableErrorCode::already_exists,
                           "cold-start target already exists"))};
          }

          std::optional<TransientEnvironmentFile> environment_file;
          if (!environment_file_contents->empty()) {
            const auto staging_root = canonical_staging_root();
            if (!staging_root) {
              return CheckpointResult<RuntimeIncarnation>{
                  std::unexpected(staging_root.error())};
            }
            auto created = TransientEnvironmentFile::create(
                *staging_root, *environment_file_contents);
            if (!created) {
              return CheckpointResult<RuntimeIncarnation>{
                  std::unexpected(created.error())};
            }
            environment_file.emplace(std::move(*created));
          }

          std::vector<std::string> arguments{
              "run",
              "--detach",
              "--name",
              runtime_container.value(),
              "--label",
              std::string(managed_label) + "=true",
              "--label",
              std::string(logical_id_label) + "=" + container_id.value(),
              "--network=none",
              "--read-only",
              "--cap-drop=all",
              "--passwd=false",
              "--security-opt=no-new-privileges",
              "--cpuset-cpus=" + std::to_string(cpu_assignment.container_cpu),
          };
          if (environment_file) {
            arguments.emplace_back("--env-file");
            arguments.push_back(environment_file->path().string());
          }
          arguments.push_back(spec.image_reference);
          arguments.insert(arguments.end(), spec.argv.begin(), spec.argv.end());

          const auto started = run_checked(
              config_.podman_executable, std::move(arguments),
              config_.operation_timeout, StableErrorCode::cold_start_failed,
              "cold-start command", cpu_assignment.checkpoint_cpu,
              config_.environment);
          const auto environment_removed = environment_file
                                               ? environment_file->remove()
                                               : CheckpointResult<void>{};
          if (!environment_removed) {
            return CheckpointResult<RuntimeIncarnation>{std::unexpected(
                with_runtime_side_effect(environment_removed.error()))};
          }
          if (!started) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(started.error()))};
          }
          const auto started_id =
              parse_runtime_id_output(started->standard_output);
          if (!started_id) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(make_error(
                    StableErrorCode::cold_start_failed,
                    "cold start returned a malformed runtime identifier")))};
          }

          const auto target = inspect_container(
              runtime_container.value(), StableErrorCode::cold_start_failed,
              cpu_assignment);
          if (!target) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(target.error()))};
          }
          if (const auto valid = validate_owned_container(
                  *target, container_id, cpu_assignment.container_cpu, true);
              !valid) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(valid.error()))};
          }
          if (target->runtime_id != *started_id ||
              target->image_identity != *image) {
            return CheckpointResult<RuntimeIncarnation>{
                std::unexpected(with_runtime_side_effect(make_error(
                    StableErrorCode::cold_start_failed,
                    "cold-started incarnation failed postcondition checks")))};
          }

          return RuntimeIncarnation{
              .reference = runtime_container,
              .runtime_id = target->runtime_id,
          };
        });
  }

  [[nodiscard]] CheckpointResult<void>
  cleanup_failed_restore(const ContainerId &container_id,
                         const RuntimeContainerRef &runtime_container,
                         RuntimeCpuAssignment cpu_assignment) {
    return with_affinity<void>(
        cpu_assignment, "restore cleanup", [&]() -> CheckpointResult<void> {
          if (const auto reference =
                  validate_runtime_reference(runtime_container);
              !reference) {
            return reference;
          }
          if (const auto logical_id = validate_logical_id(container_id);
              !logical_id) {
            return logical_id;
          }

          const auto presence =
              container_presence(runtime_container.value(), cpu_assignment,
                                 StableErrorCode::restore_failed);
          if (!presence) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(presence.error()))};
          }
          if (!presence->normal) {
            if (presence->external) {
              return CheckpointResult<void>{
                  std::unexpected(with_runtime_side_effect(make_error(
                      StableErrorCode::conflict,
                      "an external restore target prevents absence proof")))};
            }
            return {};
          }
          const auto before = inspect_container(runtime_container.value(),
                                                StableErrorCode::restore_failed,
                                                cpu_assignment);
          if (!before) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(before.error()))};
          }
          if (const auto valid = validate_owned_container(
                  *before, container_id, cpu_assignment.container_cpu, false);
              !valid) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(valid.error()))};
          }
          if (const auto stopped =
                  stop_container(before->runtime_id, cpu_assignment);
              !stopped) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(stopped.error()))};
          }

          const auto stopped_container = inspect_container(
              before->runtime_id, StableErrorCode::restore_failed,
              cpu_assignment);
          if (!stopped_container) {
            return CheckpointResult<void>{std::unexpected(
                with_runtime_side_effect(stopped_container.error()))};
          }
          if (const auto valid =
                  validate_owned_container(*stopped_container, container_id,
                                           cpu_assignment.container_cpu, false);
              !valid) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(valid.error()))};
          }
          if (stopped_container->runtime_id != before->runtime_id) {
            return std::unexpected(with_runtime_side_effect(make_error(
                StableErrorCode::conflict,
                "runtime incarnation changed during restore cleanup")));
          }
          if (const auto removed =
                  remove_container(before->runtime_id, cpu_assignment);
              !removed) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(removed.error()))};
          }
          const auto absent =
              prove_absence(runtime_container.value(), cpu_assignment,
                            StableErrorCode::restore_failed);
          if (!absent) {
            return CheckpointResult<void>{
                std::unexpected(with_runtime_side_effect(absent.error()))};
          }
          return {};
        });
  }

private:
  template <typename T, typename Function>
  [[nodiscard]] CheckpointResult<T>
  with_affinity(RuntimeCpuAssignment cpu_assignment, std::string_view operation,
                Function &&function) {
    if (const auto valid = validate_config(); !valid) {
      log_failure(operation);
      return std::unexpected(valid.error());
    }
    if (!is_standard_cpu_pair(cpu_assignment.container_cpu,
                              cpu_assignment.checkpoint_cpu)) {
      log_failure(operation);
      return std::unexpected(make_error(
          StableErrorCode::invalid_argument,
          "runtime CPU assignment is outside the qualified profile"));
    }
    if (const auto available =
            validate_cpu_available(cpu_assignment.container_cpu);
        !available) {
      log_failure(operation);
      return std::unexpected(available.error());
    }
    if (const auto available =
            validate_cpu_available(cpu_assignment.checkpoint_cpu);
        !available) {
      log_failure(operation);
      return std::unexpected(available.error());
    }

    auto binding =
        affinity_controller_.bind_current_thread(cpu_assignment.checkpoint_cpu);
    if (!binding || *binding == nullptr) {
      log_failure(operation);
      return std::unexpected(
          make_error(StableErrorCode::unavailable,
                     "checkpoint CPU affinity could not be established", true));
    }

    auto result = std::forward<Function>(function)();
    const auto restored = (*binding)->restore();
    if (!restored && result) {
      auto error = make_error(StableErrorCode::internal,
                              "checkpoint CPU affinity could not be restored");
      if (operation == "restore" || operation == "cold start") {
        error.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
      }
      result = std::unexpected(std::move(error));
    }
    if (!result) {
      log_failure(operation);
    }
    return result;
  }

  [[nodiscard]] CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const {
    const auto available =
        affinity_controller_.validate_cpu_available(logical_cpu);
    if (!available) {
      return std::unexpected(make_error(StableErrorCode::unavailable,
                                        "a required logical CPU is unavailable",
                                        true));
    }
    return {};
  }

  void log_failure(std::string_view operation) noexcept {
    const std::array<LogField, 1> fields{
        LogField{.key = "operation", .value = operation}};
    logger_.log(LogLevel::error, "Podman adapter operation failed", fields);
  }

  [[nodiscard]] CheckpointResult<void> validate_config() const {
    const auto valid_path = [](const std::filesystem::path &path) {
      const auto text = path.string();
      return path.is_absolute() && !text.empty() && !has_nul(text);
    };
    if (!valid_path(config_.podman_executable) ||
        !valid_path(config_.criu_executable) ||
        !valid_path(config_.staging_root)) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "adapter executable and staging paths must be absolute"));
    }
    if (config_.profile_timeout <= std::chrono::milliseconds::zero() ||
        config_.inspection_timeout <= std::chrono::milliseconds::zero() ||
        config_.operation_timeout <= std::chrono::milliseconds::zero() ||
        config_.cleanup_timeout <= std::chrono::milliseconds::zero() ||
        config_.termination_grace_period < std::chrono::milliseconds::zero()) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "adapter timeouts are outside the supported range"));
    }
    if (config_.maximum_json_bytes == 0U ||
        config_.maximum_standard_output_bytes < config_.maximum_json_bytes ||
        config_.maximum_standard_error_bytes == 0U ||
        config_.maximum_arguments == 0U ||
        config_.maximum_argument_bytes == 0U ||
        config_.maximum_environment_variables == 0U ||
        config_.maximum_environment_value_bytes == 0U ||
        config_.environment.size() > config_.maximum_environment_variables) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "adapter process limits are invalid"));
    }

    std::unordered_set<std::string> names;
    for (const auto &variable : config_.environment) {
      if (!is_valid_environment_name(variable.name) ||
          variable.value.size() > config_.maximum_environment_value_bytes ||
          has_nul(variable.value) || !names.insert(variable.name).second ||
          variable.name == "CONTAINER_HOST" ||
          variable.name == "CONTAINER_CONNECTION") {
        return std::unexpected(
            make_error(StableErrorCode::invalid_argument,
                       "adapter process environment is invalid"));
      }
    }
    const auto staging_root = canonical_staging_root();
    if (!staging_root) {
      return std::unexpected(staging_root.error());
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  validate_runtime_reference(const RuntimeContainerRef &reference) const {
    if (!is_valid_runtime_reference(reference.value())) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "runtime container reference is invalid"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  validate_logical_id(const ContainerId &container_id) const {
    if (container_id.value().size() > 255U || has_nul(container_id.value())) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "logical container ID is invalid"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<std::filesystem::path>
  canonical_staging_root() const {
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(config_.staging_root, error);
    if (error || status.type() != std::filesystem::file_type::directory) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "configured staging root is unavailable or unsafe"));
    }
    auto canonical = std::filesystem::canonical(config_.staging_root, error);
    struct stat native_status{};
    if (error || canonical != config_.staging_root.lexically_normal() ||
        ::lstat(canonical.c_str(), &native_status) != 0 ||
        !S_ISDIR(native_status.st_mode) ||
        native_status.st_uid != ::geteuid() ||
        (native_status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "configured staging root is unavailable or unsafe"));
    }
    return canonical;
  }

  [[nodiscard]] CheckpointResult<std::filesystem::path>
  validate_staging_directory(const std::filesystem::path &directory) const {
    if (!directory.is_absolute() || has_nul(directory.string())) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "staging directory is invalid"));
    }
    const auto root = canonical_staging_root();
    if (!root) {
      return std::unexpected(root.error());
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error || status.type() != std::filesystem::file_type::directory) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "staging directory is unsafe"));
    }
    auto canonical = std::filesystem::canonical(directory, error);
    if (error || canonical != directory.lexically_normal() ||
        !path_is_within(canonical, *root)) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "staging directory is unsafe"));
    }
    return canonical;
  }

  [[nodiscard]] CheckpointResult<void>
  validate_new_artifact_path(const std::filesystem::path &artifact) const {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(artifact, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
      return std::unexpected(
          make_error(StableErrorCode::conflict,
                     "checkpoint export path is not exclusively available"));
    }
    if (error && error != std::errc::no_such_file_or_directory) {
      return std::unexpected(
          make_error(StableErrorCode::storage_error,
                     "checkpoint export path could not be validated"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  validate_existing_artifact(const std::filesystem::path &artifact) const {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(artifact, error);
    if (error || status.type() != std::filesystem::file_type::regular) {
      return std::unexpected(
          make_error(StableErrorCode::corrupted_snapshot,
                     "checkpoint artifact is not a regular non-symlink file"));
    }
    const auto size = std::filesystem::file_size(artifact, error);
    if (error || size == 0U) {
      return std::unexpected(
          make_error(StableErrorCode::corrupted_snapshot,
                     "checkpoint artifact is empty or unreadable"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<std::filesystem::path>
  validate_snapshot_artifact(const std::filesystem::path &artifact) const {
    if (!artifact.is_absolute() || has_nul(artifact.string())) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "snapshot artifact path is invalid"));
    }
    const auto valid = validate_existing_artifact(artifact);
    if (!valid) {
      return std::unexpected(valid.error());
    }
    const auto root = canonical_staging_root();
    if (!root) {
      return std::unexpected(root.error());
    }
    std::error_code error;
    const auto canonical = std::filesystem::canonical(artifact, error);
    if (error || canonical != artifact.lexically_normal() ||
        !path_is_within(canonical, *root)) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "snapshot artifact path is unsafe"));
    }
    return canonical;
  }

  [[nodiscard]] CheckpointResult<ProcessResult>
  run_raw(const std::filesystem::path &executable,
          std::vector<std::string> arguments, std::chrono::milliseconds timeout,
          StableErrorCode failure_code, std::string_view phase,
          std::uint32_t logical_cpu,
          const std::vector<EnvironmentVariable> &environment) {
    if (arguments.size() > config_.maximum_arguments ||
        environment.size() > config_.maximum_environment_variables ||
        std::any_of(arguments.begin(), arguments.end(), [&](const auto &value) {
          return value.size() > config_.maximum_argument_bytes ||
                 has_nul(value);
        })) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "process request exceeds configured argument limits"));
    }

    const ProcessRequest request{
        .executable = executable,
        .argv = std::move(arguments),
        .environment = environment,
        .working_directory = std::nullopt,
        .logical_cpu = logical_cpu,
        .timeout = timeout,
        .termination_grace_period = config_.termination_grace_period,
        .maximum_standard_output_bytes = config_.maximum_standard_output_bytes,
        .maximum_standard_error_bytes = config_.maximum_standard_error_bytes,
    };
    const auto result = process_runner_.run(request);
    if (!result) {
      const auto code =
          result.error().code == StableErrorCode::deadline_exceeded
              ? StableErrorCode::deadline_exceeded
              : failure_code;
      return std::unexpected(
          make_error(code, std::string(phase) + " could not be executed",
                     result.error().retryable ||
                         code == StableErrorCode::deadline_exceeded));
    }
    if (result->standard_output_truncated || result->standard_error_truncated ||
        result->standard_output.size() >
            config_.maximum_standard_output_bytes ||
        result->standard_error.size() > config_.maximum_standard_error_bytes) {
      return std::unexpected(make_error(
          failure_code, std::string(phase) + " produced truncated output"));
    }
    if (result->termination == ProcessTermination::timed_out) {
      return std::unexpected(
          make_error(StableErrorCode::deadline_exceeded,
                     std::string(phase) + " exceeded its deadline", true));
    }
    if (result->termination != ProcessTermination::exited ||
        !result->exit_code.has_value() || result->signal_number.has_value()) {
      return std::unexpected(make_error(
          failure_code, std::string(phase) + " did not exit normally"));
    }
    return *result;
  }

  [[nodiscard]] CheckpointResult<ProcessResult>
  run_checked(const std::filesystem::path &executable,
              std::vector<std::string> arguments,
              std::chrono::milliseconds timeout, StableErrorCode failure_code,
              std::string_view phase, std::uint32_t logical_cpu,
              const std::vector<EnvironmentVariable> &environment) {
    auto result = run_raw(executable, std::move(arguments), timeout,
                          failure_code, phase, logical_cpu, environment);
    if (!result) {
      return result;
    }
    if (*result->exit_code != 0) {
      return std::unexpected(make_error(
          failure_code, std::string(phase) + " returned a nonzero status"));
    }
    return result;
  }

  [[nodiscard]] CheckpointResult<Json>
  parse_json_result(const ProcessResult &result, StableErrorCode failure_code,
                    std::string_view document_name) const {
    if (result.standard_output.empty() ||
        result.standard_output.size() > config_.maximum_json_bytes) {
      return std::unexpected(
          make_error(failure_code, std::string(document_name) +
                                       " JSON exceeds the configured limit"));
    }
    auto document = Json::parse(result.standard_output, nullptr, false, true);
    if (document.is_discarded()) {
      return std::unexpected(make_error(
          failure_code, std::string(document_name) + " JSON is malformed"));
    }
    return document;
  }

  [[nodiscard]] CheckpointResult<Json>
  run_json(const std::filesystem::path &executable,
           std::vector<std::string> arguments,
           std::chrono::milliseconds timeout, StableErrorCode failure_code,
           std::string_view document_name, std::uint32_t logical_cpu) {
    const auto result =
        run_checked(executable, std::move(arguments), timeout, failure_code,
                    document_name, logical_cpu, config_.environment);
    if (!result) {
      return std::unexpected(result.error());
    }
    return parse_json_result(*result, failure_code, document_name);
  }

  [[nodiscard]] CheckpointResult<RuntimeFingerprint>
  inspect_runtime_profile(RuntimeCpuAssignment cpu_assignment) {
    const auto version =
        run_json(config_.podman_executable, {"version", "--format=json"},
                 config_.profile_timeout, StableErrorCode::incompatible_runtime,
                 "Podman version", cpu_assignment.checkpoint_cpu);
    if (!version) {
      return std::unexpected(version.error());
    }
    const auto info =
        run_json(config_.podman_executable, {"info", "--format=json"},
                 config_.profile_timeout, StableErrorCode::incompatible_runtime,
                 "Podman info", cpu_assignment.checkpoint_cpu);
    if (!info) {
      return std::unexpected(info.error());
    }
    const auto criu = run_checked(
        config_.criu_executable, {"--version"}, config_.profile_timeout,
        StableErrorCode::incompatible_runtime, "CRIU version",
        cpu_assignment.checkpoint_cpu, config_.environment);
    if (!criu) {
      return std::unexpected(criu.error());
    }

    const auto *client = member(*version, "Client");
    const auto client_version =
        client == nullptr ? std::nullopt : string_member(*client, "Version");
    const auto client_architecture =
        client == nullptr ? std::nullopt : string_member(*client, "OsArch");
    const auto client_build_origin =
        client == nullptr ? std::nullopt
                          : string_member(*client, "BuildOrigin");
    if (!client_version || *client_version != kPodmanVersion ||
        !client_architecture || *client_architecture != kOsArchitecture ||
        !client_build_origin || *client_build_origin != "Debian") {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman version does not match the qualified profile"));
    }

    const auto *host = member(*info, "host");
    const auto *distribution =
        host == nullptr ? nullptr : member(*host, "distribution");
    const auto *security =
        host == nullptr ? nullptr : member(*host, "security");
    const auto *oci_runtime =
        host == nullptr ? nullptr : member(*host, "ociRuntime");
    const auto *id_mappings =
        host == nullptr ? nullptr : member(*host, "idMappings");
    const auto *info_version = member(*info, "version");
    if (host == nullptr || distribution == nullptr || security == nullptr ||
        oci_runtime == nullptr || id_mappings == nullptr ||
        info_version == nullptr) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman info is missing qualified profile fields"));
    }

    const auto distribution_name = string_member(*distribution, "distribution");
    const auto distribution_version = string_member(*distribution, "version");
    const auto distribution_codename = string_member(*distribution, "codename");
    const auto architecture = string_member(*host, "arch");
    const auto kernel = string_member(*host, "kernel");
    const auto cgroup_version = string_member(*host, "cgroupVersion");
    const auto cgroup_manager = string_member(*host, "cgroupManager");
    const auto service_is_remote = bool_member(*host, "serviceIsRemote");
    const auto rootless = bool_member(*security, "rootless");
    const auto apparmor_enabled = bool_member(*security, "apparmorEnabled");
    const auto seccomp_enabled = bool_member(*security, "seccompEnabled");
    const auto seccomp_path = string_member(*security, "seccompProfilePath");
    const auto selinux_enabled = bool_member(*security, "selinuxEnabled");
    const auto runtime_name = string_member(*oci_runtime, "name");
    const auto runtime_version = string_member(*oci_runtime, "version");
    const auto reported_version = string_member(*info_version, "Version");
    if (!distribution_name || *distribution_name != "debian" ||
        !distribution_version || *distribution_version != "13" ||
        !distribution_codename || *distribution_codename != "trixie") {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman distribution does not match qualified Debian 13 trixie"));
    }
    if (!architecture || *architecture != kArchitecture || !kernel ||
        *kernel != kKernelRelease) {
      return std::unexpected(make_error(StableErrorCode::incompatible_runtime,
                                        "Podman architecture or kernel does "
                                        "not match the qualified profile"));
    }
    if (!cgroup_version || *cgroup_version != "v2" || !cgroup_manager ||
        *cgroup_manager != "systemd") {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman cgroup mode does not match qualified v2/systemd"));
    }
    if (!service_is_remote || *service_is_remote) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman service must be local for the qualified profile"));
    }
    if (!rootless || *rootless) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman must be rootful for the qualified profile"));
    }
    if (!apparmor_enabled || !*apparmor_enabled) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman AppArmor support must be enabled for the qualified profile"));
    }
    if (!selinux_enabled || *selinux_enabled) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman SELinux mode does not match the qualified disabled profile"));
    }
    if (!seccomp_enabled || !*seccomp_enabled || !seccomp_path ||
        *seccomp_path != "/usr/share/containers/seccomp.json") {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman seccomp profile does not match the qualified default"));
    }
    if (!runtime_name || *runtime_name != "crun" || !runtime_version) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman OCI runtime does not match qualified crun"));
    }
    if (!reported_version || *reported_version != kPodmanVersion) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman info version does not match the qualified profile"));
    }
    if (!has_qualified_capabilities(*security)) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman capability profile does not match the qualified profile"));
    }

    const auto *uid_map = member(*id_mappings, "uidmap");
    const auto *gid_map = member(*id_mappings, "gidmap");
    if (uid_map == nullptr || gid_map == nullptr || !uid_map->is_null() ||
        !gid_map->is_null()) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman user namespace does not match the qualified profile"));
    }

    const auto *controllers = member(*host, "cgroupControllers");
    if (controllers == nullptr || !controllers->is_array()) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "Podman cgroup controller profile is malformed"));
    }
    std::set<std::string> observed_controllers;
    for (const auto &controller : *controllers) {
      if (!controller.is_string()) {
        return std::unexpected(
            make_error(StableErrorCode::incompatible_runtime,
                       "Podman cgroup controller profile is malformed"));
      }
      observed_controllers.insert(controller.get<std::string>());
    }
    const std::set<std::string> qualified_controllers{
        "cpu", "cpuset", "hugetlb", "io", "memory", "misc", "pids", "rdma"};
    if (controllers->size() != qualified_controllers.size() ||
        observed_controllers != qualified_controllers) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "Podman cgroup controllers do not match the qualified profile"));
    }

    const auto runtime_lines = split_lines(*runtime_version);
    if (runtime_lines.empty() ||
        runtime_lines.front() !=
            std::string("crun version ") + std::string(kOciRuntimeVersion)) {
      return std::unexpected(make_error(
          StableErrorCode::incompatible_runtime,
          "OCI runtime version does not match the qualified profile"));
    }
    const auto runtime_tokens = whitespace_tokens(*runtime_version);
    const std::array<std::string_view, 5> required_features{
        "+APPARMOR", "+CAP", "+CRIU", "+SECCOMP", "+SELINUX"};
    const auto commit_line =
        std::string("commit: ") + std::string(kOciRuntimeCommit);
    if (std::find(runtime_lines.begin(), runtime_lines.end(), commit_line) ==
            runtime_lines.end() ||
        std::any_of(required_features.begin(), required_features.end(),
                    [&](std::string_view feature) {
                      return !runtime_tokens.contains(std::string(feature));
                    })) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "OCI runtime build does not match the qualified profile"));
    }

    const auto criu_lines = split_lines(criu->standard_output);
    const auto criu_line = std::string("Version: ") + std::string(kCriuVersion);
    if (criu_lines.empty() || criu_lines.front() != criu_line) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "CRIU version does not match the qualified profile"));
    }

    return RuntimeFingerprint{
        .runtime_name = "podman",
        .runtime_version = std::string(kPodmanVersion),
        .runtime_build_origin = "Debian 13",
        .architecture = std::string(kArchitecture),
        .kernel_release = std::string(kKernelRelease),
        .criu_version = std::string(kCriuVersion),
        .oci_runtime_name = "crun",
        .oci_runtime_version = std::string(kOciRuntimeVersion),
        .oci_runtime_commit = std::string(kOciRuntimeCommit),
        .oci_runtime_features = std::string(kCanonicalOciFeatures),
        .cgroup_version = "v2",
        .cgroup_manager = "systemd",
        .cgroup_controllers = std::string(kCanonicalCgroupControllers),
        .privilege_mode = "rootful",
        .user_namespace_mode = "host",
        .capability_profile = std::string(kCapabilityProfile),
        .selinux_mode = "disabled",
        .apparmor_profile = "host-default",
        .seccomp_profile = "default",
        .command_profile = std::string(command_profile),
    };
  }

  [[nodiscard]] CheckpointResult<void> validate_restore_compatibility_unbound(
      const RuntimeFingerprint &snapshot_fingerprint,
      std::string_view source_image_identity,
      RuntimeCpuAssignment cpu_assignment) {
    const auto observed = inspect_runtime_profile(cpu_assignment);
    if (!observed) {
      return std::unexpected(observed.error());
    }
    if (*observed != snapshot_fingerprint) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "snapshot runtime fingerprint is incompatible"));
    }
    const auto expected_image = canonical_image_id(source_image_identity);
    if (!expected_image) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "snapshot source image identity is malformed"));
    }
    const auto observed_image =
        inspect_image(source_image_identity, cpu_assignment,
                      StableErrorCode::incompatible_runtime);
    if (!observed_image) {
      return std::unexpected(observed_image.error());
    }
    if (*observed_image != *expected_image) {
      return std::unexpected(
          make_error(StableErrorCode::incompatible_runtime,
                     "snapshot source image is unavailable or incompatible"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<ContainerInspection>
  inspect_container(std::string_view reference, StableErrorCode failure_code,
                    RuntimeCpuAssignment cpu_assignment) {
    const auto document = run_json(
        config_.podman_executable,
        {"container", "inspect", "--format=json", std::string(reference)},
        config_.inspection_timeout, failure_code, "container inspect",
        cpu_assignment.checkpoint_cpu);
    if (!document) {
      return std::unexpected(document.error());
    }
    if (!document->is_array() || document->size() != 1U ||
        !document->front().is_object()) {
      return std::unexpected(make_error(
          failure_code, "container inspect JSON has an invalid shape"));
    }
    const auto &object = document->front();
    const auto runtime_id = string_member(object, "Id");
    const auto image = string_member(object, "Image");
    const auto *state = member(object, "State");
    const auto *container_config = member(object, "Config");
    const auto *host_config = member(object, "HostConfig");
    const auto *labels = container_config == nullptr
                             ? nullptr
                             : member(*container_config, "Labels");
    const auto running =
        state == nullptr ? std::nullopt : bool_member(*state, "Running");
    const auto exit_code =
        state == nullptr ? std::nullopt : int_member(*state, "ExitCode");
    const auto cpuset = host_config == nullptr
                            ? std::nullopt
                            : string_member(*host_config, "CpusetCpus");
    const auto managed = labels == nullptr
                             ? std::nullopt
                             : string_member(*labels, managed_label);
    const auto logical_id = labels == nullptr
                                ? std::nullopt
                                : string_member(*labels, logical_id_label);
    const auto canonical_image =
        image ? canonical_image_id(*image) : std::nullopt;
    if (!runtime_id || !is_valid_runtime_id(*runtime_id) || !canonical_image ||
        !running || !exit_code || !cpuset || !managed || !logical_id) {
      return std::unexpected(make_error(
          failure_code, "container inspect JSON is incomplete or malformed"));
    }
    return ContainerInspection{
        .runtime_id = *runtime_id,
        .image_identity = *canonical_image,
        .running = *running,
        .exit_code = *exit_code,
        .cpuset = *cpuset,
        .managed = *managed,
        .logical_id = *logical_id,
    };
  }

  [[nodiscard]] CheckpointResult<std::string>
  inspect_image(std::string_view reference, RuntimeCpuAssignment cpu_assignment,
                StableErrorCode failure_code) {
    if (!is_valid_image_reference(reference)) {
      return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                        "image reference is invalid"));
    }
    const auto document =
        run_json(config_.podman_executable,
                 {"image", "inspect", "--format=json", std::string(reference)},
                 config_.inspection_timeout, failure_code, "image inspect",
                 cpu_assignment.checkpoint_cpu);
    if (!document) {
      return std::unexpected(document.error());
    }
    if (!document->is_array() || document->size() != 1U ||
        !document->front().is_object()) {
      return std::unexpected(
          make_error(failure_code, "image inspect JSON has an invalid shape"));
    }
    const auto identity = string_member(document->front(), "Id");
    const auto canonical =
        identity ? canonical_image_id(*identity) : std::nullopt;
    if (!canonical) {
      return std::unexpected(make_error(
          failure_code, "image inspect JSON has an invalid identity"));
    }
    return *canonical;
  }

  [[nodiscard]] CheckpointResult<void> validate_owned_container(
      const ContainerInspection &inspection, const ContainerId &container_id,
      std::uint32_t container_cpu, bool require_running) const {
    if (inspection.managed != "true" ||
        inspection.logical_id != container_id.value()) {
      return std::unexpected(make_error(
          StableErrorCode::permission_denied,
          "container ownership labels do not match the logical container"));
    }
    if (inspection.cpuset != std::to_string(container_cpu)) {
      return std::unexpected(make_error(
          StableErrorCode::conflict,
          "container CPU assignment does not match the registered profile"));
    }
    if (require_running && !inspection.running) {
      return std::unexpected(
          make_error(StableErrorCode::conflict,
                     "container is not running as required by the operation"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<bool>
  query_container_exists(std::string_view reference, bool external,
                         RuntimeCpuAssignment cpu_assignment,
                         StableErrorCode failure_code) {
    auto arguments = std::vector<std::string>{"container", "exists"};
    if (external) {
      arguments.emplace_back("--external");
    }
    arguments.emplace_back(reference);
    const auto result = run_raw(
        config_.podman_executable, std::move(arguments),
        config_.inspection_timeout, failure_code, "container existence check",
        cpu_assignment.checkpoint_cpu, config_.environment);
    if (!result) {
      return std::unexpected(result.error());
    }
    if (*result->exit_code == 0) {
      return true;
    }
    if (*result->exit_code == 1) {
      return false;
    }
    return std::unexpected(make_error(
        failure_code, "container existence check returned an unknown status"));
  }

  [[nodiscard]] CheckpointResult<ContainerPresence>
  container_presence(std::string_view reference,
                     RuntimeCpuAssignment cpu_assignment,
                     StableErrorCode failure_code) {
    const auto normal =
        query_container_exists(reference, false, cpu_assignment, failure_code);
    if (!normal) {
      return std::unexpected(normal.error());
    }
    const auto external =
        query_container_exists(reference, true, cpu_assignment, failure_code);
    if (!external) {
      return std::unexpected(external.error());
    }
    return ContainerPresence{.normal = *normal, .external = *external};
  }

  [[nodiscard]] CheckpointResult<void>
  prove_absence(std::string_view reference, RuntimeCpuAssignment cpu_assignment,
                StableErrorCode failure_code) {
    const auto presence =
        container_presence(reference, cpu_assignment, failure_code);
    if (!presence) {
      return std::unexpected(presence.error());
    }
    if (presence->normal || presence->external) {
      return std::unexpected(
          make_error(failure_code, "container absence could not be proven"));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  stop_container(std::string_view runtime_id,
                 RuntimeCpuAssignment cpu_assignment) {
    const auto stopped = run_checked(
        config_.podman_executable,
        {"stop", "--time=10", std::string(runtime_id)}, config_.cleanup_timeout,
        StableErrorCode::unavailable, "container stop",
        cpu_assignment.checkpoint_cpu, config_.environment);
    if (!stopped) {
      return std::unexpected(stopped.error());
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  remove_container(std::string_view runtime_id,
                   RuntimeCpuAssignment cpu_assignment) {
    const auto removed =
        run_checked(config_.podman_executable, {"rm", std::string(runtime_id)},
                    config_.cleanup_timeout, StableErrorCode::unavailable,
                    "container removal", cpu_assignment.checkpoint_cpu,
                    config_.environment);
    if (!removed) {
      return std::unexpected(removed.error());
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<std::string>
  cold_start_environment_file_contents(const ColdStartSpec &spec) const {
    if (spec.environment.size() > config_.maximum_environment_variables) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "cold-start environment exceeds the configured limit"));
    }

    std::string contents;
    std::unordered_set<std::string> requested_names;
    for (const auto &variable : spec.environment) {
      if (!is_valid_environment_name(variable.name) ||
          variable.name.size() > config_.maximum_argument_bytes ||
          variable.value.size() > config_.maximum_environment_value_bytes ||
          has_nul(variable.value) ||
          variable.value.find('\r') != std::string::npos ||
          variable.value.find('\n') != std::string::npos ||
          !requested_names.insert(variable.name).second) {
        return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                          "cold-start environment is invalid"));
      }
      contents.append(variable.name);
      contents.push_back('=');
      contents.append(variable.value);
      contents.push_back('\n');
    }
    return contents;
  }

  [[nodiscard]] CheckpointResult<void>
  validate_workload_arguments(const std::vector<std::string> &arguments,
                              bool has_environment_file) const {
    constexpr std::size_t fixed_arguments = 15U;
    constexpr std::size_t environment_file_arguments = 2U;
    const auto required_fixed_arguments =
        fixed_arguments +
        (has_environment_file ? environment_file_arguments : 0U);
    const auto remaining_after_workload =
        arguments.size() <= config_.maximum_arguments
            ? config_.maximum_arguments - arguments.size()
            : 0U;
    if (arguments.size() > config_.maximum_arguments ||
        required_fixed_arguments > remaining_after_workload ||
        std::any_of(arguments.begin(), arguments.end(), [&](const auto &value) {
          return value.size() > config_.maximum_argument_bytes ||
                 has_nul(value);
        })) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "cold-start argv exceeds the configured limits"));
    }
    return {};
  }

  IProcessRunner &process_runner_;
  IAffinityController &affinity_controller_;
  ILogger &logger_;
  PodmanAdapterConfig config_;
};

PodmanAdapter::PodmanAdapter(IProcessRunner &process_runner,
                             IAffinityController &affinity_controller,
                             ILogger &logger, PodmanAdapterConfig config)
    : impl_(std::make_unique<Impl>(process_runner, affinity_controller, logger,
                                   std::move(config))) {}

PodmanAdapter::~PodmanAdapter() = default;

CheckpointResult<void> PodmanAdapter::validate_restore_compatibility(
    const RuntimeFingerprint &snapshot_fingerprint,
    std::string_view source_image_identity,
    RuntimeCpuAssignment cpu_assignment) {
  return impl_->validate_restore_compatibility(
      snapshot_fingerprint, source_image_identity, cpu_assignment);
}

CheckpointResult<CapturedSnapshot>
PodmanAdapter::capture(const ContainerId &container_id,
                       const RuntimeContainerRef &runtime_container,
                       const std::filesystem::path &staging_directory,
                       RuntimeCpuAssignment cpu_assignment) {
  return impl_->capture(container_id, runtime_container, staging_directory,
                        cpu_assignment);
}

CheckpointResult<RuntimeObservation>
PodmanAdapter::inspect_runtime(const ContainerId &container_id,
                               const RuntimeContainerRef &runtime_container,
                               RuntimeCpuAssignment cpu_assignment) {
  return impl_->inspect_runtime(container_id, runtime_container,
                                cpu_assignment);
}

CheckpointResult<void>
PodmanAdapter::stop_and_remove(const ContainerId &container_id,
                               const RuntimeIncarnation &expected_incarnation,
                               RuntimeCpuAssignment cpu_assignment) {
  return impl_->stop_and_remove(container_id, expected_incarnation,
                                cpu_assignment);
}

CheckpointResult<void>
PodmanAdapter::confirm_absence(const ContainerId &container_id,
                               const RuntimeContainerRef &runtime_container,
                               RuntimeCpuAssignment cpu_assignment) {
  return impl_->confirm_absence(container_id, runtime_container,
                                cpu_assignment);
}

CheckpointResult<RuntimeIncarnation>
PodmanAdapter::restore(const ContainerId &container_id,
                       const RuntimeContainerRef &runtime_container,
                       const std::filesystem::path &snapshot_artifact,
                       const RuntimeFingerprint &snapshot_fingerprint,
                       std::string_view source_image_identity,
                       RuntimeCpuAssignment cpu_assignment) {
  return impl_->restore(container_id, runtime_container, snapshot_artifact,
                        snapshot_fingerprint, source_image_identity,
                        cpu_assignment);
}

CheckpointResult<RuntimeIncarnation>
PodmanAdapter::cold_start(const ContainerId &container_id,
                          const RuntimeContainerRef &runtime_container,
                          const ColdStartSpec &spec,
                          RuntimeCpuAssignment cpu_assignment) {
  return impl_->cold_start(container_id, runtime_container, spec,
                           cpu_assignment);
}

CheckpointResult<void> PodmanAdapter::cleanup_failed_restore(
    const ContainerId &container_id,
    const RuntimeContainerRef &runtime_container,
    RuntimeCpuAssignment cpu_assignment) {
  return impl_->cleanup_failed_restore(container_id, runtime_container,
                                       cpu_assignment);
}

} // namespace gravesentinel::ccm
