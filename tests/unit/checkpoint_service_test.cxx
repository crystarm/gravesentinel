#include "src/core/checkpoint_service.hxx"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

using namespace std::chrono_literals;

template <typename Identifier>
[[nodiscard]] Identifier make_identifier(std::string value) {
  auto created = Identifier::create(std::move(value));
  if (!created) {
    throw std::runtime_error(created.error().diagnostic);
  }
  return std::move(*created);
}

[[nodiscard]] CheckpointErrorDetail make_test_error(StableErrorCode code,
                                                    std::string diagnostic) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

[[nodiscard]] ContainerRegistration
make_registration(std::string name, std::uint32_t container_cpu,
                  std::uint32_t checkpoint_cpu,
                  std::chrono::seconds interval = 60s) {
  const std::string runtime_name = "runtime-" + name;
  return ContainerRegistration{
      .container_id = make_identifier<ContainerId>(std::move(name)),
      .runtime_container = make_identifier<RuntimeContainerRef>(runtime_name),
      .checkpoint =
          CheckpointConfig{
              .interval = interval,
              .keep_last = 3U,
              .container_cpu = container_cpu,
              .checkpoint_cpu = checkpoint_cpu,
          },
      .cold_start = std::nullopt,
      .runtime_fingerprint = std::nullopt,
  };
}

[[nodiscard]] RequestId make_request(std::string value) {
  return make_identifier<RequestId>(std::move(value));
}

[[nodiscard]] OperationId make_operation(std::string value) {
  return make_identifier<OperationId>(std::move(value));
}

void expect_error(const CheckpointResult<void> &result,
                  StableErrorCode expected_code) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, expected_code);
}

class FakeTime final : public IClock, public ITimer {
public:
  [[nodiscard]] WallTime wall_now() const noexcept override {
    const std::scoped_lock lock{mutex_};
    return WallTime{std::chrono::duration_cast<WallTime::duration>(
        now_.time_since_epoch())};
  }

  [[nodiscard]] MonotonicTime monotonic_now() const noexcept override {
    std::unique_lock lock{mutex_};
    if (block_next_monotonic_read_) {
      block_next_monotonic_read_ = false;
      monotonic_read_blocked_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return monotonic_read_released_; });
    }
    return now_;
  }

  [[nodiscard]] Generation generation() const noexcept override {
    return generation_.load(std::memory_order_acquire);
  }

  [[nodiscard]] TimerWaitResult
  wait_until(MonotonicTime deadline, Generation observed_generation,
             std::stop_token stop_token) override {
    std::unique_lock lock{mutex_};
    highest_observed_generation_ =
        std::max(highest_observed_generation_, observed_generation);
    condition_.notify_all();
    static_cast<void>(condition_.wait(lock, stop_token, [&] {
      return now_ >= deadline ||
             generation_.load(std::memory_order_acquire) != observed_generation;
    }));
    if (stop_token.stop_requested()) {
      return TimerWaitResult::stopped;
    }
    if (now_ >= deadline) {
      return TimerWaitResult::deadline_reached;
    }
    return TimerWaitResult::interrupted;
  }

  void interrupt() noexcept override {
    interrupt_count_.fetch_add(1U, std::memory_order_release);
    generation_.fetch_add(1U, std::memory_order_acq_rel);
    condition_.notify_all();
  }

  void block_next_monotonic_read() {
    const std::scoped_lock lock{mutex_};
    block_next_monotonic_read_ = true;
    monotonic_read_blocked_ = false;
    monotonic_read_released_ = false;
  }

  void wait_for_blocked_monotonic_read() {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] { return monotonic_read_blocked_; });
  }

  void release_monotonic_read() {
    {
      const std::scoped_lock lock{mutex_};
      monotonic_read_released_ = true;
    }
    condition_.notify_all();
  }

  void advance_without_wait(std::chrono::seconds amount) {
    {
      const std::scoped_lock lock{mutex_};
      now_ += std::chrono::duration_cast<MonotonicTime::duration>(amount);
      generation_.fetch_add(1U, std::memory_order_acq_rel);
    }
    condition_.notify_all();
  }

  void advance(std::chrono::seconds amount) {
    Generation target_generation{};
    {
      const std::scoped_lock lock{mutex_};
      now_ += std::chrono::duration_cast<MonotonicTime::duration>(amount);
      target_generation =
          generation_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    }
    condition_.notify_all();

    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] {
      return highest_observed_generation_ >= target_generation;
    });
  }

  [[nodiscard]] std::uint64_t interrupt_count() const noexcept {
    return interrupt_count_.load(std::memory_order_acquire);
  }

  void wait_for_interrupt_count(std::uint64_t expected_count) {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] {
      return interrupt_count_.load(std::memory_order_acquire) >= expected_count;
    });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable_any condition_;
  MonotonicTime now_{};
  mutable bool block_next_monotonic_read_{false};
  mutable bool monotonic_read_blocked_{false};
  mutable bool monotonic_read_released_{false};
  std::atomic<Generation> generation_{0U};
  std::atomic<std::uint64_t> interrupt_count_{0U};
  Generation highest_observed_generation_{0U};
};

