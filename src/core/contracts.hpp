#pragma once

#include "src/core/errors.hpp"
#include "src/protocol/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gravesentinel::ccm {

struct CapturedSnapshot {
  std::filesystem::path artifact_path;
  RuntimeFingerprint runtime_fingerprint;
};

class IStorage {
public:
  virtual ~IStorage() = default;

  [[nodiscard]] virtual CheckpointResult<bool>
  contains_chunk(const ContainerId &container_id,
                 const ChunkHash &hash) const = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  publish_chunk(const ContainerId &container_id, const ChunkHash &hash,
                std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container_id, const ChunkHash &hash) const = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  publish_manifest(const ContainerId &container_id,
                   const CheckpointId &checkpoint_id,
                   std::span<const std::byte> manifest) = 0;
  [[nodiscard]] virtual CheckpointResult<std::vector<CheckpointId>>
  list_checkpoints(const ContainerId &container_id) const = 0;
};

class IPodmanAdapter {
public:
  virtual ~IPodmanAdapter() = default;

  [[nodiscard]] virtual CheckpointResult<CapturedSnapshot>
  capture(const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &staging_directory,
          std::uint32_t checkpoint_cpu) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  restore(const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &snapshot_artifact,
          std::uint32_t container_cpu) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  cold_start(const RuntimeContainerRef &runtime_container,
             const ColdStartSpec &spec, std::uint32_t container_cpu) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  cleanup_failed_restore(const RuntimeContainerRef &runtime_container) = 0;
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

  [[nodiscard]] virtual ChunkHash
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

class IAffinityController {
public:
  virtual ~IAffinityController() = default;

  [[nodiscard]] virtual CheckpointResult<void>
  bind_current_thread(std::uint32_t logical_cpu) = 0;
  [[nodiscard]] virtual CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const = 0;
};

struct ProcessRequest {
  std::filesystem::path executable;
  std::vector<std::string> argv;
  std::vector<EnvironmentVariable> environment;
  std::optional<std::filesystem::path> working_directory;
  std::optional<std::uint32_t> logical_cpu;
};

struct ProcessResult {
  int exit_code;
  std::string standard_output;
  std::string standard_error;
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
