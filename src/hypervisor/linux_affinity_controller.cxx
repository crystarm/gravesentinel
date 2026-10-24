#include "src/hypervisor/linux_affinity_controller.hxx"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sched.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

constexpr int maximum_supported_cpu_count = 1 << 20;

struct CpuSetDeleter {
  void operator()(cpu_set_t *set) const noexcept {
    if (set != nullptr) {
      CPU_FREE(set);
    }
  }
};

using CpuSetPointer = std::unique_ptr<cpu_set_t, CpuSetDeleter>;

struct DynamicCpuSet {
  CpuSetPointer set;
  std::size_t size;
  std::size_t cpu_count;
};

[[nodiscard]] CheckpointErrorDetail make_error(StableErrorCode code,
                                               std::string diagnostic,
                                               bool retryable = false) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

[[nodiscard]] StableErrorCode system_error_code(int error_number) noexcept {
  switch (error_number) {
  case EACCES:
  case EPERM:
    return StableErrorCode::permission_denied;
  case EAGAIN:
  case ENOMEM:
    return StableErrorCode::resource_exhausted;
  case EINVAL:
    return StableErrorCode::invalid_argument;
  default:
    return StableErrorCode::unavailable;
  }
}

[[nodiscard]] bool system_error_retryable(int error_number) noexcept {
  return error_number == EAGAIN || error_number == ENOMEM;
}

[[nodiscard]] CheckpointErrorDetail make_system_error(std::string operation,
                                                      int error_number) {
  operation += ": ";
  operation += std::error_code{error_number, std::generic_category()}.message();
  operation += " (errno ";
  operation += std::to_string(error_number);
  operation += ')';
  return make_error(system_error_code(error_number), std::move(operation),
                    system_error_retryable(error_number));
}

[[nodiscard]] CheckpointResult<std::size_t>
initial_cpu_count(std::uint32_t logical_cpu) {
  if (logical_cpu >= static_cast<std::uint32_t>(maximum_supported_cpu_count)) {
    return std::unexpected(make_error(
        StableErrorCode::invalid_argument,
        "logical CPU exceeds the supported Linux affinity mask size"));
  }

  errno = 0;
  const long configured_cpu_count = ::sysconf(_SC_NPROCESSORS_CONF);
  if (configured_cpu_count < 1) {
    const int query_error = errno == 0 ? EIO : errno;
    return std::unexpected(
        make_system_error("query configured CPU count", query_error));
  }
  if (configured_cpu_count > maximum_supported_cpu_count) {
    return std::unexpected(make_error(
        StableErrorCode::resource_exhausted,
        "configured CPU count exceeds the supported affinity mask size"));
  }

  const auto required_cpu_count =
      std::max(static_cast<std::size_t>(logical_cpu) + 1U,
               static_cast<std::size_t>(configured_cpu_count));
  std::size_t cpu_count = static_cast<std::size_t>(CPU_SETSIZE);
  while (cpu_count < required_cpu_count) {
    cpu_count *= 2U;
  }
  return cpu_count;
}

[[nodiscard]] CheckpointResult<DynamicCpuSet>
allocate_cpu_set(std::size_t cpu_count) {
  cpu_set_t *const raw_set = CPU_ALLOC(cpu_count);
  if (raw_set == nullptr) {
    return std::unexpected(make_error(StableErrorCode::resource_exhausted,
                                      "allocate Linux CPU affinity mask",
                                      true));
  }

  const std::size_t set_size = CPU_ALLOC_SIZE(cpu_count);
  CPU_ZERO_S(set_size, raw_set);
  return DynamicCpuSet{
      .set = CpuSetPointer{raw_set},
      .size = set_size,
      .cpu_count = cpu_count,
  };
}

[[nodiscard]] CheckpointResult<DynamicCpuSet>
read_affinity(std::uint32_t logical_cpu, pid_t thread_id,
              std::string_view operation) {
  auto cpu_count_result = initial_cpu_count(logical_cpu);
  if (!cpu_count_result) {
    return std::unexpected(std::move(cpu_count_result.error()));
  }

  std::size_t cpu_count = *cpu_count_result;
  while (true) {
    auto mask_result = allocate_cpu_set(cpu_count);
    if (!mask_result) {
      return std::unexpected(std::move(mask_result.error()));
    }

    auto mask = std::move(*mask_result);
    if (::sched_getaffinity(thread_id, mask.size, mask.set.get()) == 0) {
      return mask;
    }

    const int affinity_error = errno;
    if (affinity_error != EINVAL ||
        cpu_count >= static_cast<std::size_t>(maximum_supported_cpu_count)) {
      return std::unexpected(
          make_system_error(std::string(operation), affinity_error));
    }
    cpu_count *= 2U;
  }
}