class FakeAffinity final : public IAffinityController {
public:
  explicit FakeAffinity(std::thread::id control_thread_id_value)
      : control_thread_id_(control_thread_id_value) {}

  void make_unavailable(std::uint32_t logical_cpu) {
    const std::scoped_lock lock{mutex_};
    unavailable_cpus_.insert(logical_cpu);
  }

  void return_null_binding_on_next_bind() {
    const std::scoped_lock lock{mutex_};
    return_null_binding_ = true;
  }

  void throw_on_next_restore() {
    const std::scoped_lock lock{mutex_};
    throw_next_restore_ = true;
  }

  [[nodiscard]] CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const override {
    const std::scoped_lock lock{mutex_};
    validated_cpus_.push_back(logical_cpu);
    if (unavailable_cpus_.contains(logical_cpu)) {
      return std::unexpected(make_test_error(
          StableErrorCode::unavailable,
          "logical CPU is unavailable to the fake affinity controller"));
    }
    validated_available_cpus_.insert(logical_cpu);
    return {};
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IThreadAffinityBinding>>
  bind_current_thread(std::uint32_t logical_cpu) override {
    const auto current_thread_id = std::this_thread::get_id();
    {
      const std::scoped_lock lock{mutex_};
      bind_cpus_.push_back(logical_cpu);
      if (current_thread_id == control_thread_id_) {
        bound_control_thread_ = true;
      }
      if (!validated_available_cpus_.contains(logical_cpu)) {
        unvalidated_bind_ = true;
      }
      if (std::exchange(return_null_binding_, false)) {
        return std::unique_ptr<IThreadAffinityBinding>{};
      }
      if (bound_threads_.contains(current_thread_id)) {
        duplicate_thread_bind_ = true;
      }
      bound_threads_[current_thread_id] = logical_cpu;
    }
    return std::unique_ptr<IThreadAffinityBinding>{
        std::make_unique<Binding>(*this, current_thread_id, logical_cpu)};
  }

  [[nodiscard]] std::optional<std::uint32_t> current_cpu() const {
    const std::scoped_lock lock{mutex_};
    const auto found = bound_threads_.find(std::this_thread::get_id());
    if (found == bound_threads_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  [[nodiscard]] std::vector<std::uint32_t> bind_cpus() const {
    const std::scoped_lock lock{mutex_};
    return bind_cpus_;
  }

  [[nodiscard]] std::vector<std::uint32_t> restored_cpus() const {
    const std::scoped_lock lock{mutex_};
    return restored_cpus_;
  }

  [[nodiscard]] bool bound_control_thread() const {
    const std::scoped_lock lock{mutex_};
    return bound_control_thread_;
  }

  [[nodiscard]] bool all_binds_were_prevalidated() const {
    const std::scoped_lock lock{mutex_};
    return !unvalidated_bind_;
  }

  [[nodiscard]] bool all_restores_used_binding_thread() const {
    const std::scoped_lock lock{mutex_};
    return !wrong_thread_restore_;
  }

  [[nodiscard]] bool duplicate_thread_bind() const {
    const std::scoped_lock lock{mutex_};
    return duplicate_thread_bind_;
  }

  [[nodiscard]] std::size_t active_binding_count() const {
    const std::scoped_lock lock{mutex_};
    return bound_threads_.size();
  }

private:
  class Binding final : public IThreadAffinityBinding {
  public:
    Binding(FakeAffinity &owner_value, std::thread::id binding_thread_id_value,
            std::uint32_t logical_cpu_value)
        : owner_(owner_value), binding_thread_id_(binding_thread_id_value),
          logical_cpu_(logical_cpu_value) {}

    [[nodiscard]] CheckpointResult<void> restore() override {
      return owner_.restore_binding(binding_thread_id_, logical_cpu_);
    }

  private:
    FakeAffinity &owner_;
    std::thread::id binding_thread_id_;
    std::uint32_t logical_cpu_;
  };

  [[nodiscard]] CheckpointResult<void>
  restore_binding(std::thread::id binding_thread_id,
                  std::uint32_t logical_cpu) {
    const auto current_thread_id = std::this_thread::get_id();
    const std::scoped_lock lock{mutex_};
    if (std::exchange(throw_next_restore_, false)) {
      throw std::runtime_error("injected affinity restore exception");
    }
    if (current_thread_id != binding_thread_id) {
      wrong_thread_restore_ = true;
      return std::unexpected(make_test_error(
          StableErrorCode::internal,
          "affinity binding was restored on a different thread"));
    }
    const auto found = bound_threads_.find(binding_thread_id);
    if (found == bound_threads_.end() || found->second != logical_cpu) {
      wrong_thread_restore_ = true;
      return std::unexpected(make_test_error(
          StableErrorCode::internal,
          "affinity binding was not active when it was restored"));
    }
    restored_cpus_.push_back(logical_cpu);
    bound_threads_.erase(found);
    return {};
  }

  std::thread::id control_thread_id_;
  mutable std::mutex mutex_;
  std::set<std::uint32_t> unavailable_cpus_;
  mutable std::vector<std::uint32_t> validated_cpus_;
  mutable std::set<std::uint32_t> validated_available_cpus_;
  std::map<std::thread::id, std::uint32_t> bound_threads_;
  std::vector<std::uint32_t> bind_cpus_;
  std::vector<std::uint32_t> restored_cpus_;
  bool bound_control_thread_{false};
  bool unvalidated_bind_{false};
  bool wrong_thread_restore_{false};
  bool duplicate_thread_bind_{false};
  bool return_null_binding_{false};
  bool throw_next_restore_{false};
};

enum class ObservedOperationKind : std::uint8_t { create, restore };

struct OperationObservation {
  ObservedOperationKind kind;
  std::string container_id;
  std::thread::id thread_id;
  std::optional<std::uint32_t> worker_cpu;
};

class FakeCheckpointOperations final : public ICheckpointOperations {
public:
  explicit FakeCheckpointOperations(FakeAffinity &affinity_value)
      : affinity_(affinity_value) {}

  void block_next_calls(std::size_t count) {
    const std::scoped_lock lock{mutex_};
    blocking_entries_remaining_ = count;
    blocked_calls_released_ = false;
  }

  void release_blocked_calls() {
    {
      const std::scoped_lock lock{mutex_};
      blocked_calls_released_ = true;
    }
    condition_.notify_all();
  }

  void throw_on_next_create() {
    const std::scoped_lock lock{mutex_};
    throw_next_create_ = true;
  }

  void throw_on_next_restore() {
    const std::scoped_lock lock{mutex_};
    throw_next_restore_ = true;
  }

  void run_during_next_create(std::function<void()> callback) {
    const std::scoped_lock lock{mutex_};
    next_create_callback_ = std::move(callback);
  }

  void wait_for_active_calls(std::size_t expected_count) {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] { return active_calls_ >= expected_count; });
  }

  void wait_for_total_calls(std::size_t expected_count) {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] { return total_calls_ >= expected_count; });
  }

  void wait_for_no_active_calls() {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] { return active_calls_ == 0U; });
  }

  [[nodiscard]] std::size_t total_calls() const {
    const std::scoped_lock lock{mutex_};
    return total_calls_;
  }

  [[nodiscard]] std::size_t maximum_active_calls() const {
    const std::scoped_lock lock{mutex_};
    return maximum_active_calls_;
  }

  [[nodiscard]] std::size_t
  maximum_active_calls_for(const ContainerId &container_id) const {
    const std::scoped_lock lock{mutex_};
    const auto found = maximum_active_by_container_.find(container_id.value());
    return found == maximum_active_by_container_.end() ? 0U : found->second;
  }

  [[nodiscard]] std::vector<OperationObservation> observations() const {
    const std::scoped_lock lock{mutex_};
    return observations_;
  }

  [[nodiscard]] std::vector<CheckpointCreateRequest> create_requests() const {
    const std::scoped_lock lock{mutex_};
    return create_requests_;
  }

  [[nodiscard]] std::vector<RestoreRequest> restore_requests() const {
    const std::scoped_lock lock{mutex_};
    return restore_requests_;
  }

  [[nodiscard]] CheckpointResult<CreatedCheckpoint>
  create(const CheckpointCreateRequest &request) override {
    auto callback = take_create_callback();
    if (callback) {
      callback();
    }
    auto context = enter_create(request);
    wait_if_blocked(context.blocked);
    leave_call(context.container_id);
    if (context.throw_exception) {
      throw std::runtime_error("injected checkpoint operation exception");
    }

    return CreatedCheckpoint{
        .operation_id = request.operation_id,
        .checkpoint_id = make_identifier<CheckpointId>(
            "checkpoint-" + std::to_string(context.sequence)),
        .generation = context.sequence,
        .source_incarnation =
            RuntimeIncarnation{
                .reference = request.runtime_container,
                .runtime_id = "source-" + request.operation_id.value(),
            },
        .staging_cleanup_complete = true,
    };
  }

  [[nodiscard]] CheckpointResult<RestoredContainer>
  restore_or_restart(const RestoreRequest &request) override {
    auto context = enter_restore(request);
    wait_if_blocked(context.blocked);
    leave_call(context.container_id);
    if (context.throw_exception) {
      throw std::runtime_error("injected restore operation exception");
    }

    return RestoredContainer{
        .operation_id = request.operation_id,
        .incarnation =
            RuntimeIncarnation{
                .reference = request.runtime_container,
                .runtime_id = "restored-" + request.operation_id.value(),
            },
        .outcome = RestoreOutcome::checkpoint,
        .checkpoint_id = std::nullopt,
        .generation = std::nullopt,
        .pin_release_complete = true,
        .staging_cleanup_complete = true,
    };
  }

