#include "src/core/checkpoint_service.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace gravesentinel::ccm {
namespace {

constexpr std::size_t kMaximumRegistrations = 4;
constexpr auto kGarbageCollectionInterval = std::chrono::minutes{5};
constexpr auto kMaintenanceRetryInterval = std::chrono::seconds{30};

[[nodiscard]] constexpr std::string_view
error_code_name(StableErrorCode code) noexcept {
  switch (code) {
  case StableErrorCode::invalid_argument:
    return "invalid_argument";
  case StableErrorCode::not_found:
    return "not_found";
  case StableErrorCode::already_exists:
    return "already_exists";
  case StableErrorCode::resource_exhausted:
    return "resource_exhausted";
  case StableErrorCode::conflict:
    return "conflict";
  case StableErrorCode::permission_denied:
    return "permission_denied";
  case StableErrorCode::capture_failed:
    return "capture_failed";
  case StableErrorCode::storage_error:
    return "storage_error";
  case StableErrorCode::corrupted_snapshot:
    return "corrupted_snapshot";
  case StableErrorCode::incompatible_runtime:
    return "incompatible_runtime";
  case StableErrorCode::restore_failed:
    return "restore_failed";
  case StableErrorCode::cold_start_failed:
    return "cold_start_failed";
  case StableErrorCode::deadline_exceeded:
    return "deadline_exceeded";
  case StableErrorCode::unavailable:
    return "unavailable";
  case StableErrorCode::internal:
    return "internal";
  }
  return "unknown";
}

[[nodiscard]] CheckpointErrorDetail
make_error(StableErrorCode code, std::string diagnostic,
           const RequestId *request_id = nullptr, bool retryable = false,
           StorageSideEffect storage_side_effect = StorageSideEffect::none,
           RuntimeSideEffect runtime_side_effect = RuntimeSideEffect::none) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = request_id == nullptr
                            ? std::optional<RequestId>{}
                            : std::optional<RequestId>{*request_id},
      .storage_side_effect = storage_side_effect,
      .runtime_side_effect = runtime_side_effect,
  };
}

void attach_correlation(CheckpointErrorDetail &error,
                        const RequestId &request_id) {
  if (!error.correlation_id.has_value()) {
    error.correlation_id = request_id;
  }
}

static_assert(
    std::is_nothrow_move_constructible_v<CheckpointErrorDetail>,
    "worker exception fallbacks must be movable without allocating memory");

[[nodiscard]] CheckpointErrorDetail
exception_error(std::string_view boundary, const std::exception *exception,
                const RequestId *request_id, bool allocation_failure,
                StorageSideEffect storage_side_effect,
                RuntimeSideEffect runtime_side_effect) {
  std::string diagnostic{boundary};
  if (allocation_failure) {
    diagnostic += ": memory allocation failed";
  } else if (exception != nullptr) {
    diagnostic += ": ";
    diagnostic += exception->what();
  } else {
    diagnostic += ": unknown exception";
  }
  return make_error(allocation_failure ? StableErrorCode::resource_exhausted
                                       : StableErrorCode::internal,
                    std::move(diagnostic), request_id, allocation_failure,
                    storage_side_effect, runtime_side_effect);
}

[[nodiscard]] CheckpointErrorDetail exception_error_or_fallback(
    std::string_view boundary, const std::exception *exception,
    const RequestId *request_id, bool allocation_failure,
    StorageSideEffect storage_side_effect,
    RuntimeSideEffect runtime_side_effect,
    CheckpointErrorDetail &fallback) noexcept {
  try {
    return exception_error(boundary, exception, request_id, allocation_failure,
                           storage_side_effect, runtime_side_effect);
  } catch (...) {
    return std::move(fallback);
  }
}

void log_service_failure(ILogger &logger, std::string_view message,
                         const CheckpointErrorDetail &error,
                         const ContainerId *container_id = nullptr) noexcept {
  const std::string_view container =
      container_id == nullptr ? std::string_view{} : container_id->value();
  const std::array fields{
      LogField{"container_id", container},
      LogField{"error_code", error_code_name(error.code)},
      LogField{"diagnostic", error.diagnostic},
  };
  logger.log(LogLevel::error, message, fields);
}

[[nodiscard]] CheckpointErrorDetail
validation_error(const ValidationError &error, const RequestId &request_id) {
  const auto code = error.code == ValidationErrorCode::too_many_registrations
                        ? StableErrorCode::resource_exhausted
                        : StableErrorCode::invalid_argument;
  return make_error(code, error.diagnostic, &request_id);
}

[[nodiscard]] CheckpointResult<IClock::MonotonicTime>
next_deadline(IClock::MonotonicTime now, std::chrono::seconds interval,
              const RequestId *request_id) {
  const auto remaining = IClock::MonotonicTime::max() - now;
  const auto remaining_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(remaining);
  if (interval > remaining_seconds) {
    return std::unexpected(make_error(
        StableErrorCode::resource_exhausted,
        "checkpoint interval cannot be represented by the monotonic clock",
        request_id));
  }
  return now +
         std::chrono::duration_cast<IClock::MonotonicTime::duration>(interval);
}

[[nodiscard]] bool cpu_assignments_overlap(const CheckpointConfig &left,
                                           const CheckpointConfig &right) {
  const bool container_sets_overlap =
      std::ranges::any_of(left.container_cpus.values(), [&](const auto cpu) {
        return right.container_cpus.contains(cpu);
      });
  return container_sets_overlap ||
         left.container_cpus.contains(right.checkpoint_cpu) ||
         right.container_cpus.contains(left.checkpoint_cpu) ||
         left.checkpoint_cpu == right.checkpoint_cpu;
}

} // namespace

CheckpointOperations::CheckpointOperations(
    CheckpointCreator &creator, RestoreCoordinator &restore_coordinator,
    ICheckpointMaintenance &maintenance, IPodmanAdapter &runtime) noexcept
    : creator_(creator), restore_coordinator_(restore_coordinator),
      maintenance_(maintenance), runtime_(runtime) {}

CheckpointResult<CreatedCheckpoint>
CheckpointOperations::create(const CheckpointCreateRequest &request) {
  return creator_.create(request);
}

CheckpointResult<RestoredContainer>
CheckpointOperations::restore_or_restart(const RestoreRequest &request) {
  return restore_coordinator_.restore_or_restart(request);
}

CheckpointResult<RuntimeObservation> CheckpointOperations::inspect_runtime(
    const ContainerRegistration &registration) {
  return runtime_.inspect_runtime(
      registration.container_id, registration.runtime_container,
      RuntimeCpuAssignment{
          .container_cpus = registration.checkpoint.container_cpus,
          .checkpoint_cpu = registration.checkpoint.checkpoint_cpu,
      });
}

