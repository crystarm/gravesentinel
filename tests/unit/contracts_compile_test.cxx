#include "src/core/contracts.hxx"
#include "src/scheduler_client/hypervisor_client.hxx"

#include <gtest/gtest.h>

#include <expected>
#include <type_traits>

#ifndef GRAVESENTINEL_CXX_STANDARD
#error "project C++ standard marker is missing"
#endif

static_assert(GRAVESENTINEL_CXX_STANDARD == 23);
static_assert(__cplusplus >= 202100L);

namespace gravesentinel::ccm {
namespace {

static_assert(std::is_abstract_v<IStorage>);
static_assert(std::is_abstract_v<IStorageOperation>);
static_assert(std::is_abstract_v<IStoragePin>);
static_assert(std::is_abstract_v<IPodmanAdapter>);
static_assert(std::is_abstract_v<IClock>);
static_assert(std::is_abstract_v<ITimer>);
static_assert(std::is_abstract_v<IHasher>);
static_assert(std::is_abstract_v<IIdGenerator>);
static_assert(std::is_abstract_v<ILogger>);
static_assert(std::is_abstract_v<IThreadAffinityBinding>);
static_assert(std::is_abstract_v<IAffinityController>);
static_assert(std::is_abstract_v<IProcessRunner>);
static_assert(std::is_abstract_v<ICheckpointService>);
static_assert(std::is_abstract_v<IHypervisorClient>);

static_assert(std::has_virtual_destructor_v<IStorage>);
static_assert(std::has_virtual_destructor_v<IStorageOperation>);
static_assert(std::has_virtual_destructor_v<IStoragePin>);
static_assert(std::has_virtual_destructor_v<IPodmanAdapter>);
static_assert(std::has_virtual_destructor_v<ITimer>);
static_assert(std::has_virtual_destructor_v<IThreadAffinityBinding>);
static_assert(std::has_virtual_destructor_v<IAffinityController>);
static_assert(std::has_virtual_destructor_v<IProcessRunner>);
static_assert(std::has_virtual_destructor_v<ICheckpointService>);
static_assert(std::has_virtual_destructor_v<IHypervisorClient>);

TEST(ContractTest, InternalErrorsRemainStructured) {
  const auto request_id = RequestId::create("request-42").value();
  const CheckpointResult<int> result = std::unexpected(CheckpointErrorDetail{
      .code = StableErrorCode::storage_error,
      .diagnostic = "manifest publication failed",
      .retryable = true,
      .correlation_id = request_id,
      .storage_side_effect = StorageSideEffect::publication_may_be_visible,
      .runtime_side_effect = RuntimeSideEffect::none,
  });

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::storage_error);
  EXPECT_TRUE(result.error().retryable);
  EXPECT_EQ(result.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  ASSERT_TRUE(result.error().correlation_id.has_value());
  EXPECT_EQ(result.error().correlation_id->value(), "request-42");
}

TEST(ContractTest, OperationStatusSeparatesFailureFromIndeterminate) {
  const auto failed = OperationStatus{
      .operation_id = OperationId::create("operation-failed").value(),
      .state = OperationState::failed,
      .error = StableErrorCode::restore_failed,
  };
  const auto indeterminate = OperationStatus{
      .operation_id = OperationId::create("operation-unknown").value(),
      .state = OperationState::indeterminate,
      .error = StableErrorCode::unavailable,
  };

  EXPECT_NE(failed.state, indeterminate.state);
  EXPECT_EQ(failed.error, StableErrorCode::restore_failed);
  EXPECT_EQ(indeterminate.error, StableErrorCode::unavailable);
}

} // namespace
} // namespace gravesentinel::ccm