private:
  struct CallContext {
    std::string container_id;
    std::uint64_t sequence;
    bool blocked;
    bool throw_exception;
  };

  [[nodiscard]] CallContext
  enter_create(const CheckpointCreateRequest &request) {
    const auto worker_cpu = affinity_.current_cpu();
    const auto current_thread_id = std::this_thread::get_id();
    std::scoped_lock lock{mutex_};
    create_requests_.push_back(request);
    const bool throw_exception = std::exchange(throw_next_create_, false);
    return enter_call_locked(ObservedOperationKind::create,
                             request.container_id.value(), current_thread_id,
                             worker_cpu, throw_exception);
  }

  [[nodiscard]] CallContext enter_restore(const RestoreRequest &request) {
    const auto worker_cpu = affinity_.current_cpu();
    const auto current_thread_id = std::this_thread::get_id();
    std::scoped_lock lock{mutex_};
    restore_requests_.push_back(request);
    const bool throw_exception = std::exchange(throw_next_restore_, false);
    return enter_call_locked(ObservedOperationKind::restore,
                             request.container_id.value(), current_thread_id,
                             worker_cpu, throw_exception);
  }

  [[nodiscard]] std::function<void()> take_create_callback() {
    const std::scoped_lock lock{mutex_};
    return std::exchange(next_create_callback_, {});
  }

  [[nodiscard]] CallContext
  enter_call_locked(ObservedOperationKind kind, std::string container_id,
                    std::thread::id thread_id,
                    std::optional<std::uint32_t> worker_cpu,
                    bool throw_exception) {
    ++total_calls_;
    ++active_calls_;
    maximum_active_calls_ = std::max(maximum_active_calls_, active_calls_);
    auto &container_active = active_by_container_[container_id];
    ++container_active;
    auto &container_maximum = maximum_active_by_container_[container_id];
    container_maximum = std::max(container_maximum, container_active);
    observations_.push_back(OperationObservation{
        .kind = kind,
        .container_id = container_id,
        .thread_id = thread_id,
        .worker_cpu = worker_cpu,
    });

    const bool blocked = blocking_entries_remaining_ > 0U;
    if (blocked) {
      --blocking_entries_remaining_;
    }
    const auto sequence = next_sequence_++;
    condition_.notify_all();
    return CallContext{
        .container_id = std::move(container_id),
        .sequence = sequence,
        .blocked = blocked,
        .throw_exception = throw_exception,
    };
  }

  void wait_if_blocked(bool blocked) {
    if (!blocked) {
      return;
    }
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [&] { return blocked_calls_released_; });
  }

  void leave_call(const std::string &container_id) {
    {
      const std::scoped_lock lock{mutex_};
      --active_calls_;
      --active_by_container_.at(container_id);
    }
    condition_.notify_all();
  }

  FakeAffinity &affinity_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t blocking_entries_remaining_{0U};
  bool blocked_calls_released_{true};
  bool throw_next_create_{false};
  bool throw_next_restore_{false};
  std::function<void()> next_create_callback_;
  std::size_t total_calls_{0U};
  std::size_t active_calls_{0U};
  std::size_t maximum_active_calls_{0U};
  std::uint64_t next_sequence_{1U};
  std::map<std::string, std::size_t> active_by_container_;
  std::map<std::string, std::size_t> maximum_active_by_container_;
  std::vector<OperationObservation> observations_;
  std::vector<CheckpointCreateRequest> create_requests_;
  std::vector<RestoreRequest> restore_requests_;
};

