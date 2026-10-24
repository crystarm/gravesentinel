#include "src/core/checkpoint_maintenance.hxx"

#include <utility>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] CheckpointErrorDetail invalid_config(std::string diagnostic) {
  return CheckpointErrorDetail{
      .code = StableErrorCode::invalid_argument,
      .diagnostic = std::move(diagnostic),
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

} // namespace

CheckpointMaintenance::CheckpointMaintenance(IStorage &storage, IClock &clock,
                                             CheckpointMaintenanceConfig config)
    : storage_(storage), clock_(clock), config_(config) {}

CheckpointResult<void> CheckpointMaintenance::validate_config() const {
  if (config_.garbage_collection_grace_period < std::chrono::seconds::zero()) {
    return std::unexpected(
        invalid_config("garbage collection grace period must not be negative"));
  }
  if (config_.maximum_garbage_deletions_per_pass == 0U) {
    return std::unexpected(invalid_config(
        "garbage collection deletion limit must be greater than zero"));
  }
  return {};
}

CheckpointResult<RetentionReport>
CheckpointMaintenance::apply_retention(const ContainerId &container_id,
                                       std::size_t keep_last) {
  if (auto valid = validate_config(); !valid) {
    return std::unexpected(valid.error());
  }
  auto operation =
      storage_.begin_maintenance(container_id, StorageOperationKind::retention);
  if (!operation) {
    return std::unexpected(operation.error());
  }
  return (*operation)->apply_retention(keep_last);
}

CheckpointResult<GarbageCollectionReport>
CheckpointMaintenance::collect_garbage(const ContainerId &container_id) {
  if (auto valid = validate_config(); !valid) {
    return std::unexpected(valid.error());
  }
  auto operation = storage_.begin_maintenance(
      container_id, StorageOperationKind::garbage_collection);
  if (!operation) {
    return std::unexpected(operation.error());
  }
  return (*operation)
      ->collect_garbage(GarbageCollectionOptions{
          .now = clock_.wall_now(),
          .grace_period = config_.garbage_collection_grace_period,
          .maximum_deletions = config_.maximum_garbage_deletions_per_pass,
      });
}

} // namespace gravesentinel::ccm
