#include "src/core/checkpoint_maintenance.hxx"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] CheckpointErrorDetail test_error(std::string diagnostic) {
  return CheckpointErrorDetail{
      .code = StableErrorCode::storage_error,
      .diagnostic = std::move(diagnostic),
      .retryable = true,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

[[nodiscard]] ContainerId container_id() {
  return ContainerId::create("container-1").value();
}

struct MaintenanceState {
  StorageOperationKind kind{StorageOperationKind::retention};
  std::optional<std::size_t> keep_last;
  std::optional<GarbageCollectionOptions> garbage_options;
  std::optional<CheckpointErrorDetail> retention_error;
  std::optional<CheckpointErrorDetail> garbage_error;
};

class FakeMaintenanceOperation final : public IStorageMaintenanceOperation {
public:
  FakeMaintenanceOperation(ContainerId container,
                           std::shared_ptr<MaintenanceState> state)
      : container_(std::move(container)), state_(std::move(state)) {}

  [[nodiscard]] const ContainerId &container_id() const noexcept override {
    return container_;
  }

  [[nodiscard]] StorageOperationKind kind() const noexcept override {
    return state_->kind;
  }

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(std::size_t keep_last) override {
    state_->keep_last = keep_last;
    if (state_->retention_error.has_value()) {
      return std::unexpected(*state_->retention_error);
    }
    return RetentionReport{
        .manifests_deleted = 2,
        .manifests_deferred = 0,
        .complete = true,
    };
  }

  [[nodiscard]] CheckpointResult<GarbageCollectionReport>
  collect_garbage(const GarbageCollectionOptions &options) override {
    state_->garbage_options = options;
    if (state_->garbage_error.has_value()) {
      return std::unexpected(*state_->garbage_error);
    }
    return GarbageCollectionReport{
        .chunks_marked = 1,
        .chunks_deleted = 2,
        .candidate_records_deleted = 2,
        .more_work = false,
    };
  }

private:
  ContainerId container_;
  std::shared_ptr<MaintenanceState> state_;
};

class FakeStorage final : public IStorage {
public:
  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &, const OperationId &,
                  StorageOperationKind) override {
    return std::unexpected(test_error("unexpected regular operation"));
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageMaintenanceOperation>>
  begin_maintenance(const ContainerId &container,
                    StorageOperationKind kind) override {
    ++begin_calls;
    if (begin_error.has_value()) {
      return std::unexpected(*begin_error);
    }
    state->kind = kind;
    return std::unique_ptr<IStorageMaintenanceOperation>{
        std::make_unique<FakeMaintenanceOperation>(container, state)};
  }

  [[nodiscard]] CheckpointResult<bool>
  contains_chunk(const ContainerId &, const ChunkHash &) const override {
    return false;
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &, const ChunkHash &) const override {
    return std::unexpected(test_error("chunk not found"));
  }

  [[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &) const override {
    return std::vector<StoredManifest>{};
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &, std::uint64_t) const override {
    return std::unexpected(test_error("manifest not found"));
  }

  std::shared_ptr<MaintenanceState> state =
      std::make_shared<MaintenanceState>();
  std::optional<CheckpointErrorDetail> begin_error;
  std::size_t begin_calls{0};
};

class FakeClock final : public IClock {
public:
  explicit FakeClock(WallTime now) : now_(now) {}

  [[nodiscard]] WallTime wall_now() const noexcept override { return now_; }

  [[nodiscard]] MonotonicTime monotonic_now() const noexcept override {
    return MonotonicTime{};
  }

private:
  WallTime now_;
};

TEST(CheckpointMaintenanceTest, UsesLeaseOnlyRetentionOperation) {
  FakeStorage storage;
  FakeClock clock{IClock::WallTime{std::chrono::hours{42}}};
  CheckpointMaintenance maintenance{storage, clock};

  const auto result = maintenance.apply_retention(container_id(), 2);

  ASSERT_TRUE(result);
  EXPECT_EQ(storage.begin_calls, 1U);
  EXPECT_EQ(storage.state->kind, StorageOperationKind::retention);
  EXPECT_EQ(storage.state->keep_last, 2U);
  EXPECT_TRUE(result->complete);
}

TEST(CheckpointMaintenanceTest,
     UsesConfiguredGraceAndBoundForGarbageCollection) {
  FakeStorage storage;
  const auto now = IClock::WallTime{std::chrono::hours{42}};
  FakeClock clock{now};
  CheckpointMaintenance maintenance{
      storage, clock,
      CheckpointMaintenanceConfig{
          .garbage_collection_grace_period = std::chrono::minutes{17},
          .maximum_garbage_deletions_per_pass = 23,
      }};

  const auto result = maintenance.collect_garbage(container_id());

  ASSERT_TRUE(result);
  ASSERT_TRUE(storage.state->garbage_options.has_value());
  EXPECT_EQ(storage.state->kind, StorageOperationKind::garbage_collection);
  EXPECT_EQ(storage.state->garbage_options->now, now);
  EXPECT_EQ(storage.state->garbage_options->grace_period,
            std::chrono::minutes{17});
  EXPECT_EQ(storage.state->garbage_options->maximum_deletions, 23U);
}

TEST(CheckpointMaintenanceTest, RejectsInvalidConfigBeforeAcquiringLease) {
  FakeStorage storage;
  FakeClock clock{IClock::WallTime{}};
  CheckpointMaintenance maintenance{
      storage, clock,
      CheckpointMaintenanceConfig{
          .garbage_collection_grace_period = std::chrono::seconds{-1},
          .maximum_garbage_deletions_per_pass = 0,
      }};

  const auto result = maintenance.collect_garbage(container_id());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::invalid_argument);
  EXPECT_EQ(storage.begin_calls, 0U);
}

TEST(CheckpointMaintenanceTest, PropagatesLeaseAndPassFailures) {
  FakeStorage storage;
  FakeClock clock{IClock::WallTime{}};
  CheckpointMaintenance maintenance{storage, clock};
  storage.begin_error = test_error("lease busy");

  const auto lease_failure = maintenance.apply_retention(container_id(), 3);
  ASSERT_FALSE(lease_failure);
  EXPECT_EQ(lease_failure.error().diagnostic, "lease busy");

  storage.begin_error.reset();
  storage.state->garbage_error = test_error("sweep failed");
  const auto pass_failure = maintenance.collect_garbage(container_id());
  ASSERT_FALSE(pass_failure);
  EXPECT_EQ(pass_failure.error().diagnostic, "sweep failed");
}

} // namespace
} // namespace gravesentinel::ccm