CheckpointResult<void> CheckpointOperations::stop_runtime(
    const ContainerRegistration &registration,
    const std::optional<RuntimeIncarnation> &expected_incarnation) {
  auto target = expected_incarnation;
  if (!target.has_value()) {
    auto observation = inspect_runtime(registration);
    if (!observation) {
      return std::unexpected(observation.error());
    }
    if (observation->status.state == RuntimeState::absent) {
      return {};
    }
    if (!observation->incarnation.has_value()) {
      return std::unexpected(make_error(
          StableErrorCode::internal,
          "runtime observation did not provide a present incarnation"));
    }
    target = std::move(observation->incarnation);
  }
  return runtime_.stop_and_remove(
      registration.container_id, *target,
      RuntimeCpuAssignment{
          .container_cpus = registration.checkpoint.container_cpus,
          .checkpoint_cpu = registration.checkpoint.checkpoint_cpu,
      });
}

CheckpointResult<RetentionReport>
CheckpointOperations::apply_retention(const ContainerId &container_id,
                                      std::size_t keep_last) {
  return maintenance_.apply_retention(container_id, keep_last);
}

CheckpointResult<GarbageCollectionReport>
CheckpointOperations::collect_garbage(const ContainerId &container_id) {
  return maintenance_.collect_garbage(container_id);
}

struct SteadyTimer::Implementation {
  mutable std::mutex mutex;
  std::condition_variable_any condition;
  std::atomic<Generation> generation{0};
};

SteadyTimer::SteadyTimer()
    : implementation_(std::make_unique<Implementation>()) {}

SteadyTimer::~SteadyTimer() = default;

ITimer::Generation SteadyTimer::generation() const noexcept {
  return implementation_->generation.load(std::memory_order_acquire);
}

TimerWaitResult SteadyTimer::wait_until(IClock::MonotonicTime deadline,
                                        Generation observed_generation,
                                        std::stop_token stop_token) {
  std::unique_lock lock{implementation_->mutex};
  const bool interrupted =
      implementation_->condition.wait_until(lock, stop_token, deadline, [&] {
        return implementation_->generation.load(std::memory_order_acquire) !=
               observed_generation;
      });
  if (stop_token.stop_requested()) {
    return TimerWaitResult::stopped;
  }
  return interrupted ? TimerWaitResult::interrupted
                     : TimerWaitResult::deadline_reached;
}

void SteadyTimer::interrupt() noexcept {
  {
    const std::scoped_lock lock{implementation_->mutex};
    implementation_->generation.fetch_add(1U, std::memory_order_release);
  }
  implementation_->condition.notify_all();
}

struct CheckpointService::Implementation {
  enum class WorkKind : std::uint8_t {
    create,
    restore,
    status,
    stop,
    retention,
    garbage_collection,
  };

  struct WorkItem {
    WorkKind kind;
    bool periodic;
    std::uint64_t periodic_sequence;
    std::optional<RequestId> request_id;
    std::optional<OperationId> operation_id;
    std::shared_ptr<std::promise<CheckpointResult<void>>> completion;
    std::shared_ptr<std::promise<CheckpointResult<RuntimeStatus>>>
        status_completion;
    CheckpointErrorDetail exception_fallback;
  };

  struct Slot {
    Slot(ContainerRegistration value,
         IClock::MonotonicTime create_deadline_value,
         IClock::MonotonicTime garbage_collection_deadline_value)
        : registration(std::move(value)),
          next_create_deadline(create_deadline_value),
          next_garbage_collection_deadline(garbage_collection_deadline_value),
          worker_exception_fallback(make_error(
              StableErrorCode::resource_exhausted,
              "checkpoint worker exhausted memory while handling an exception",
              nullptr, true, StorageSideEffect::publication_may_be_visible,
              RuntimeSideEffect::incarnation_may_exist)),
          affinity_exception_fallback(make_error(
              StableErrorCode::resource_exhausted,
              "checkpoint worker exhausted memory while restoring affinity",
              nullptr, true)),
          retention_exception_fallback(make_error(
              StableErrorCode::resource_exhausted,
              "retention exhausted memory while handling an exception", nullptr,
              true, StorageSideEffect::publication_may_be_visible)),
          garbage_collection_exception_fallback(make_error(
              StableErrorCode::resource_exhausted,
              "garbage collection exhausted memory while handling an exception",
              nullptr, true, StorageSideEffect::publication_may_be_visible)) {}

    ContainerRegistration registration;
    IClock::MonotonicTime next_create_deadline;
    IClock::MonotonicTime next_garbage_collection_deadline;
    std::optional<IClock::MonotonicTime> retention_retry_deadline;
    std::optional<IClock::MonotonicTime> garbage_collection_retry_deadline;
    CheckpointErrorDetail worker_exception_fallback;
    CheckpointErrorDetail affinity_exception_fallback;
    CheckpointErrorDetail retention_exception_fallback;
    CheckpointErrorDetail garbage_collection_exception_fallback;
    std::mutex mutex;
    std::condition_variable condition;
    bool closing{false};
    bool running{false};
    std::optional<WorkKind> running_kind;
    bool startup_ready{false};
    std::optional<CheckpointErrorDetail> startup_error;
    std::optional<CheckpointErrorDetail> exit_error;
    std::optional<RuntimeIncarnation> incarnation;
    std::optional<WorkItem> foreground_pending;
    std::optional<WorkItem> periodic_create_pending;
    std::size_t foreground_waiters{0U};
    bool retention_pending{false};
    bool garbage_collection_pending{false};
    std::uint64_t next_periodic_sequence{1};
    std::thread::id worker_thread_id;
    std::jthread worker;
  };

  Implementation(ICheckpointOperations &operations_value, IClock &clock_value,
                 ITimer &timer_value, IIdGenerator &id_generator_value,
                 IAffinityController &affinity_controller_value,
                 ILogger &logger_value)
      : operations(operations_value), clock(clock_value), timer(timer_value),
        id_generator(id_generator_value),
        affinity_controller(affinity_controller_value), logger(logger_value),
        scheduler_exception_fallback(make_error(
            StableErrorCode::resource_exhausted,
            "periodic scheduler exhausted memory while handling an exception",
            nullptr, true)),
        scheduler([this](std::stop_token stop_token) {
          scheduler_loop(stop_token);
        }) {}

  ~Implementation() = default;

  ICheckpointOperations &operations;
  IClock &clock;
  ITimer &timer;
  IIdGenerator &id_generator;
  IAffinityController &affinity_controller;
  ILogger &logger;

  std::mutex lifecycle_mutex;
  std::mutex registry_mutex;
  std::map<std::string, std::shared_ptr<Slot>, std::less<>> registry;
  std::atomic<bool> stopping{false};
  bool shutdown_complete{false};
  std::optional<CheckpointErrorDetail> shutdown_error;
  CheckpointErrorDetail scheduler_exception_fallback;
  std::jthread scheduler;
  inline static thread_local Implementation *worker_context = nullptr;

