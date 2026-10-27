#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {

enum class ValidationErrorCode : std::uint8_t {
  empty_value,
  embedded_nul,
  invalid_interval,
  invalid_keep_last,
  invalid_cpu_pair,
  invalid_cpu_set,
  duplicate_cpu_assignment,
  too_many_registrations,
  invalid_cold_start_spec,
  invalid_runtime_fingerprint,
};

struct ValidationError {
  ValidationErrorCode code;
  std::string_view field;
  std::string diagnostic;

  bool operator==(const ValidationError &) const = default;
};

namespace detail {

struct ContainerIdTag {
  static constexpr std::string_view field_name = "container_id";
};

struct CheckpointIdTag {
  static constexpr std::string_view field_name = "checkpoint_id";
};

struct RequestIdTag {
  static constexpr std::string_view field_name = "request_id";
};

struct OperationIdTag {
  static constexpr std::string_view field_name = "operation_id";
};

struct RuntimeContainerRefTag {
  static constexpr std::string_view field_name = "runtime_container_ref";
};

} // namespace detail

template <typename Tag> class BasicIdentifier final {
public:
  [[nodiscard]] static std::expected<BasicIdentifier, ValidationError>
  create(std::string value) {
    if (value.empty()) {
      return std::unexpected(ValidationError{
          .code = ValidationErrorCode::empty_value,
          .field = Tag::field_name,
          .diagnostic = "identifier must not be empty",
      });
    }
    if (value.find('\0') != std::string::npos) {
      return std::unexpected(ValidationError{
          .code = ValidationErrorCode::embedded_nul,
          .field = Tag::field_name,
          .diagnostic = "identifier must not contain NUL bytes",
      });
    }
    return BasicIdentifier(std::move(value));
  }

  [[nodiscard]] const std::string &value() const noexcept { return value_; }

  auto operator<=>(const BasicIdentifier &) const = default;

private:
  explicit BasicIdentifier(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

using ContainerId = BasicIdentifier<detail::ContainerIdTag>;
using CheckpointId = BasicIdentifier<detail::CheckpointIdTag>;
using RequestId = BasicIdentifier<detail::RequestIdTag>;
using OperationId = BasicIdentifier<detail::OperationIdTag>;
using RuntimeContainerRef = BasicIdentifier<detail::RuntimeContainerRefTag>;

class CpuSet final {
public:
  static constexpr std::size_t maximum_size = 8;
  static constexpr std::uint32_t maximum_cpu = 7;

  CpuSet() : cpus_{0U} {}

  [[nodiscard]] static std::expected<CpuSet, ValidationError>
  create(std::vector<std::uint32_t> cpus);

  [[nodiscard]] const std::vector<std::uint32_t> &values() const noexcept {
    return cpus_;
  }
  [[nodiscard]] bool contains(std::uint32_t cpu) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return cpus_.size(); }

  auto operator<=>(const CpuSet &) const = default;

private:
  explicit CpuSet(std::vector<std::uint32_t> cpus) : cpus_(std::move(cpus)) {}

  std::vector<std::uint32_t> cpus_;
};

class ChunkHash final {
public:
  static constexpr std::size_t digest_size = 32;
  using Bytes = std::array<std::byte, digest_size>;

  explicit constexpr ChunkHash(Bytes bytes) : bytes_(bytes) {}

  [[nodiscard]] constexpr const Bytes &bytes() const noexcept { return bytes_; }

  auto operator<=>(const ChunkHash &) const = default;

private:
  Bytes bytes_;
};

struct CheckpointConfig {
  std::chrono::seconds interval{60};
  std::size_t keep_last{3};
  CpuSet container_cpus;
  std::uint32_t checkpoint_cpu{};

  bool operator==(const CheckpointConfig &) const = default;
};

struct EnvironmentVariable {
  std::string name;
  std::string value;

  bool operator==(const EnvironmentVariable &) const = default;
};

struct MountSpec {
  std::string source;
  std::string target;
  bool read_only{true};

  bool operator==(const MountSpec &) const = default;
};

struct ColdStartSpec {
  std::string image_reference;
  std::vector<std::string> argv;
  std::vector<EnvironmentVariable> environment;
  std::vector<MountSpec> mounts;
  std::vector<std::string> runtime_options;

  bool operator==(const ColdStartSpec &) const = default;
};

struct RuntimeFingerprint {
  std::string runtime_name;
  std::string runtime_version;
  std::string runtime_build_origin;
  std::string architecture;
  std::string kernel_release;
  std::string criu_version;
  std::string oci_runtime_name;
  std::string oci_runtime_version;
  std::string oci_runtime_commit;
  std::string oci_runtime_features;
  std::string cgroup_version;
  std::string cgroup_manager;
  std::string cgroup_controllers;
  std::string privilege_mode;
  std::string user_namespace_mode;
  std::string capability_profile;
  std::string selinux_mode;
  std::string apparmor_profile;
  std::string seccomp_profile;
  std::string command_profile;

  bool operator==(const RuntimeFingerprint &) const = default;
};

struct ContainerRegistration {
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  CheckpointConfig checkpoint;
  std::optional<ColdStartSpec> cold_start;
  std::optional<RuntimeFingerprint> runtime_fingerprint;

  bool operator==(const ContainerRegistration &) const = default;
};

enum class RuntimeState : std::uint8_t { absent, running, exited };

struct RuntimeStatus {
  RuntimeState state;
  std::optional<int> exit_code;

  bool operator==(const RuntimeStatus &) const = default;
};

enum class StableErrorCode : std::uint8_t {
  invalid_argument,
  not_found,
  already_exists,
  resource_exhausted,
  conflict,
  permission_denied,
  capture_failed,
  storage_error,
  corrupted_snapshot,
  incompatible_runtime,
  restore_failed,
  cold_start_failed,
  deadline_exceeded,
  unavailable,
  internal,
};

enum class OperationState : std::uint8_t {
  queued,
  running,
  succeeded,
  failed,
  indeterminate,
};

struct OperationStatus {
  OperationId operation_id;
  OperationState state;
  std::optional<StableErrorCode> error;

  bool operator==(const OperationStatus &) const = default;
};

[[nodiscard]] std::expected<void, ValidationError>
validate_checkpoint_config(const CheckpointConfig &config);

[[nodiscard]] std::expected<void, ValidationError>
validate_container_registration(const ContainerRegistration &registration);

[[nodiscard]] std::expected<void, ValidationError>
validate_cpu_assignments(std::span<const ContainerRegistration> registrations);

} // namespace gravesentinel::ccm
