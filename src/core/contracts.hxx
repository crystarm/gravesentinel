#pragma once

#include "src/core/errors.hxx"
#include "src/protocol/types.hxx"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gravesentinel::ccm {

struct RuntimeCpuAssignment {
  std::uint32_t container_cpu;
  std::uint32_t checkpoint_cpu;

  bool operator==(const RuntimeCpuAssignment &) const = default;
};

struct RuntimeIncarnation {
  RuntimeContainerRef reference;
  std::string runtime_id;

  bool operator==(const RuntimeIncarnation &) const = default;
};

struct CapturedSnapshot {
  std::filesystem::path artifact_path;
  std::string source_image_identity;
  RuntimeFingerprint runtime_fingerprint;
  RuntimeIncarnation source_incarnation;
};

enum class StorageOperationKind : std::uint8_t {
  create_checkpoint,
  restore,
  retention,
  garbage_collection,
};

struct StoredManifest {
  std::uint64_t generation;

  auto operator<=>(const StoredManifest &) const = default;
};

struct StorageDirectoryIdentity {
  std::uint64_t device;
  std::uint64_t inode;

  bool operator==(const StorageDirectoryIdentity &) const = default;
};

class IStoragePin {
public:
  virtual ~IStoragePin() = default;

  [[nodiscard]] virtual CheckpointResult<void> release() = 0;
};

class IStorageOperation {
public:
  virtual ~IStorageOperation() = default;

  [[nodiscard]] virtual const ContainerId &container_id() const noexcept = 0;
  [[nodiscard]] virtual StorageOperationKind kind() const noexcept = 0;
  [[nodiscard]] virtual const std::filesystem::path &
  staging_directory() const noexcept = 0;
  [[nodiscard]] virtual StorageDirectoryIdentity
  staging_directory_identity() const noexcept = 0;
  [[nodiscard]] virtual const std::filesystem::path &
  runtime_artifact_directory() const noexcept = 0;
  [[nodiscard]] virtual StorageDirectoryIdentity
  runtime_artifact_directory_identity() const noexcept = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<std::byte>>
  read_chunk(const ChunkHash &hash) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  publish_chunk(const ChunkHash &hash, std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  publish_manifest(const CheckpointId &checkpoint_id,
                   std::span<const std::byte> manifest) = 0;
  [[nodiscard]] virtual CheckpointResult<std::unique_ptr<IStoragePin>>
  pin_manifest(std::uint64_t generation) = 0;
  [[nodiscard]] virtual CheckpointResult<void> cleanup() = 0;
};

class IStorage {
public:
  virtual ~IStorage() = default;

  [[nodiscard]] virtual CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &container_id,
                  const OperationId &operation_id,
                  StorageOperationKind kind) = 0;
  [[nodiscard]] virtual CheckpointResult<bool>
  contains_chunk(const ContainerId &container_id,
                 const ChunkHash &hash) const = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container_id, const ChunkHash &hash) const = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &container_id) const = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &container_id,
                std::uint64_t generation) const = 0;
};

class IPodmanAdapter {
public:
  virtual ~IPodmanAdapter() = default;

  [[nodiscard]] virtual CheckpointResult<void>
  validate_restore_compatibility(const RuntimeFingerprint &snapshot_fingerprint,
                                 std::string_view source_image_identity,
                                 RuntimeCpuAssignment cpu_assignment) = 0;
  [[nodiscard]] virtual CheckpointResult<CapturedSnapshot>
  capture(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &staging_directory,
          RuntimeCpuAssignment cpu_assignment) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  stop_and_remove(const ContainerId &container_id,
                  const RuntimeIncarnation &expected_incarnation,
                  RuntimeCpuAssignment cpu_assignment) = 0;
  [[nodiscard]] virtual CheckpointResult<RuntimeIncarnation>
  restore(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &snapshot_artifact,
          const RuntimeFingerprint &snapshot_fingerprint,
          std::string_view source_image_identity,
          RuntimeCpuAssignment cpu_assignment) = 0;
  [[nodiscard]] virtual CheckpointResult<RuntimeIncarnation>
  cold_start(const ContainerId &container_id,
             const RuntimeContainerRef &runtime_container,
             const ColdStartSpec &spec,
             RuntimeCpuAssignment cpu_assignment) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  cleanup_failed_restore(const ContainerId &container_id,
                         const RuntimeContainerRef &runtime_container,
                         RuntimeCpuAssignment cpu_assignment) = 0;
};

