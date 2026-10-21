#include "src/protocol/types.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace gravesentinel::ccm {
namespace {

constexpr std::size_t kMaximumRegistrations = 4;
constexpr std::size_t kMvpCpuCount = 8;

constexpr std::array<std::array<std::uint32_t, 2>, kMaximumRegistrations>
    kStandardCpuPairs{{{0, 1}, {2, 3}, {4, 5}, {6, 7}}};

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

} // namespace

bool is_standard_cpu_pair(std::uint32_t container_cpu,
                          std::uint32_t checkpoint_cpu) noexcept {
  for (const auto &pair : kStandardCpuPairs) {
    if (pair[0] == container_cpu && pair[1] == checkpoint_cpu) {
      return true;
    }
  }
  return false;
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
  if (!is_standard_cpu_pair(config.container_cpu, config.checkpoint_cpu)) {
    return std::unexpected(
        make_error(ValidationErrorCode::invalid_cpu_pair, "checkpoint.cpu_pair",
                   "CPU pair must be one of (0,1), (2,3), (4,5), or (6,7)"));
  }
  return {};
}

std::expected<void, ValidationError>
validate_container_registration(const ContainerRegistration &registration) {
  if (auto result = validate_checkpoint_config(registration.checkpoint);
      !result) {
    return result;
  }

  if (registration.cold_start.has_value() &&
      !is_nonempty_without_nul(registration.cold_start->image_reference)) {
    return std::unexpected(make_error(
        ValidationErrorCode::invalid_cold_start_spec,
        "cold_start.image_reference",
        "cold-start image reference must be nonempty and contain no NUL"));
  }

  if (registration.runtime_fingerprint.has_value()) {
    const auto &fingerprint = *registration.runtime_fingerprint;
    if (!is_nonempty_without_nul(fingerprint.runtime_name) ||
        !is_nonempty_without_nul(fingerprint.runtime_version) ||
        !is_nonempty_without_nul(fingerprint.architecture)) {
      return std::unexpected(make_error(
          ValidationErrorCode::invalid_runtime_fingerprint,
          "runtime_fingerprint",
          "runtime name, version, and architecture must be nonempty and "
          "contain no NUL"));
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

    const std::array cpus{registration.checkpoint.container_cpu,
                          registration.checkpoint.checkpoint_cpu};
    for (const auto cpu : cpus) {
      if (assigned[cpu]) {
        return std::unexpected(make_error(
            ValidationErrorCode::duplicate_cpu_assignment,
            "checkpoint.cpu_pair",
            "a logical CPU must not be assigned to multiple registrations"));
      }
      assigned[cpu] = true;
    }
  }

  return {};
}

} // namespace gravesentinel::ccm
