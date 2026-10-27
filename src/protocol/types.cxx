#include "src/protocol/types.hxx"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_set>

namespace gravesentinel::ccm {
namespace {

constexpr std::size_t kMaximumRegistrations = 4;
constexpr std::size_t kMvpCpuCount = CpuSet::maximum_size;

constexpr std::size_t kMaximumColdStartImageBytes = 2048;
constexpr std::size_t kMaximumColdStartArgvEntries = 256;
constexpr std::size_t kMaximumColdStartArgumentBytes = 4096;
constexpr std::size_t kMaximumColdStartEnvironmentEntries = 256;
constexpr std::size_t kMaximumColdStartEnvironmentNameBytes = 256;
constexpr std::size_t kMaximumColdStartEnvironmentValueBytes = 4096;
constexpr std::size_t kMaximumColdStartTotalBytes = 64U * 1024U;

[[nodiscard]] ValidationError make_error(ValidationErrorCode code,
                                         std::string_view field,
                                         std::string_view diagnostic) {
  return ValidationError{
      .code = code,
      .field = field,
      .diagnostic = std::string(diagnostic),
  };
}

[[nodiscard]] bool is_nonempty_without_nul(std::string_view value) noexcept {
  return !value.empty() && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool is_ascii_letter(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool is_ascii_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] bool is_posix_environment_name(std::string_view name) noexcept {
  if (name.empty() || (!is_ascii_letter(name.front()) && name.front() != '_')) {
    return false;
  }

  for (const char value : name.substr(1)) {
    if (!is_ascii_letter(value) && !is_ascii_digit(value) && value != '_') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool consume_cold_start_bytes(std::size_t &total,
                                            std::size_t bytes) noexcept {
  if (bytes > kMaximumColdStartTotalBytes - total) {
    return false;
  }
  total += bytes;
  return true;
}

} // namespace

std::expected<CpuSet, ValidationError>
CpuSet::create(std::vector<std::uint32_t> cpus) {
  if (cpus.empty()) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cpu_set, "checkpoint.container_cpus",
        "container CPU set must contain at least one logical CPU"));
  }
  if (cpus.size() > maximum_size) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cpu_set, "checkpoint.container_cpus",
        "container CPU set exceeds the eight-CPU profile limit"));
  }
  std::ranges::sort(cpus);
  if (cpus.back() > maximum_cpu) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cpu_set, "checkpoint.container_cpus",
        "container logical CPUs must be in the inclusive range 0..7"));
  }
  if (std::ranges::adjacent_find(cpus) != cpus.end()) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cpu_set, "checkpoint.container_cpus",
        "container CPU set must not contain duplicate logical CPUs"));
  }
  return CpuSet(std::move(cpus));
}

bool CpuSet::contains(const std::uint32_t cpu) const noexcept {
  return std::ranges::binary_search(cpus_, cpu);
}

std::expected<void, ValidationError>
validate_checkpoint_config(const CheckpointConfig &config) {
  if (config.interval <= std::chrono::seconds::zero()) {
    return std::unexpected(
        make_error(ValidationErrorCode::invalid_interval, "checkpoint.interval",
                   "checkpoint interval must be greater than zero"));
  }
  if (config.keep_last < 1 || config.keep_last > 3) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_keep_last, "checkpoint.keep_last",
        "keep_last must be in the inclusive range 1..3"));
  }
  if (config.checkpoint_cpu > CpuSet::maximum_cpu ||
      config.container_cpus.contains(config.checkpoint_cpu)) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cpu_pair, "checkpoint.cpu_assignment",
        "checkpoint CPU must be in 0..7 and outside the container CPU set"));
  }
  return {};
}

