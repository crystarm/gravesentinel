#pragma once

#include "src/core/contracts.hxx"

#include <chrono>
#include <cstddef>

namespace gravesentinel::ccm {

struct CheckpointMaintenanceConfig {
  std::chrono::seconds garbage_collection_grace_period{std::chrono::hours{1}};
  std::size_t maximum_garbage_deletions_per_pass{128};

  bool operator==(const CheckpointMaintenanceConfig &) const = default;
};

class ICheckpointMaintenance {
public:
  virtual ~ICheckpointMaintenance() = default;

  [[nodiscard]] virtual CheckpointResult<RetentionReport>
  apply_retention(const ContainerId &container_id, std::size_t keep_last) = 0;
  [[nodiscard]] virtual CheckpointResult<GarbageCollectionReport>
  collect_garbage(const ContainerId &container_id) = 0;
};

class CheckpointMaintenance final : public ICheckpointMaintenance {
public:
  CheckpointMaintenance(IStorage &storage, IClock &clock,
                        CheckpointMaintenanceConfig config = {});

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(const ContainerId &container_id,
                  std::size_t keep_last) override;
  [[nodiscard]] CheckpointResult<GarbageCollectionReport>
  collect_garbage(const ContainerId &container_id) override;

private:
  [[nodiscard]] CheckpointResult<void> validate_config() const;

  IStorage &storage_;
  IClock &clock_;
  CheckpointMaintenanceConfig config_;
};

} // namespace gravesentinel::ccm