  struct DueWorkUpdate {
    bool notify_worker{false};
    std::optional<CheckpointErrorDetail> scheduling_error;
  };

  [[nodiscard]] DueWorkUpdate
  promote_due_work_locked(Slot &slot, IClock::MonotonicTime now) {
    DueWorkUpdate update;
    if (slot.closing || stopping.load(std::memory_order_acquire)) {
      return update;
    }

    if (now >= slot.next_create_deadline) {
      auto next =
          next_deadline(now, slot.registration.checkpoint.interval, nullptr);
      if (next) {
        slot.next_create_deadline = *next;
      } else {
        slot.next_create_deadline = IClock::MonotonicTime::max();
        update.scheduling_error = std::move(next.error());
      }
      if (!slot.running && !slot.periodic_create_pending.has_value()) {
        slot.periodic_create_pending = WorkItem{
            .kind = WorkKind::create,
            .periodic = true,
            .periodic_sequence = slot.next_periodic_sequence++,
            .request_id = std::nullopt,
            .operation_id = std::nullopt,
            .completion = nullptr,
            .status_completion = nullptr,
            .exception_fallback =
                make_error(StableErrorCode::resource_exhausted,
                           "periodic checkpoint exhausted memory", nullptr,
                           true, StorageSideEffect::publication_may_be_visible),
        };
        update.notify_worker = true;
      }
    }

    if (now >= slot.next_garbage_collection_deadline) {
      auto next =
          next_deadline(now,
                        std::chrono::duration_cast<std::chrono::seconds>(
                            kGarbageCollectionInterval),
                        nullptr);
      if (next) {
        slot.next_garbage_collection_deadline = *next;
      } else {
        slot.next_garbage_collection_deadline = IClock::MonotonicTime::max();
        if (!update.scheduling_error.has_value()) {
          update.scheduling_error = std::move(next.error());
        }
      }
      if (!slot.garbage_collection_retry_deadline.has_value()) {
        slot.garbage_collection_pending = true;
        update.notify_worker = true;
      }
    }

    if (slot.retention_retry_deadline.has_value() &&
        now >= *slot.retention_retry_deadline) {
      slot.retention_retry_deadline.reset();
      slot.retention_pending = true;
      update.notify_worker = true;
    }
    if (slot.garbage_collection_retry_deadline.has_value() &&
        now >= *slot.garbage_collection_retry_deadline) {
      slot.garbage_collection_retry_deadline.reset();
      slot.garbage_collection_pending = true;
      update.notify_worker = true;
    }
    return update;
  }