std::expected<void, ValidationError>
validate_container_registration(const ContainerRegistration &registration) {
  if (auto result = validate_checkpoint_config(registration.checkpoint);
      !result) {
    return result;
  }

  if (registration.cold_start.has_value()) {
    const auto &cold_start = *registration.cold_start;
    if (!is_nonempty_without_nul(cold_start.image_reference)) {
      return std::unexpected(make_error(
          ValidationErrorCode::invalid_cold_start_spec,
          "cold_start.image_reference",
          "cold-start image reference must be nonempty and contain no NUL"));
    }
    if (cold_start.image_reference.size() > kMaximumColdStartImageBytes) {
      return std::unexpected(
          make_error(ValidationErrorCode::invalid_cold_start_spec,
                     "cold_start.image_reference",
                     "cold-start image reference exceeds the 2048-byte limit"));
    }
    if (cold_start.argv.size() > kMaximumColdStartArgvEntries) {
      return std::unexpected(make_error(
          ValidationErrorCode::invalid_cold_start_spec, "cold_start.argv",
          "cold-start argv exceeds the 256-entry limit"));
    }
    if (cold_start.environment.size() > kMaximumColdStartEnvironmentEntries) {
      return std::unexpected(
          make_error(ValidationErrorCode::invalid_cold_start_spec,
                     "cold_start.environment",
                     "cold-start environment exceeds the 256-entry limit"));
    }

    std::size_t total_bytes = 0;
    if (!consume_cold_start_bytes(total_bytes,
                                  cold_start.image_reference.size())) {
      return std::unexpected(
          make_error(ValidationErrorCode::invalid_cold_start_spec, "cold_start",
                     "cold-start strings exceed the 65536-byte total limit"));
    }

    for (const auto &argument : cold_start.argv) {
      if (argument.find('\0') != std::string::npos) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec, "cold_start.argv",
            "cold-start argv entries must not contain NUL"));
      }
      if (argument.size() > kMaximumColdStartArgumentBytes) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec, "cold_start.argv",
            "a cold-start argv entry exceeds the 4096-byte limit"));
      }
      if (!consume_cold_start_bytes(total_bytes, argument.size())) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec, "cold_start",
            "cold-start strings exceed the 65536-byte total limit"));
      }
    }

    std::unordered_set<std::string_view> environment_names;
    environment_names.reserve(cold_start.environment.size());
    for (const auto &variable : cold_start.environment) {
      if (variable.name.size() > kMaximumColdStartEnvironmentNameBytes) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec,
            "cold_start.environment.name",
            "a cold-start environment name exceeds the 256-byte limit"));
      }
      if (variable.name.find('\0') != std::string::npos ||
          !is_posix_environment_name(variable.name)) {
        return std::unexpected(
            make_error(ValidationErrorCode::invalid_cold_start_spec,
                       "cold_start.environment.name",
                       "cold-start environment names must match "
                       "[A-Za-z_][A-Za-z0-9_]* and contain no NUL"));
      }
      if (!environment_names.insert(variable.name).second) {
        return std::unexpected(
            make_error(ValidationErrorCode::invalid_cold_start_spec,
                       "cold_start.environment.name",
                       "cold-start environment names must be unique"));
      }
      if (variable.value.find('\0') != std::string::npos) {
        return std::unexpected(
            make_error(ValidationErrorCode::invalid_cold_start_spec,
                       "cold_start.environment.value",
                       "cold-start environment values must not contain NUL"));
      }
      if (variable.value.size() > kMaximumColdStartEnvironmentValueBytes) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec,
            "cold_start.environment.value",
            "a cold-start environment value exceeds the 4096-byte limit"));
      }
      if (!consume_cold_start_bytes(total_bytes, variable.name.size()) ||
          !consume_cold_start_bytes(total_bytes, variable.value.size())) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_cold_start_spec, "cold_start",
            "cold-start strings exceed the 65536-byte total limit"));
      }
    }

    if (!cold_start.mounts.empty()) {
      return std::unexpected(make_error(
          ValidationErrorCode::invalid_cold_start_spec, "cold_start.mounts",
          "cold-start mounts are unsupported by "
          "podman-criu-rootful-v2"));
    }
    if (!cold_start.runtime_options.empty()) {
      return std::unexpected(
          make_error(ValidationErrorCode::invalid_cold_start_spec,
                     "cold_start.runtime_options",
                     "cold-start runtime options are unsupported by "
                     "podman-criu-rootful-v2"));
    }
  }

  if (registration.runtime_fingerprint.has_value()) {
    const auto &fingerprint = *registration.runtime_fingerprint;
    const std::string *fields[]{
        &fingerprint.runtime_name,         &fingerprint.runtime_version,
        &fingerprint.runtime_build_origin, &fingerprint.architecture,
        &fingerprint.kernel_release,       &fingerprint.criu_version,
        &fingerprint.oci_runtime_name,     &fingerprint.oci_runtime_version,
        &fingerprint.oci_runtime_commit,   &fingerprint.oci_runtime_features,
        &fingerprint.cgroup_version,       &fingerprint.cgroup_manager,
        &fingerprint.cgroup_controllers,   &fingerprint.privilege_mode,
        &fingerprint.user_namespace_mode,  &fingerprint.capability_profile,
        &fingerprint.selinux_mode,         &fingerprint.apparmor_profile,
        &fingerprint.seccomp_profile,      &fingerprint.command_profile,
    };
    for (const auto *field : fields) {
      if (!is_nonempty_without_nul(*field)) {
        return std::unexpected(make_error(
            ValidationErrorCode::invalid_runtime_fingerprint,
            "runtime_fingerprint",
            "all runtime fingerprint fields must be nonempty and contain no "
            "NUL"));
      }
    }
  }

  return {};
}

std::expected<void, ValidationError>
validate_cpu_assignments(std::span<const ContainerRegistration> registrations) {
  if (registrations.size() > kMaximumRegistrations) {
    return std::unexpected(
        make_error(ValidationErrorCode::too_many_registrations, "registrations",
                   "the MVP permits at most four simultaneous registrations"));
  }

  std::array<bool, kMvpCpuCount> assigned{};
  for (const auto &registration : registrations) {
    if (auto result = validate_container_registration(registration); !result) {
      return result;
    }

    for (const auto cpu : registration.checkpoint.container_cpus.values()) {
      if (assigned[cpu]) {
        return std::unexpected(make_error(
            ValidationErrorCode::duplicate_cpu_assignment,
            "checkpoint.cpu_assignment",
            "a logical CPU must not be assigned to multiple registrations"));
      }
      assigned[cpu] = true;
    }
    const auto checkpoint_cpu = registration.checkpoint.checkpoint_cpu;
    if (assigned[checkpoint_cpu]) {
      return std::unexpected(make_error(
          ValidationErrorCode::duplicate_cpu_assignment,
          "checkpoint.cpu_assignment",
          "a logical CPU must not be assigned to multiple registrations"));
    }
    assigned[checkpoint_cpu] = true;
  }

  return {};
}

} // namespace gravesentinel::ccm