class IClock {
public:
  using WallTime = std::chrono::system_clock::time_point;
  using MonotonicTime = std::chrono::steady_clock::time_point;

  virtual ~IClock() = default;

  [[nodiscard]] virtual WallTime wall_now() const noexcept = 0;
  [[nodiscard]] virtual MonotonicTime monotonic_now() const noexcept = 0;
};

class IHasher {
public:
  virtual ~IHasher() = default;

  [[nodiscard]] virtual CheckpointResult<ChunkHash>
  hash(std::span<const std::byte> bytes) const = 0;
};

class IIdGenerator {
public:
  virtual ~IIdGenerator() = default;

  [[nodiscard]] virtual CheckpointResult<CheckpointId> new_checkpoint_id() = 0;
  [[nodiscard]] virtual CheckpointResult<OperationId> new_operation_id() = 0;
};

enum class LogLevel : std::uint8_t { debug, info, warning, error };

struct LogField {
  std::string_view key;
  std::string_view value;
};

class ILogger {
public:
  virtual ~ILogger() = default;

  virtual void log(LogLevel level, std::string_view message,
                   std::span<const LogField> fields) noexcept = 0;
};

class IThreadAffinityBinding {
public:
  virtual ~IThreadAffinityBinding() = default;

  [[nodiscard]] virtual CheckpointResult<void> restore() = 0;
};

class IAffinityController {
public:
  virtual ~IAffinityController() = default;

  [[nodiscard]] virtual CheckpointResult<
      std::unique_ptr<IThreadAffinityBinding>>
  bind_current_thread(std::uint32_t logical_cpu) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const = 0;
};

enum class ProcessTermination : std::uint8_t {
  exited,
  signaled,
  timed_out,
};

struct ProcessRequest {
  std::filesystem::path executable;
  std::vector<std::string> argv;
  std::vector<EnvironmentVariable> environment;
  std::optional<std::filesystem::path> working_directory;
  std::optional<std::uint32_t> logical_cpu;
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  std::chrono::milliseconds termination_grace_period{std::chrono::seconds{1}};
  std::size_t maximum_standard_output_bytes{64U * 1024U};
  std::size_t maximum_standard_error_bytes{64U * 1024U};
};

struct ProcessResult {
  ProcessTermination termination;
  std::optional<int> exit_code;
  std::optional<int> signal_number;
  std::string standard_output;
  std::string standard_error;
  bool standard_output_truncated{false};
  bool standard_error_truncated{false};
};

class IProcessRunner {
public:
  virtual ~IProcessRunner() = default;

  [[nodiscard]] virtual CheckpointResult<ProcessResult>
  run(const ProcessRequest &request) = 0;
};

class ICheckpointService {
public:
  virtual ~ICheckpointService() = default;

  [[nodiscard]] virtual CheckpointResult<void>
  register_container(const ContainerRegistration &registration,
                     const RequestId &request_id) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  unregister_container(const ContainerId &container_id,
                       const RequestId &request_id) = 0;
  [[nodiscard]] virtual CheckpointResult<OperationId>
  submit_create_checkpoint(const ContainerId &container_id,
                           const RequestId &request_id) = 0;
  [[nodiscard]] virtual CheckpointResult<OperationId>
  submit_restore_or_restart(const ContainerId &container_id,
                            const RequestId &request_id) = 0;
  [[nodiscard]] virtual CheckpointResult<OperationStatus>
  get_operation(const OperationId &operation_id) const = 0;
};

} // namespace gravesentinel::ccm