  [[nodiscard]] CheckpointResult<void>
  register_container(const ContainerRegistration &registration,
                     const RequestId &request_id) {
    try {
      if (worker_context == this) {
        return std::unexpected(make_error(
            StableErrorCode::conflict,
            "a checkpoint worker cannot register through its own service",
            &request_id));
      }
      if (auto valid = validate_container_registration(registration); !valid) {
        return std::unexpected(validation_error(valid.error(), request_id));
      }

      const std::unique_lock lifecycle_lock{lifecycle_mutex};
      if (stopping.load(std::memory_order_acquire)) {
        return std::unexpected(make_error(
            StableErrorCode::unavailable,
            "checkpoint service is shutting down and accepts no registrations",
            &request_id));
      }

      {
        const std::scoped_lock registry_lock{registry_mutex};
        if (registry.contains(registration.container_id.value())) {
          return std::unexpected(make_error(StableErrorCode::already_exists,
                                            "container is already registered",
                                            &request_id));
        }
        if (registry.size() >= kMaximumRegistrations) {
          return std::unexpected(make_error(
              StableErrorCode::resource_exhausted,
              "all four checkpoint service registration slots are occupied",
              &request_id));
        }
        for (const auto &[unused_id, existing] : registry) {
          static_cast<void>(unused_id);
          if (cpu_assignments_overlap(registration.checkpoint,
                                      existing->registration.checkpoint)) {
            return std::unexpected(make_error(
                StableErrorCode::invalid_argument,
                "a logical CPU is already assigned to another registration",
                &request_id));
          }
        }
      }

      for (const auto cpu : registration.checkpoint.container_cpus.values()) {
        auto available = affinity_controller.validate_cpu_available(cpu);
        if (!available) {
          auto error = std::move(available.error());
          attach_correlation(error, request_id);
          return std::unexpected(std::move(error));
        }
      }
      if (auto available = affinity_controller.validate_cpu_available(
              registration.checkpoint.checkpoint_cpu);
          !available) {
        auto error = std::move(available.error());
        attach_correlation(error, request_id);
        return std::unexpected(std::move(error));
      }

      const auto now = clock.monotonic_now();
      auto create_deadline =
          next_deadline(now, registration.checkpoint.interval, &request_id);
      if (!create_deadline) {
        return std::unexpected(std::move(create_deadline.error()));
      }
      auto garbage_collection_deadline =
          next_deadline(now,
                        std::chrono::duration_cast<std::chrono::seconds>(
                            kGarbageCollectionInterval),
                        &request_id);
      if (!garbage_collection_deadline) {
        return std::unexpected(std::move(garbage_collection_deadline.error()));
      }

      auto slot =
          std::make_shared<Slot>(registration, std::move(*create_deadline),
                                 std::move(*garbage_collection_deadline));
      try {
        slot->worker =
            std::jthread([this, slot](std::stop_token) { worker_loop(slot); });
      } catch (const std::bad_alloc &) {
        return std::unexpected(
            make_error(StableErrorCode::resource_exhausted,
                       "could not allocate a checkpoint worker thread",
                       &request_id, true));
      } catch (const std::system_error &error) {
        return std::unexpected(
            make_error(StableErrorCode::resource_exhausted,
                       "could not start a checkpoint worker thread: " +
                           std::string{error.what()},
                       &request_id, true));
      }

      {
        std::unique_lock slot_lock{slot->mutex};
        slot->condition.wait(slot_lock, [&] { return slot->startup_ready; });
        if (slot->startup_error.has_value()) {
          auto error = std::move(*slot->startup_error);
          slot->closing = true;
          slot_lock.unlock();
          slot->condition.notify_all();
          slot->worker.join();
          attach_correlation(error, request_id);
          return std::unexpected(std::move(error));
        }
      }

      try {
        const std::scoped_lock registry_lock{registry_mutex};
        registry.emplace(registration.container_id.value(), slot);
      } catch (...) {
        {
          const std::scoped_lock slot_lock{slot->mutex};
          slot->closing = true;
        }
        slot->condition.notify_all();
        slot->worker.join();
        throw;
      }
      timer.interrupt();
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error(
          "register checkpoint container", nullptr, &request_id, true,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error(
          "register checkpoint container", &error, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (...) {
      return std::unexpected(exception_error(
          "register checkpoint container", nullptr, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    }
  }

  [[nodiscard]] CheckpointResult<void>
  unregister_container(const ContainerId &container_id,
                       const RequestId &request_id) {
    try {
      if (worker_context == this) {
        return std::unexpected(make_error(
            StableErrorCode::conflict,
            "a checkpoint worker cannot unregister through its own service",
            &request_id));
      }
      const std::unique_lock lifecycle_lock{lifecycle_mutex};
      if (stopping.load(std::memory_order_acquire)) {
        return std::unexpected(make_error(StableErrorCode::unavailable,
                                          "checkpoint service is shutting down",
                                          &request_id));
      }

      std::shared_ptr<Slot> slot;
      {
        const std::scoped_lock registry_lock{registry_mutex};
        const auto found = registry.find(container_id.value());
        if (found == registry.end()) {
          return std::unexpected(make_error(StableErrorCode::not_found,
                                            "container is not registered",
                                            &request_id));
        }
        slot = found->second;
      }

      {
        const std::scoped_lock slot_lock{slot->mutex};
        if (slot->worker_thread_id == std::this_thread::get_id()) {
          return std::unexpected(make_error(
              StableErrorCode::conflict,
              "a checkpoint worker cannot unregister its own container",
              &request_id));
        }
        if (slot->running || slot->foreground_pending.has_value()) {
          return std::unexpected(
              make_error(StableErrorCode::conflict,
                         "container has an active checkpoint operation",
                         &request_id, true));
        }
        slot->closing = true;
        slot->periodic_create_pending.reset();
        slot->retention_pending = false;
        slot->garbage_collection_pending = false;
        slot->retention_retry_deadline.reset();
        slot->garbage_collection_retry_deadline.reset();
      }
      slot->condition.notify_all();
      timer.interrupt();
      slot->worker.join();

      std::optional<CheckpointErrorDetail> exit_error;
      {
        const std::scoped_lock slot_lock{slot->mutex};
        exit_error = std::move(slot->exit_error);
      }
      {
        const std::scoped_lock registry_lock{registry_mutex};
        registry.erase(container_id.value());
      }
      if (exit_error.has_value()) {
        attach_correlation(*exit_error, request_id);
        return std::unexpected(std::move(*exit_error));
      }
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error(
          "unregister checkpoint container", nullptr, &request_id, true,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error(
          "unregister checkpoint container", &error, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (...) {
      return std::unexpected(exception_error(
          "unregister checkpoint container", nullptr, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    }
  }

  [[nodiscard]] CheckpointResult<void>
  run_manual(const ContainerId &container_id, const RequestId &request_id,
             const OperationId &operation_id, WorkKind kind) {
    try {
      if (worker_context == this) {
        return std::unexpected(make_error(
            StableErrorCode::conflict,
            "a checkpoint worker cannot synchronously call its own service",
            &request_id));
      }
      if (stopping.load(std::memory_order_acquire)) {
        return std::unexpected(make_error(
            StableErrorCode::unavailable,
            "checkpoint service is shutting down and accepts no operations",
            &request_id));
      }

      std::shared_ptr<Slot> slot;
      {
        const std::scoped_lock registry_lock{registry_mutex};
        const auto found = registry.find(container_id.value());
        if (found == registry.end()) {
          return std::unexpected(make_error(StableErrorCode::not_found,
                                            "container is not registered",
                                            &request_id));
        }
        slot = found->second;
      }

      auto completion =
          std::make_shared<std::promise<CheckpointResult<void>>>();
      auto future = completion->get_future();
      WorkItem item{
          .kind = kind,
          .periodic = false,
          .periodic_sequence = 0,
          .request_id = request_id,
          .operation_id = operation_id,
          .completion = completion,
          .status_completion = nullptr,
          .exception_fallback = make_error(
              StableErrorCode::resource_exhausted,
              "checkpoint operation exhausted memory", &request_id, true,
              kind == WorkKind::create
                  ? StorageSideEffect::publication_may_be_visible
                  : StorageSideEffect::none,
              kind == WorkKind::restore || kind == WorkKind::stop
                  ? RuntimeSideEffect::incarnation_may_exist
                  : RuntimeSideEffect::none),
      };
      {
        std::unique_lock slot_lock{slot->mutex};
        ++slot->foreground_waiters;
        try {
          slot->condition.wait(slot_lock, [&] {
            return slot->closing || stopping.load(std::memory_order_acquire) ||
                   (!slot->running && !slot->foreground_pending.has_value());
          });
        } catch (...) {
          --slot->foreground_waiters;
          slot_lock.unlock();
          slot->condition.notify_all();
          throw;
        }
        if (slot->closing || stopping.load(std::memory_order_acquire)) {
          --slot->foreground_waiters;
          slot_lock.unlock();
          slot->condition.notify_all();
          return std::unexpected(make_error(
              StableErrorCode::unavailable,
              "container registration is closing and accepts no operations",
              &request_id));
        }
        slot->foreground_pending = std::move(item);
        --slot->foreground_waiters;
      }
      slot->condition.notify_all();
      completion.reset();

      auto result = future.get();
      if (!result) {
        attach_correlation(result.error(), request_id);
      }
      return result;
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error(
          "dispatch checkpoint operation", nullptr, &request_id, true,
          kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          kind == WorkKind::restore || kind == WorkKind::stop
              ? RuntimeSideEffect::incarnation_may_exist
              : RuntimeSideEffect::none));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error(
          "dispatch checkpoint operation", &error, &request_id, false,
          kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          kind == WorkKind::restore || kind == WorkKind::stop
              ? RuntimeSideEffect::incarnation_may_exist
              : RuntimeSideEffect::none));
    } catch (...) {
      return std::unexpected(exception_error(
          "dispatch checkpoint operation", nullptr, &request_id, false,
          kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          kind == WorkKind::restore || kind == WorkKind::stop
              ? RuntimeSideEffect::incarnation_may_exist
              : RuntimeSideEffect::none));
    }
  }

  [[nodiscard]] CheckpointResult<RuntimeStatus>
  runtime_status(const ContainerId &container_id, const RequestId &request_id) {
    try {
      if (worker_context == this) {
        return std::unexpected(make_error(
            StableErrorCode::conflict,
            "a checkpoint worker cannot inspect its own runtime", &request_id));
      }
      if (stopping.load(std::memory_order_acquire)) {
        return std::unexpected(make_error(
            StableErrorCode::unavailable,
            "checkpoint service is shutting down and accepts no operations",
            &request_id));
      }

      std::shared_ptr<Slot> slot;
      {
        const std::scoped_lock registry_lock{registry_mutex};
        const auto found = registry.find(container_id.value());
        if (found == registry.end()) {
          return std::unexpected(make_error(StableErrorCode::not_found,
                                            "container is not registered",
                                            &request_id));
        }
        slot = found->second;
      }

      auto completion =
          std::make_shared<std::promise<CheckpointResult<RuntimeStatus>>>();
      auto future = completion->get_future();
      WorkItem item{
          .kind = WorkKind::status,
          .periodic = false,
          .periodic_sequence = 0,
          .request_id = request_id,
          .operation_id = std::nullopt,
          .completion = nullptr,
          .status_completion = completion,
          .exception_fallback = make_error(
              StableErrorCode::resource_exhausted,
              "runtime inspection exhausted memory", &request_id, true),
      };
      {
        std::unique_lock slot_lock{slot->mutex};
        ++slot->foreground_waiters;
        slot->condition.wait(slot_lock, [&] {
          return slot->closing || stopping.load(std::memory_order_acquire) ||
                 (!slot->running && !slot->foreground_pending.has_value());
        });
        if (slot->closing || stopping.load(std::memory_order_acquire)) {
          --slot->foreground_waiters;
          slot_lock.unlock();
          slot->condition.notify_all();
          return std::unexpected(make_error(
              StableErrorCode::unavailable,
              "container registration is closing and accepts no operations",
              &request_id));
        }
        slot->foreground_pending = std::move(item);
        --slot->foreground_waiters;
      }
      slot->condition.notify_all();
      completion.reset();

      auto result = future.get();
      if (!result) {
        attach_correlation(result.error(), request_id);
      }
      return result;
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error(
          "inspect container runtime", nullptr, &request_id, true,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error(
          "inspect container runtime", &error, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (...) {
      return std::unexpected(exception_error(
          "inspect container runtime", nullptr, &request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    }
  }

  [[nodiscard]] CheckpointResult<void> shutdown() {
    try {
      if (worker_context == this) {
        return std::unexpected(
            make_error(StableErrorCode::conflict,
                       "a checkpoint worker cannot shut down its own service"));
      }
      const std::unique_lock lifecycle_lock{lifecycle_mutex};
      if (shutdown_complete) {
        if (shutdown_error.has_value()) {
          return std::unexpected(*shutdown_error);
        }
        return {};
      }

      std::array<std::shared_ptr<Slot>, kMaximumRegistrations> slots;
      std::size_t slot_count = 0;
      {
        const std::scoped_lock registry_lock{registry_mutex};
        for (const auto &[unused_id, slot] : registry) {
          static_cast<void>(unused_id);
          const std::scoped_lock slot_lock{slot->mutex};
          if (slot->worker_thread_id == std::this_thread::get_id()) {
            return std::unexpected(make_error(
                StableErrorCode::conflict,
                "a checkpoint worker cannot shut down its own service"));
          }
          slots[slot_count++] = slot;
        }
      }

      stopping.store(true, std::memory_order_release);
      for (std::size_t index = 0; index < slot_count; ++index) {
        const auto &slot = slots[index];
        {
          const std::scoped_lock slot_lock{slot->mutex};
          slot->closing = true;
          slot->periodic_create_pending.reset();
          slot->retention_pending = false;
          slot->garbage_collection_pending = false;
          slot->retention_retry_deadline.reset();
          slot->garbage_collection_retry_deadline.reset();
        }
        slot->condition.notify_all();
      }

      scheduler.request_stop();
      timer.interrupt();
      scheduler.join();

      for (std::size_t index = 0; index < slot_count; ++index) {
        slots[index]->worker.join();
        const std::scoped_lock slot_lock{slots[index]->mutex};
        if (!shutdown_error.has_value() &&
            slots[index]->exit_error.has_value()) {
          shutdown_error = slots[index]->exit_error;
        }
      }
      {
        const std::scoped_lock registry_lock{registry_mutex};
        registry.clear();
      }
      shutdown_complete = true;
      if (shutdown_error.has_value()) {
        return std::unexpected(*shutdown_error);
      }
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error(
          "shut down checkpoint service", nullptr, nullptr, true,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error(
          "shut down checkpoint service", &error, nullptr, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    } catch (...) {
      return std::unexpected(exception_error(
          "shut down checkpoint service", nullptr, nullptr, false,
          StorageSideEffect::none, RuntimeSideEffect::none));
    }
  }

  void worker_loop(const std::shared_ptr<Slot> &slot) noexcept {
    struct WorkerContextGuard {
      explicit WorkerContextGuard(Implementation *owner_value) noexcept
          : previous(worker_context) {
        worker_context = owner_value;
      }

      ~WorkerContextGuard() { worker_context = previous; }

      Implementation *previous;
    };

    const WorkerContextGuard context_guard{this};
    std::unique_ptr<IThreadAffinityBinding> binding;
    try {
      {
        const std::scoped_lock slot_lock{slot->mutex};
        slot->worker_thread_id = std::this_thread::get_id();
      }

      auto bound = affinity_controller.bind_current_thread(
          slot->registration.checkpoint.checkpoint_cpu);
      if (!bound || *bound == nullptr) {
        auto error = !bound ? std::move(bound.error())
                            : make_error(StableErrorCode::internal,
                                         "affinity controller returned a null "
                                         "worker binding");
        const std::scoped_lock slot_lock{slot->mutex};
        slot->startup_error = std::move(error);
        slot->startup_ready = true;
        slot->closing = true;
        slot->condition.notify_all();
        return;
      }
      binding = std::move(*bound);
      {
        const std::scoped_lock slot_lock{slot->mutex};
        slot->startup_ready = true;
      }
      slot->condition.notify_all();

      while (true) {
        std::optional<WorkItem> item;
        std::optional<WorkKind> kind;
        std::optional<CheckpointErrorDetail> scheduling_error;
        {
          std::unique_lock slot_lock{slot->mutex};
          slot->condition.wait(slot_lock, [&] {
            return slot->closing || slot->foreground_pending.has_value() ||
                   (slot->foreground_waiters == 0U &&
                    (slot->periodic_create_pending.has_value() ||
                     slot->retention_pending ||
                     slot->garbage_collection_pending));
          });

          auto promoted = promote_due_work_locked(*slot, clock.monotonic_now());
          scheduling_error = std::move(promoted.scheduling_error);

          if (slot->foreground_pending.has_value()) {
            item = std::move(*slot->foreground_pending);
            slot->foreground_pending.reset();
            kind = item->kind;
          } else if (slot->foreground_waiters == 0U &&
                     slot->periodic_create_pending.has_value()) {
            item = std::move(*slot->periodic_create_pending);
            slot->periodic_create_pending.reset();
            kind = WorkKind::create;
          } else if (slot->foreground_waiters == 0U &&
                     slot->retention_pending) {
            slot->retention_pending = false;
            kind = WorkKind::retention;
          } else if (slot->foreground_waiters == 0U &&
                     slot->garbage_collection_pending) {
            slot->garbage_collection_pending = false;
            kind = WorkKind::garbage_collection;
          } else if (slot->closing) {
            break;
          } else {
            continue;
          }

          slot->running = true;
          slot->running_kind = *kind;
        }
        slot->condition.notify_all();
        if (scheduling_error.has_value()) {
          log_service_failure(logger, "checkpoint scheduling failed",
                              *scheduling_error,
                              &slot->registration.container_id);
        }

        if (item.has_value()) {
          if (item->status_completion != nullptr) {
            auto result = execute_status_item(slot, *item);
            {
              const std::scoped_lock slot_lock{slot->mutex};
              slot->running = false;
              slot->running_kind.reset();
            }
            slot->condition.notify_all();
            try {
              item->status_completion->set_value(std::move(result));
            } catch (const std::future_error &error) {
              auto detail = make_error(StableErrorCode::internal,
                                       "deliver runtime status result: " +
                                           std::string{error.what()});
              log_service_failure(logger, "runtime status delivery failed",
                                  detail, &slot->registration.container_id);
            }
          } else {
            auto result = execute_item(slot, *item);
            {
              const std::scoped_lock slot_lock{slot->mutex};
              slot->running = false;
              slot->running_kind.reset();
            }
            slot->condition.notify_all();

            if (item->completion != nullptr) {
              try {
                item->completion->set_value(std::move(result));
              } catch (const std::future_error &error) {
                auto detail =
                    make_error(StableErrorCode::internal,
                               "deliver checkpoint operation result: " +
                                   std::string{error.what()});
                log_service_failure(logger,
                                    "checkpoint worker result delivery failed",
                                    detail, &slot->registration.container_id);
              }
            } else if (!result) {
              log_service_failure(logger, "periodic checkpoint failed",
                                  result.error(),
                                  &slot->registration.container_id);
            }
          }
        } else {
          execute_maintenance(slot, *kind);
          {
            const std::scoped_lock slot_lock{slot->mutex};
            slot->running = false;
            slot->running_kind.reset();
          }
          slot->condition.notify_all();
        }
      }
    } catch (const std::bad_alloc &) {
      auto error = exception_error_or_fallback(
          "checkpoint worker boundary", nullptr, nullptr, true,
          StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::incarnation_may_exist,
          slot->worker_exception_fallback);
      fail_worker(slot, std::move(error));
    } catch (const std::exception &exception) {
      auto error = exception_error_or_fallback(
          "checkpoint worker boundary", &exception, nullptr, false,
          StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::incarnation_may_exist,
          slot->worker_exception_fallback);
      fail_worker(slot, std::move(error));
    } catch (...) {
      auto error = exception_error_or_fallback(
          "checkpoint worker boundary", nullptr, nullptr, false,
          StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::incarnation_may_exist,
          slot->worker_exception_fallback);
      fail_worker(slot, std::move(error));
    }

    if (binding != nullptr) {
      try {
        auto restored = binding->restore();
        if (!restored) {
          const std::scoped_lock slot_lock{slot->mutex};
          if (!slot->exit_error.has_value()) {
            slot->exit_error = std::move(restored.error());
          }
        }
      } catch (const std::bad_alloc &) {
        auto error = exception_error_or_fallback(
            "restore checkpoint worker affinity", nullptr, nullptr, true,
            StorageSideEffect::none, RuntimeSideEffect::none,
            slot->affinity_exception_fallback);
        const std::scoped_lock slot_lock{slot->mutex};
        if (!slot->exit_error.has_value()) {
          slot->exit_error.emplace(std::move(error));
        }
      } catch (const std::exception &exception) {
        auto error = exception_error_or_fallback(
            "restore checkpoint worker affinity", &exception, nullptr, false,
            StorageSideEffect::none, RuntimeSideEffect::none,
            slot->affinity_exception_fallback);
        const std::scoped_lock slot_lock{slot->mutex};
        if (!slot->exit_error.has_value()) {
          slot->exit_error.emplace(std::move(error));
        }
      } catch (...) {
        auto error = exception_error_or_fallback(
            "restore checkpoint worker affinity", nullptr, nullptr, false,
            StorageSideEffect::none, RuntimeSideEffect::none,
            slot->affinity_exception_fallback);
        const std::scoped_lock slot_lock{slot->mutex};
        if (!slot->exit_error.has_value()) {
          slot->exit_error.emplace(std::move(error));
        }
      }
      binding.reset();
    }
  }

  [[nodiscard]] CheckpointResult<RuntimeStatus>
  execute_status_item(const std::shared_ptr<Slot> &slot, WorkItem &item) {
    try {
      auto observation = operations.inspect_runtime(slot->registration);
      if (!observation) {
        auto error = std::move(observation.error());
        attach_correlation(error, *item.request_id);
        return std::unexpected(std::move(error));
      }
      {
        const std::scoped_lock slot_lock{slot->mutex};
        slot->incarnation = observation->incarnation;
      }
      return observation->status;
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error_or_fallback(
          "runtime status worker", nullptr, &*item.request_id, true,
          StorageSideEffect::none, RuntimeSideEffect::none,
          item.exception_fallback));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error_or_fallback(
          "runtime status worker", &error, &*item.request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none,
          item.exception_fallback));
    } catch (...) {
      return std::unexpected(exception_error_or_fallback(
          "runtime status worker", nullptr, &*item.request_id, false,
          StorageSideEffect::none, RuntimeSideEffect::none,
          item.exception_fallback));
    }
  }

  [[nodiscard]] CheckpointResult<void>
  execute_item(const std::shared_ptr<Slot> &slot, WorkItem &item) {
    std::optional<RequestId> generated_request;
    std::optional<OperationId> generated_operation;
    const RequestId *request_id = nullptr;
    const OperationId *operation_id = nullptr;
    const auto worker_name =
        item.kind == WorkKind::create ? "create checkpoint worker"
        : item.kind == WorkKind::stop ? "stop container worker"
                                      : "restore checkpoint worker";
    const auto runtime_side_effect =
        item.kind == WorkKind::restore || item.kind == WorkKind::stop
            ? RuntimeSideEffect::incarnation_may_exist
            : RuntimeSideEffect::none;

    try {
      if (item.periodic) {
        auto request = RequestId::create(
            "periodic-" + slot->registration.container_id.value() + "-" +
            std::to_string(item.periodic_sequence));
        if (!request) {
          return std::unexpected(make_error(
              StableErrorCode::internal,
              "could not construct a periodic checkpoint request ID"));
        }
        generated_request = std::move(*request);
        request_id = &*generated_request;

        auto operation = id_generator.new_operation_id();
        if (!operation) {
          auto error = std::move(operation.error());
          attach_correlation(error, *request_id);
          return std::unexpected(std::move(error));
        }
        generated_operation = std::move(*operation);
        operation_id = &*generated_operation;
      } else {
        request_id = &*item.request_id;
        operation_id = &*item.operation_id;
      }

      if (item.kind == WorkKind::create) {
        auto created = operations.create(CheckpointCreateRequest{
            .request_id = *request_id,
            .operation_id = *operation_id,
            .container_id = slot->registration.container_id,
            .runtime_container = slot->registration.runtime_container,
            .cpu_assignment =
                RuntimeCpuAssignment{
                    .container_cpus =
                        slot->registration.checkpoint.container_cpus,
                    .checkpoint_cpu =
                        slot->registration.checkpoint.checkpoint_cpu,
                },
            .keep_last = slot->registration.checkpoint.keep_last,
        });
        if (!created) {
          auto error = std::move(created.error());
          attach_correlation(error, *request_id);
          return std::unexpected(std::move(error));
        }
        {
          const std::scoped_lock slot_lock{slot->mutex};
          slot->incarnation = std::move(created->source_incarnation);
          if (!slot->closing && !stopping.load(std::memory_order_acquire)) {
            if (!slot->garbage_collection_retry_deadline.has_value()) {
              slot->garbage_collection_pending = true;
            }
            if (!created->retention_complete &&
                !slot->retention_retry_deadline.has_value()) {
              slot->retention_pending = true;
            }
          }
        }
        slot->condition.notify_all();
        return {};
      }

      std::optional<RuntimeIncarnation> expected_incarnation;
      {
        const std::scoped_lock slot_lock{slot->mutex};
        expected_incarnation = std::move(slot->incarnation);
        slot->incarnation.reset();
      }
      if (item.kind == WorkKind::stop) {
        auto stopped =
            operations.stop_runtime(slot->registration, expected_incarnation);
        if (!stopped) {
          auto error = std::move(stopped.error());
          attach_correlation(error, *request_id);
          return std::unexpected(std::move(error));
        }
        return {};
      }

      auto restored = operations.restore_or_restart(RestoreRequest{
          .request_id = *request_id,
          .operation_id = *operation_id,
          .container_id = slot->registration.container_id,
          .runtime_container = slot->registration.runtime_container,
          .cpu_assignment =
              RuntimeCpuAssignment{
                  .container_cpus =
                      slot->registration.checkpoint.container_cpus,
                  .checkpoint_cpu =
                      slot->registration.checkpoint.checkpoint_cpu,
              },
          .expected_source_incarnation = std::move(expected_incarnation),
          .cold_start = slot->registration.cold_start,
      });
      if (!restored) {
        auto error = std::move(restored.error());
        attach_correlation(error, *request_id);
        return std::unexpected(std::move(error));
      }
      {
        const std::scoped_lock slot_lock{slot->mutex};
        slot->incarnation = std::move(restored->incarnation);
      }
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(exception_error_or_fallback(
          worker_name, nullptr, request_id, true,
          item.kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          runtime_side_effect, item.exception_fallback));
    } catch (const std::exception &error) {
      return std::unexpected(exception_error_or_fallback(
          worker_name, &error, request_id, false,
          item.kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          runtime_side_effect, item.exception_fallback));
    } catch (...) {
      return std::unexpected(exception_error_or_fallback(
          worker_name, nullptr, request_id, false,
          item.kind == WorkKind::create
              ? StorageSideEffect::publication_may_be_visible
              : StorageSideEffect::none,
          runtime_side_effect, item.exception_fallback));
    }
  }

  void schedule_maintenance_retry(const std::shared_ptr<Slot> &slot,
                                  WorkKind kind) noexcept {
    const auto now = clock.monotonic_now();
    const auto retry_interval =
        std::chrono::duration_cast<IClock::MonotonicTime::duration>(
            kMaintenanceRetryInterval);
    const auto deadline = IClock::MonotonicTime::max() - now < retry_interval
                              ? IClock::MonotonicTime::max()
                              : now + retry_interval;
    bool armed = false;
    {
      const std::scoped_lock slot_lock{slot->mutex};
      if (!slot->closing && !stopping.load(std::memory_order_acquire)) {
        if (kind == WorkKind::retention) {
          slot->retention_pending = false;
          slot->retention_retry_deadline = deadline;
        } else {
          slot->garbage_collection_pending = false;
          slot->garbage_collection_retry_deadline = deadline;
        }
        armed = true;
      }
    }
    if (armed) {
      timer.interrupt();
    }
  }

  void maintenance_failed(const std::shared_ptr<Slot> &slot, WorkKind kind,
                          const CheckpointErrorDetail &error) noexcept {
    log_service_failure(logger,
                        kind == WorkKind::retention
                            ? "checkpoint retention failed"
                            : "checkpoint garbage collection failed",
                        error, &slot->registration.container_id);
    if (error.retryable) {
      schedule_maintenance_retry(slot, kind);
    }
  }

  void execute_maintenance(const std::shared_ptr<Slot> &slot,
                           WorkKind kind) noexcept {
    try {
      if (kind == WorkKind::retention) {
        auto report =
            operations.apply_retention(slot->registration.container_id,
                                       slot->registration.checkpoint.keep_last);
        if (!report) {
          maintenance_failed(slot, kind, report.error());
          return;
        }
        if (!report->complete) {
          schedule_maintenance_retry(slot, kind);
          return;
        }
        {
          const std::scoped_lock slot_lock{slot->mutex};
          slot->retention_retry_deadline.reset();
        }
      } else {
        auto report =
            operations.collect_garbage(slot->registration.container_id);
        if (!report) {
          maintenance_failed(slot, kind, report.error());
          return;
        }
        {
          const std::scoped_lock slot_lock{slot->mutex};
          slot->garbage_collection_retry_deadline.reset();
          if (!slot->closing && !stopping.load(std::memory_order_acquire)) {
            slot->garbage_collection_pending =
                slot->garbage_collection_pending || report->more_work;
          }
        }
      }
      slot->condition.notify_all();
    } catch (const std::bad_alloc &) {
      auto error = exception_error_or_fallback(
          kind == WorkKind::retention ? "checkpoint retention worker"
                                      : "checkpoint garbage collection worker",
          nullptr, nullptr, true, StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::none,
          kind == WorkKind::retention
              ? slot->retention_exception_fallback
              : slot->garbage_collection_exception_fallback);
      error.retryable = true;
      maintenance_failed(slot, kind, error);
    } catch (const std::exception &exception) {
      auto error = exception_error_or_fallback(
          kind == WorkKind::retention ? "checkpoint retention worker"
                                      : "checkpoint garbage collection worker",
          &exception, nullptr, false,
          StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::none,
          kind == WorkKind::retention
              ? slot->retention_exception_fallback
              : slot->garbage_collection_exception_fallback);
      error.retryable = true;
      maintenance_failed(slot, kind, error);
    } catch (...) {
      auto error = exception_error_or_fallback(
          kind == WorkKind::retention ? "checkpoint retention worker"
                                      : "checkpoint garbage collection worker",
          nullptr, nullptr, false,
          StorageSideEffect::publication_may_be_visible,
          RuntimeSideEffect::none,
          kind == WorkKind::retention
              ? slot->retention_exception_fallback
              : slot->garbage_collection_exception_fallback);
      error.retryable = true;
      maintenance_failed(slot, kind, error);
    }
  }

  void fail_worker(const std::shared_ptr<Slot> &slot,
                   CheckpointErrorDetail error) noexcept {
    log_service_failure(logger, "checkpoint worker stopped unexpectedly", error,
                        &slot->registration.container_id);
    {
      const std::scoped_lock slot_lock{slot->mutex};
      slot->closing = true;
      slot->running = false;
      if (!slot->startup_ready) {
        slot->startup_error.emplace(std::move(error));
        slot->startup_ready = true;
      } else {
        slot->exit_error.emplace(std::move(error));
      }
      slot->foreground_pending.reset();
      slot->periodic_create_pending.reset();
      slot->retention_pending = false;
      slot->garbage_collection_pending = false;
      slot->retention_retry_deadline.reset();
      slot->garbage_collection_retry_deadline.reset();
    }
    slot->condition.notify_all();
  }

  void scheduler_loop(std::stop_token stop_token) noexcept {
    try {
      while (!stop_token.stop_requested()) {
        const auto observed_generation = timer.generation();
        const auto now = clock.monotonic_now();
        auto earliest = IClock::MonotonicTime::max();
        std::array<std::shared_ptr<Slot>, kMaximumRegistrations> slots;
        std::size_t slot_count = 0;
        {
          const std::scoped_lock registry_lock{registry_mutex};
          for (const auto &[unused_id, slot] : registry) {
            static_cast<void>(unused_id);
            slots[slot_count++] = slot;
          }
        }

        for (std::size_t index = 0; index < slot_count; ++index) {
          const auto &slot = slots[index];
          bool notify_worker = false;
          std::optional<CheckpointErrorDetail> scheduling_error;
          {
            const std::scoped_lock slot_lock{slot->mutex};
            if (slot->closing || stopping.load(std::memory_order_acquire) ||
                stop_token.stop_requested()) {
              continue;
            }

            auto promoted = promote_due_work_locked(*slot, now);
            notify_worker = promoted.notify_worker;
            scheduling_error = std::move(promoted.scheduling_error);

            earliest = std::min(earliest, slot->next_create_deadline);
            earliest =
                std::min(earliest, slot->next_garbage_collection_deadline);
            if (slot->retention_retry_deadline.has_value()) {
              earliest = std::min(earliest, *slot->retention_retry_deadline);
            }
            if (slot->garbage_collection_retry_deadline.has_value()) {
              earliest =
                  std::min(earliest, *slot->garbage_collection_retry_deadline);
            }
          }
          if (notify_worker) {
            slot->condition.notify_all();
          }
          if (scheduling_error.has_value()) {
            log_service_failure(logger, "checkpoint scheduling failed",
                                *scheduling_error,
                                &slot->registration.container_id);
          }
        }

        if (stop_token.stop_requested()) {
          break;
        }
        static_cast<void>(
            timer.wait_until(earliest, observed_generation, stop_token));
      }
    } catch (const std::bad_alloc &) {
      const auto error = exception_error_or_fallback(
          "periodic scheduler boundary", nullptr, nullptr, true,
          StorageSideEffect::none, RuntimeSideEffect::none,
          scheduler_exception_fallback);
      log_service_failure(logger, "periodic scheduler stopped unexpectedly",
                          error);
    } catch (const std::exception &exception) {
      const auto error = exception_error_or_fallback(
          "periodic scheduler boundary", &exception, nullptr, false,
          StorageSideEffect::none, RuntimeSideEffect::none,
          scheduler_exception_fallback);
      log_service_failure(logger, "periodic scheduler stopped unexpectedly",
                          error);
    } catch (...) {
      const auto error = exception_error_or_fallback(
          "periodic scheduler boundary", nullptr, nullptr, false,
          StorageSideEffect::none, RuntimeSideEffect::none,
          scheduler_exception_fallback);
      log_service_failure(logger, "periodic scheduler stopped unexpectedly",
                          error);
    }
  }
};

CheckpointService::CheckpointService(ICheckpointOperations &operations,
                                     IClock &clock, ITimer &timer,
                                     IIdGenerator &id_generator,
                                     IAffinityController &affinity_controller,
                                     ILogger &logger)
    : implementation_(std::make_unique<Implementation>(
          operations, clock, timer, id_generator, affinity_controller,
          logger)) {}

CheckpointService::~CheckpointService() {
  static_cast<void>(implementation_->shutdown());
}

CheckpointResult<void>
CheckpointService::register_container(const ContainerRegistration &registration,
                                      const RequestId &request_id) {
  return implementation_->register_container(registration, request_id);
}

CheckpointResult<void>
CheckpointService::unregister_container(const ContainerId &container_id,
                                        const RequestId &request_id) {
  return implementation_->unregister_container(container_id, request_id);
}

CheckpointResult<void>
CheckpointService::create_checkpoint(const ContainerId &container_id,
                                     const RequestId &request_id,
                                     const OperationId &operation_id) {
  return implementation_->run_manual(container_id, request_id, operation_id,
                                     Implementation::WorkKind::create);
}

CheckpointResult<void>
CheckpointService::restore_or_restart(const ContainerId &container_id,
                                      const RequestId &request_id,
                                      const OperationId &operation_id) {
  return implementation_->run_manual(container_id, request_id, operation_id,
                                     Implementation::WorkKind::restore);
}

CheckpointResult<RuntimeStatus>
CheckpointService::get_runtime_status(const ContainerId &container_id,
                                      const RequestId &request_id) {
  return implementation_->runtime_status(container_id, request_id);
}

CheckpointResult<void>
CheckpointService::stop_container(const ContainerId &container_id,
                                  const RequestId &request_id,
                                  const OperationId &operation_id) {
  return implementation_->run_manual(container_id, request_id, operation_id,
                                     Implementation::WorkKind::stop);
}

CheckpointResult<void> CheckpointService::shutdown() {
  return implementation_->shutdown();
}

} // namespace gravesentinel::ccm
