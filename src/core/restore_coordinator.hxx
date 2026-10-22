#pragma once

#include "src/chunking/snapshot_chunker.hxx"
#include "src/core/contracts.hxx"

#include <cstdint>
#include <optional>

namespace gravesentinel::ccm {

enum class RestoreState : std::uint8_t {
  validating,
  reassembling,
  restoring,
  restored,
  failed,
};

enum class RestoreOutcome : std::uint8_t {
  checkpoint,
  cold_start,
};

struct RestoreRequest {
  RequestId request_id;
  OperationId operation_id;
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  RuntimeCpuAssignment cpu_assignment;
  std::optional<RuntimeIncarnation> expected_source_incarnation;
  std::optional<ColdStartSpec> cold_start;
};

struct RestoredContainer {
  OperationId operation_id;
  RuntimeIncarnation incarnation;
  RestoreOutcome outcome;
  std::optional<CheckpointId> checkpoint_id;
  std::optional<std::uint64_t> generation;
  bool pin_release_complete;
  bool staging_cleanup_complete;

  bool operator==(const RestoredContainer &) const = default;
};

class RestoreCoordinator final {
public:
  RestoreCoordinator(IStorage &storage, IPodmanAdapter &podman_adapter,
                     const IHasher &hasher, ILogger &logger);

  [[nodiscard]] CheckpointResult<RestoredContainer>
  restore_or_restart(const RestoreRequest &request);

private:
  IStorage &storage_;
  IPodmanAdapter &podman_adapter_;
  const IHasher &hasher_;
  ILogger &logger_;
};

} // namespace gravesentinel::ccm
