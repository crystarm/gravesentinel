#pragma once

#include "src/chunking/snapshot_chunker.hxx"
#include "src/core/contracts.hxx"

#include <cstdint>

namespace gravesentinel::ccm {

enum class CheckpointCreateState : std::uint8_t {
  idle,
  capturing,
  chunking,
  committing,
  committed,
  failed,
  incomplete,
};

struct CheckpointCreateRequest {
  RequestId request_id;
  OperationId operation_id;
  ContainerId container_id;
  RuntimeContainerRef runtime_container;
  RuntimeCpuAssignment cpu_assignment;
  std::size_t keep_last{3};
};

struct CreatedCheckpoint {
  OperationId operation_id;
  CheckpointId checkpoint_id;
  std::uint64_t generation;
  RuntimeIncarnation source_incarnation;
  bool retention_complete{true};
  bool staging_cleanup_complete;

  bool operator==(const CreatedCheckpoint &) const = default;
};

class CheckpointCreator final {
public:
  CheckpointCreator(IStorage &storage, IPodmanAdapter &podman_adapter,
                    ISnapshotChunker &snapshot_chunker, IClock &clock,
                    IIdGenerator &id_generator, ILogger &logger);

  [[nodiscard]] CheckpointResult<CreatedCheckpoint>
  create(const CheckpointCreateRequest &request);

private:
  IStorage &storage_;
  IPodmanAdapter &podman_adapter_;
  ISnapshotChunker &snapshot_chunker_;
  IClock &clock_;
  IIdGenerator &id_generator_;
  ILogger &logger_;
};

} // namespace gravesentinel::ccm