class FakeIdGenerator final : public IIdGenerator {
public:
  [[nodiscard]] CheckpointResult<CheckpointId> new_checkpoint_id() override {
    const auto sequence =
        checkpoint_sequence_.fetch_add(1U, std::memory_order_relaxed);
    return make_identifier<CheckpointId>("generated-checkpoint-" +
                                         std::to_string(sequence));
  }

  [[nodiscard]] CheckpointResult<OperationId> new_operation_id() override {
    const auto sequence =
        operation_sequence_.fetch_add(1U, std::memory_order_relaxed);
    return make_identifier<OperationId>("generated-operation-" +
                                        std::to_string(sequence));
  }

  [[nodiscard]] std::uint64_t operation_count() const noexcept {
    return operation_sequence_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> checkpoint_sequence_{0U};
  std::atomic<std::uint64_t> operation_sequence_{0U};
};

class FakeLogger final : public ILogger {
public:
  void log(LogLevel level, std::string_view,
           std::span<const LogField>) noexcept override {
    entry_count_.fetch_add(1U, std::memory_order_relaxed);
    if (level == LogLevel::error) {
      error_count_.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::uint64_t entry_count() const noexcept {
    return entry_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t error_count() const noexcept {
    return error_count_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> entry_count_{0U};
  std::atomic<std::uint64_t> error_count_{0U};
};

class TestRig final {
public:
  TestRig()
      : control_thread_id(std::this_thread::get_id()),
        affinity(control_thread_id), operations(affinity),
        service(operations, time, time, ids, affinity, logger) {}

  std::thread::id control_thread_id;
  FakeTime time;
  FakeAffinity affinity;
  FakeCheckpointOperations operations;
  FakeIdGenerator ids;
  FakeLogger logger;
  CheckpointService service;
};

TEST(CheckpointServiceTest,
     RejectsDuplicateContainerCpuAssignmentsAndUnavailableCpu) {
  TestRig rig;
  const auto primary = make_registration("container-primary", 0U, 1U);
  ASSERT_TRUE(
      rig.service.register_container(primary, make_request("register-1")));

  const auto duplicate_id = rig.service.register_container(
      make_registration("container-primary", 2U, 3U),
      make_request("register-duplicate-id"));
  expect_error(duplicate_id, StableErrorCode::already_exists);

  const auto duplicate_cpu = rig.service.register_container(
      make_registration("container-secondary", 0U, 1U),
      make_request("register-duplicate-cpu"));
  expect_error(duplicate_cpu, StableErrorCode::invalid_argument);

  rig.affinity.make_unavailable(3U);
  const auto unavailable_cpu = rig.service.register_container(
      make_registration("container-unavailable", 2U, 3U),
      make_request("register-unavailable"));
  expect_error(unavailable_cpu, StableErrorCode::unavailable);
  ASSERT_TRUE(unavailable_cpu.error().correlation_id.has_value());
  EXPECT_EQ(unavailable_cpu.error().correlation_id->value(),
            "register-unavailable");

  EXPECT_EQ(rig.affinity.bind_cpus(), std::vector<std::uint32_t>({1U}));
  EXPECT_TRUE(rig.service.shutdown());
  EXPECT_TRUE(rig.affinity.all_binds_were_prevalidated());
  EXPECT_TRUE(rig.affinity.all_restores_used_binding_thread());
}

TEST(CheckpointServiceTest,
     RunsFourContainersInParallelOnWorkerCpusAndRejectsFifthSlot) {
  TestRig rig;
  const std::array registrations{
      make_registration("container-0", 0U, 1U),
      make_registration("container-1", 2U, 3U),
      make_registration("container-2", 4U, 5U),
      make_registration("container-3", 6U, 7U),
  };
  for (std::size_t index = 0; index < registrations.size(); ++index) {
    ASSERT_TRUE(rig.service.register_container(
        registrations[index],
        make_request("register-" + std::to_string(index))));
  }

  const auto fifth = rig.service.register_container(
      make_registration("container-4", 0U, 1U), make_request("register-fifth"));
  expect_error(fifth, StableErrorCode::resource_exhausted);

  rig.operations.block_next_calls(registrations.size());
  std::array<std::optional<CheckpointResult<void>>, 4U> results;
  std::array<std::jthread, 4U> callers;
  for (std::size_t index = 0; index < callers.size(); ++index) {
    callers[index] = std::jthread([&, index] {
      results[index] = rig.service.create_checkpoint(
          registrations[index].container_id,
          make_request("create-" + std::to_string(index)),
          make_operation("operation-" + std::to_string(index)));
    });
  }

  rig.operations.wait_for_active_calls(registrations.size());
  EXPECT_EQ(rig.operations.maximum_active_calls(), registrations.size());
  EXPECT_EQ(rig.operations.total_calls(), registrations.size());

  const std::map<std::string, std::uint32_t> expected_worker_cpus{
      {"container-0", 1U},
      {"container-1", 3U},
      {"container-2", 5U},
      {"container-3", 7U},
  };
  const auto observations = rig.operations.observations();
  ASSERT_EQ(observations.size(), registrations.size());
  for (const auto &observation : observations) {
    ASSERT_TRUE(observation.worker_cpu.has_value());
    EXPECT_EQ(*observation.worker_cpu,
              expected_worker_cpus.at(observation.container_id));
    EXPECT_NE(observation.thread_id, rig.control_thread_id);
  }

  rig.operations.release_blocked_calls();
  for (auto &caller : callers) {
    caller.join();
  }
  for (const auto &result : results) {
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
  }

  EXPECT_TRUE(rig.service.shutdown());
  EXPECT_EQ(rig.affinity.bind_cpus(),
            std::vector<std::uint32_t>({1U, 3U, 5U, 7U}));
  auto restored_cpus = rig.affinity.restored_cpus();
  std::ranges::sort(restored_cpus);
  EXPECT_EQ(restored_cpus, std::vector<std::uint32_t>({1U, 3U, 5U, 7U}));
  EXPECT_FALSE(rig.affinity.bound_control_thread());
  EXPECT_FALSE(rig.affinity.duplicate_thread_bind());
  EXPECT_TRUE(rig.affinity.all_binds_were_prevalidated());
  EXPECT_TRUE(rig.affinity.all_restores_used_binding_thread());
  EXPECT_EQ(rig.affinity.active_binding_count(), 0U);
}

TEST(CheckpointServiceTest, SerializesOperationsForTheSameContainer) {
  TestRig rig;
  const auto registration = make_registration("serial-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(registration,
                                             make_request("register-serial")));

  rig.operations.block_next_calls(1U);
  std::optional<CheckpointResult<void>> first_result;
  std::optional<CheckpointResult<void>> second_result;
  std::jthread first([&] {
    first_result = rig.service.create_checkpoint(
        registration.container_id, make_request("serial-first"),
        make_operation("serial-operation-first"));
  });
  rig.operations.wait_for_active_calls(1U);

  std::latch second_started{1};
  std::jthread second([&] {
    second_started.count_down();
    second_result = rig.service.restore_or_restart(
        registration.container_id, make_request("serial-second"),
        make_operation("serial-operation-second"));
  });
  second_started.wait();

  rig.operations.release_blocked_calls();
  first.join();
  second.join();

  ASSERT_TRUE(first_result.has_value());
  ASSERT_TRUE(second_result.has_value());
  EXPECT_TRUE(*first_result);
  EXPECT_TRUE(*second_result);
  EXPECT_EQ(rig.operations.total_calls(), 2U);
  EXPECT_EQ(rig.operations.maximum_active_calls_for(registration.container_id),
            1U);
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest,
     PeriodicSixtySecondTicksCoalesceMissedIntervalsWithoutCatchUp) {
  TestRig rig;
  const auto registration = make_registration("periodic-container", 0U, 1U);
  rig.operations.block_next_calls(1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-periodic")));

  rig.time.advance(59s);
  EXPECT_EQ(rig.operations.total_calls(), 0U);

  rig.time.advance(1s);
  rig.operations.wait_for_active_calls(1U);
  EXPECT_EQ(rig.operations.total_calls(), 1U);

  rig.time.advance(180s);
  EXPECT_EQ(rig.operations.total_calls(), 1U);

  rig.operations.release_blocked_calls();
  rig.operations.wait_for_no_active_calls();
  const auto synchronization_result = rig.service.create_checkpoint(
      registration.container_id, make_request("manual-synchronization"),
      make_operation("manual-synchronization-operation"));
  ASSERT_TRUE(synchronization_result);

  auto requests = rig.operations.create_requests();
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests.front().request_id.value(),
            "periodic-periodic-container-1");
  EXPECT_EQ(requests.back().request_id.value(), "manual-synchronization");

  rig.time.advance(59s);
  EXPECT_EQ(rig.operations.total_calls(), 2U);
  rig.time.advance(1s);
  rig.operations.wait_for_total_calls(3U);

  requests = rig.operations.create_requests();
  ASSERT_EQ(requests.size(), 3U);
  EXPECT_EQ(requests.back().request_id.value(),
            "periodic-periodic-container-2");
  EXPECT_EQ(rig.ids.operation_count(), 2U);
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest,
     UnregisterWaitsForActiveOperationRejectsClosingIdAndFreesSlot) {
  TestRig rig;
  const std::array registrations{
      make_registration("unregister-0", 0U, 1U),
      make_registration("unregister-1", 2U, 3U),
      make_registration("unregister-2", 4U, 5U),
      make_registration("unregister-3", 6U, 7U),
  };
  for (std::size_t index = 0; index < registrations.size(); ++index) {
    ASSERT_TRUE(rig.service.register_container(
        registrations[index],
        make_request("register-unregister-" + std::to_string(index))));
  }

  const auto replacement = make_registration("replacement", 0U, 1U);
  expect_error(rig.service.register_container(replacement,
                                              make_request("replacement-full")),
               StableErrorCode::resource_exhausted);

  rig.operations.block_next_calls(1U);
  std::optional<CheckpointResult<void>> operation_result;
  std::jthread operation_caller([&] {
    operation_result = rig.service.create_checkpoint(
        registrations.front().container_id,
        make_request("active-before-unregister"),
        make_operation("active-before-unregister-operation"));
  });
  rig.operations.wait_for_active_calls(1U);

  const auto interrupts_before_unregister = rig.time.interrupt_count();
  std::optional<CheckpointResult<void>> unregister_result;
  std::jthread unregister_caller([&] {
    unregister_result = rig.service.unregister_container(
        registrations.front().container_id, make_request("unregister-active"));
  });
  rig.time.wait_for_interrupt_count(interrupts_before_unregister + 1U);

  const auto rejected_while_closing = rig.service.create_checkpoint(
      registrations.front().container_id, make_request("closing-create"),
      make_operation("closing-operation"));
  expect_error(rejected_while_closing, StableErrorCode::unavailable);

  rig.operations.release_blocked_calls();
  operation_caller.join();
  unregister_caller.join();
  ASSERT_TRUE(operation_result.has_value());
  ASSERT_TRUE(unregister_result.has_value());
  EXPECT_TRUE(*operation_result);
  EXPECT_TRUE(*unregister_result);

  expect_error(
      rig.service.create_checkpoint(registrations.front().container_id,
                                    make_request("removed-create"),
                                    make_operation("removed-operation")),
      StableErrorCode::not_found);
  EXPECT_TRUE(rig.service.register_container(
      replacement, make_request("replacement-after-unregister")));
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest,
     ShutdownDrainsActiveOperationIsIdempotentAndRejectsNewWork) {
  TestRig rig;
  const auto registration = make_registration("shutdown-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-shutdown")));

  rig.operations.block_next_calls(1U);
  std::optional<CheckpointResult<void>> operation_result;
  std::jthread operation_caller([&] {
    operation_result = rig.service.create_checkpoint(
        registration.container_id, make_request("active-before-shutdown"),
        make_operation("active-before-shutdown-operation"));
  });
  rig.operations.wait_for_active_calls(1U);

  const auto interrupts_before_shutdown = rig.time.interrupt_count();
  std::atomic<bool> shutdown_finished{false};
  std::optional<CheckpointResult<void>> shutdown_result;
  std::jthread shutdown_caller([&] {
    shutdown_result = rig.service.shutdown();
    shutdown_finished.store(true, std::memory_order_release);
  });
  rig.time.wait_for_interrupt_count(interrupts_before_shutdown + 1U);

  EXPECT_FALSE(shutdown_finished.load(std::memory_order_acquire));
  expect_error(rig.service.restore_or_restart(
                   registration.container_id,
                   make_request("restore-during-shutdown"),
                   make_operation("restore-during-shutdown-operation")),
               StableErrorCode::unavailable);

  rig.operations.release_blocked_calls();
  operation_caller.join();
  shutdown_caller.join();
  ASSERT_TRUE(operation_result.has_value());
  ASSERT_TRUE(shutdown_result.has_value());
  EXPECT_TRUE(*operation_result);
  EXPECT_TRUE(*shutdown_result);
  EXPECT_TRUE(shutdown_finished.load(std::memory_order_acquire));

  EXPECT_TRUE(rig.service.shutdown());
  EXPECT_TRUE(rig.service.shutdown());
  expect_error(rig.service.create_checkpoint(
                   registration.container_id,
                   make_request("create-after-shutdown"),
                   make_operation("create-after-shutdown-operation")),
               StableErrorCode::unavailable);
  expect_error(rig.service.register_container(
                   make_registration("after-shutdown", 2U, 3U),
                   make_request("register-after-shutdown")),
               StableErrorCode::unavailable);
  EXPECT_EQ(rig.affinity.active_binding_count(), 0U);
}

TEST(CheckpointServiceTest, ShutdownCutoffPreventsDuePeriodicAdmission) {
  TestRig rig;
  const auto registration = make_registration("shutdown-periodic", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-shutdown-periodic")));

  rig.time.block_next_monotonic_read();
  rig.time.advance_without_wait(60s);
  rig.time.wait_for_blocked_monotonic_read();

  const auto interrupts_before_shutdown = rig.time.interrupt_count();
  std::optional<CheckpointResult<void>> shutdown_result;
  std::jthread shutdown_caller(
      [&] { shutdown_result = rig.service.shutdown(); });
  rig.time.wait_for_interrupt_count(interrupts_before_shutdown + 1U);
  rig.time.release_monotonic_read();
  shutdown_caller.join();

  ASSERT_TRUE(shutdown_result.has_value());
  EXPECT_TRUE(*shutdown_result);
  EXPECT_EQ(rig.operations.total_calls(), 0U);
}

TEST(CheckpointServiceTest, ConvertsExceptionsAndReusesTheWorker) {
  TestRig rig;
  const auto registration = make_registration("exception-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-exception")));

