#pragma once

#include "src/core/checkpoint_creator.hxx"
#include "src/core/checkpoint_maintenance.hxx"
#include "src/core/contracts.hxx"
#include "src/core/restore_coordinator.hxx"

#include <memory>

namespace gravesentinel::ccm {

class ICheckpointOperations {
public:
  virtual ~ICheckpointOperations() = default;

  [[nodiscard]] virtual CheckpointResult<CreatedCheckpoint>
  create(const CheckpointCreateRequest &request) = 0;
  [[nodiscard]] virtual CheckpointResult<RestoredContainer>
  restore_or_restart(const RestoreRequest &request) = 0;
  [[nodiscard]] virtual CheckpointResult<RetentionReport>
  apply_retention(const ContainerId &container_id, std::size_t keep_last) = 0;
  [[nodiscard]] virtual CheckpointResult<GarbageCollectionReport>
  collect_garbage(const ContainerId &container_id) = 0;
};

class CheckpointOperations final : public ICheckpointOperations {
public:
  CheckpointOperations(CheckpointCreator &creator,
                       RestoreCoordinator &restore_coordinator,
                       ICheckpointMaintenance &maintenance) noexcept;

  [[nodiscard]] CheckpointResult<CreatedCheckpoint>
  create(const CheckpointCreateRequest &request) override;
  [[nodiscard]] CheckpointResult<RestoredContainer>
  restore_or_restart(const RestoreRequest &request) override;
  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(const ContainerId &container_id,
                  std::size_t keep_last) override;
  [[nodiscard]] CheckpointResult<GarbageCollectionReport>
  collect_garbage(const ContainerId &container_id) override;

private:
  CheckpointCreator &creator_;
  RestoreCoordinator &restore_coordinator_;
  ICheckpointMaintenance &maintenance_;
};

class SteadyTimer final : public ITimer {
public:
  SteadyTimer();
  ~SteadyTimer() override;

  SteadyTimer(const SteadyTimer &) = delete;
  SteadyTimer &operator=(const SteadyTimer &) = delete;
  SteadyTimer(SteadyTimer &&) = delete;
  SteadyTimer &operator=(SteadyTimer &&) = delete;

  [[nodiscard]] Generation generation() const noexcept override;
  [[nodiscard]] TimerWaitResult wait_until(IClock::MonotonicTime deadline,
                                           Generation observed_generation,
                                           std::stop_token stop_token) override;
  void interrupt() noexcept override;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

class CheckpointService final : public ICheckpointService {
public:
  CheckpointService(ICheckpointOperations &operations, IClock &clock,
                    ITimer &timer, IIdGenerator &id_generator,
                    IAffinityController &affinity_controller, ILogger &logger);
  ~CheckpointService() override;

  CheckpointService(const CheckpointService &) = delete;
  CheckpointService &operator=(const CheckpointService &) = delete;
  CheckpointService(CheckpointService &&) = delete;
  CheckpointService &operator=(CheckpointService &&) = delete;

  [[nodiscard]] CheckpointResult<void>
  register_container(const ContainerRegistration &registration,
                     const RequestId &request_id) override;
  [[nodiscard]] CheckpointResult<void>
  unregister_container(const ContainerId &container_id,
                       const RequestId &request_id) override;
  [[nodiscard]] CheckpointResult<void>
  create_checkpoint(const ContainerId &container_id,
                    const RequestId &request_id,
                    const OperationId &operation_id) override;
  [[nodiscard]] CheckpointResult<void>
  restore_or_restart(const ContainerId &container_id,
                     const RequestId &request_id,
                     const OperationId &operation_id) override;
  [[nodiscard]] CheckpointResult<void> shutdown() override;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace gravesentinel::ccm