[[nodiscard]] CheckpointResult<DynamicCpuSet>
single_cpu_affinity(std::uint32_t logical_cpu, std::size_t cpu_count) {
  auto mask_result = allocate_cpu_set(cpu_count);
  if (!mask_result) {
    return std::unexpected(std::move(mask_result.error()));
  }

  auto mask = std::move(*mask_result);
  CPU_SET_S(static_cast<std::size_t>(logical_cpu), mask.size, mask.set.get());
  return mask;
}

[[nodiscard]] pid_t current_thread_id() noexcept {
  return static_cast<pid_t>(::syscall(SYS_gettid));
}

class LinuxThreadAffinityBinding final : public IThreadAffinityBinding {
public:
  LinuxThreadAffinityBinding(pid_t owner_thread_id,
                             DynamicCpuSet original_affinity) noexcept
      : owner_thread_id_(owner_thread_id),
        original_affinity_(std::move(original_affinity)) {}

  ~LinuxThreadAffinityBinding() override {
    if (active_ && current_thread_id() == owner_thread_id_) {
      static_cast<void>(::sched_setaffinity(0, original_affinity_.size,
                                            original_affinity_.set.get()));
    }
  }

  LinuxThreadAffinityBinding(const LinuxThreadAffinityBinding &) = delete;
  LinuxThreadAffinityBinding &
  operator=(const LinuxThreadAffinityBinding &) = delete;
  LinuxThreadAffinityBinding(LinuxThreadAffinityBinding &&) = delete;
  LinuxThreadAffinityBinding &operator=(LinuxThreadAffinityBinding &&) = delete;

  [[nodiscard]] CheckpointResult<void> restore() override {
    if (!active_) {
      return {};
    }
    if (current_thread_id() != owner_thread_id_) {
      return std::unexpected(make_error(
          StableErrorCode::invalid_argument,
          "thread affinity binding must be restored by its owning thread"));
    }
    if (::sched_setaffinity(0, original_affinity_.size,
                            original_affinity_.set.get()) != 0) {
      return std::unexpected(
          make_system_error("restore current thread CPU affinity", errno));
    }
    active_ = false;
    return {};
  }

  void activate() noexcept { active_ = true; }

private:
  pid_t owner_thread_id_;
  DynamicCpuSet original_affinity_;
  bool active_{false};
};

[[nodiscard]] CheckpointResult<void>
validate_cpu_in_mask(std::uint32_t logical_cpu, const DynamicCpuSet &mask) {
  if (CPU_ISSET_S(static_cast<std::size_t>(logical_cpu), mask.size,
                  mask.set.get()) == 0) {
    return std::unexpected(make_error(
        StableErrorCode::invalid_argument,
        "logical CPU is offline or unavailable to the current thread"));
  }
  return {};
}

} // namespace

CheckpointResult<std::unique_ptr<IThreadAffinityBinding>>
LinuxAffinityController::bind_current_thread(std::uint32_t logical_cpu) {
  try {
    auto original_result =
        read_affinity(logical_cpu, 0, "read current thread CPU affinity");
    if (!original_result) {
      return std::unexpected(std::move(original_result.error()));
    }

    auto original_affinity = std::move(*original_result);
    auto validation = validate_cpu_in_mask(logical_cpu, original_affinity);
    if (!validation) {
      return std::unexpected(std::move(validation.error()));
    }

    auto target_result =
        single_cpu_affinity(logical_cpu, original_affinity.cpu_count);
    if (!target_result) {
      return std::unexpected(std::move(target_result.error()));
    }
    auto target_affinity = std::move(*target_result);

    auto binding = std::make_unique<LinuxThreadAffinityBinding>(
        current_thread_id(), std::move(original_affinity));
    if (::sched_setaffinity(0, target_affinity.size,
                            target_affinity.set.get()) != 0) {
      return std::unexpected(
          make_system_error("bind current thread to logical CPU", errno));
    }

    binding->activate();
    return std::unique_ptr<IThreadAffinityBinding>{std::move(binding)};
  } catch (const std::bad_alloc &) {
    return std::unexpected(make_error(StableErrorCode::resource_exhausted,
                                      "allocate thread affinity state", true));
  }
}

CheckpointResult<void> LinuxAffinityController::validate_cpu_available(
    std::uint32_t logical_cpu) const {
  try {
    auto affinity_result = read_affinity(logical_cpu, ::getpid(),
                                         "read process leader CPU affinity");
    if (!affinity_result) {
      return std::unexpected(std::move(affinity_result.error()));
    }
    return validate_cpu_in_mask(logical_cpu, *affinity_result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(make_error(StableErrorCode::resource_exhausted,
                                      "allocate thread affinity state", true));
  }
}

} // namespace gravesentinel::ccm