  rig.operations.throw_on_next_create();
  const auto failed = rig.service.create_checkpoint(
      registration.container_id, make_request("throwing-request"),
      make_operation("throwing-operation"));
  expect_error(failed, StableErrorCode::internal);
  ASSERT_TRUE(failed.error().correlation_id.has_value());
  EXPECT_EQ(failed.error().correlation_id->value(), "throwing-request");
  EXPECT_NE(failed.error().diagnostic.find("injected checkpoint operation"),
            std::string::npos);

  const auto recovered = rig.service.create_checkpoint(
      registration.container_id, make_request("recovered-request"),
      make_operation("recovered-operation"));
  EXPECT_TRUE(recovered);

  const auto observations = rig.operations.observations();
  ASSERT_EQ(observations.size(), 2U);
  EXPECT_EQ(observations.front().thread_id, observations.back().thread_id);
  EXPECT_EQ(rig.operations.maximum_active_calls_for(registration.container_id),
            1U);
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest, ConvertsBadAllocWithoutStoppingWorker) {
  TestRig rig;
  const auto registration = make_registration("bad-alloc-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-bad-alloc")));

  rig.operations.run_during_next_create([] { throw std::bad_alloc{}; });
  const auto failed = rig.service.create_checkpoint(
      registration.container_id, make_request("bad-alloc-request"),
      make_operation("bad-alloc-operation"));
  expect_error(failed, StableErrorCode::resource_exhausted);
  EXPECT_TRUE(failed.error().retryable);
  ASSERT_TRUE(failed.error().correlation_id.has_value());
  EXPECT_EQ(failed.error().correlation_id->value(), "bad-alloc-request");

  EXPECT_TRUE(rig.service.create_checkpoint(
      registration.container_id, make_request("after-bad-alloc"),
      make_operation("after-bad-alloc-operation")));
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest, ForwardsCreatedIncarnationToRestore) {
  TestRig rig;
  const auto registration = make_registration("incarnation-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-incarnation")));

  const auto create_result = rig.service.create_checkpoint(
      registration.container_id, make_request("create-incarnation"),
      make_operation("source-operation"));
  ASSERT_TRUE(create_result);
  const auto restore_result = rig.service.restore_or_restart(
      registration.container_id, make_request("restore-incarnation"),
      make_operation("restore-operation"));
  ASSERT_TRUE(restore_result);

  const auto restore_requests = rig.operations.restore_requests();
  ASSERT_EQ(restore_requests.size(), 1U);
  ASSERT_TRUE(restore_requests.front().expected_source_incarnation.has_value());
  EXPECT_EQ(restore_requests.front().expected_source_incarnation->reference,
            registration.runtime_container);
  EXPECT_EQ(restore_requests.front().expected_source_incarnation->runtime_id,
            "source-source-operation");
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest, RejectsSuccessfulNullAffinityBinding) {
  TestRig rig;
  rig.affinity.return_null_binding_on_next_bind();

  const auto result =
      rig.service.register_container(make_registration("null-affinity", 0U, 1U),
                                     make_request("register-null-affinity"));
  expect_error(result, StableErrorCode::internal);
  EXPECT_EQ(rig.affinity.active_binding_count(), 0U);
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest, ContainsAffinityRestoreExceptions) {
  TestRig rig;
  const auto registration = make_registration("affinity-exception", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-affinity-exception")));
  rig.affinity.throw_on_next_restore();

  const auto result = rig.service.shutdown();
  expect_error(result, StableErrorCode::internal);
  EXPECT_NE(
      result.error().diagnostic.find("injected affinity restore exception"),
      std::string::npos);
  expect_error(rig.service.shutdown(), StableErrorCode::internal);
}

TEST(CheckpointServiceTest,
     RejectsReentrantLifecycleCallWhileUnregisterWaitsForWorker) {
  TestRig rig;
  const auto registration = make_registration("reentrant-container", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-reentrant")));

  std::latch callback_entered{1};
  std::latch invoke_reentrant_call{1};
  std::optional<CheckpointResult<void>> reentrant_result;
  rig.operations.run_during_next_create([&] {
    callback_entered.count_down();
    invoke_reentrant_call.wait();
    reentrant_result = rig.service.shutdown();
  });

  std::optional<CheckpointResult<void>> create_result;
  std::jthread create_caller([&] {
    create_result = rig.service.create_checkpoint(
        registration.container_id, make_request("reentrant-create"),
        make_operation("reentrant-operation"));
  });
  callback_entered.wait();

  const auto interrupts_before_unregister = rig.time.interrupt_count();
  std::optional<CheckpointResult<void>> unregister_result;
  std::jthread unregister_caller([&] {
    unregister_result = rig.service.unregister_container(
        registration.container_id, make_request("unregister-reentrant"));
  });
  rig.time.wait_for_interrupt_count(interrupts_before_unregister + 1U);
  invoke_reentrant_call.count_down();

  create_caller.join();
  unregister_caller.join();
  ASSERT_TRUE(create_result.has_value());
  ASSERT_TRUE(unregister_result.has_value());
  ASSERT_TRUE(reentrant_result.has_value());
  EXPECT_TRUE(*create_result);
  EXPECT_TRUE(*unregister_result);
  expect_error(*reentrant_result, StableErrorCode::conflict);
  EXPECT_TRUE(rig.service.shutdown());
}

TEST(CheckpointServiceTest, ClearsIncarnationHintWhenRestoreThrows) {
  TestRig rig;
  const auto registration = make_registration("restore-exception", 0U, 1U);
  ASSERT_TRUE(rig.service.register_container(
      registration, make_request("register-restore-exception")));
  ASSERT_TRUE(rig.service.create_checkpoint(
      registration.container_id,
      make_request("create-before-restore-exception"),
      make_operation("source-before-restore-exception")));

  rig.operations.throw_on_next_restore();
  const auto failed = rig.service.restore_or_restart(
      registration.container_id, make_request("throwing-restore"),
      make_operation("throwing-restore-operation"));
  expect_error(failed, StableErrorCode::internal);

  const auto recovered = rig.service.restore_or_restart(
      registration.container_id, make_request("restore-after-exception"),
      make_operation("restore-after-exception-operation"));
  ASSERT_TRUE(recovered);

  const auto restore_requests = rig.operations.restore_requests();
  ASSERT_EQ(restore_requests.size(), 2U);
  EXPECT_TRUE(restore_requests.front().expected_source_incarnation.has_value());
  EXPECT_FALSE(restore_requests.back().expected_source_incarnation.has_value());
  EXPECT_TRUE(rig.service.shutdown());
}

} // namespace
} // namespace gravesentinel::ccm
