#include "src/hypervisor/posix_process_runner.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

using MonotonicClock = std::chrono::steady_clock;

constexpr int maximum_supported_cpu_count = 1 << 20;
constexpr auto maximum_poll_interval = std::chrono::milliseconds{20};
constexpr std::size_t read_buffer_size = 16U * 1024U;
constexpr std::size_t maximum_drain_bytes_per_iteration = 64U * 1024U;
constexpr int child_exec_error_descriptor = 3;

class FileDescriptor {
public:
  FileDescriptor() noexcept = default;
  explicit FileDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}

  ~FileDescriptor() { reset(); }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  FileDescriptor(FileDescriptor &&other) noexcept
      : descriptor_(other.release()) {}

  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

  [[nodiscard]] int release() noexcept {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
  }

  void reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = descriptor;
  }

private:
  int descriptor_{-1};
};

struct Pipe {
  FileDescriptor read_end;
  FileDescriptor write_end;
};

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

enum class ChildFailureStage : std::uint8_t {
  process_group,
  affinity,
  working_directory,
  standard_output,
  standard_error,
  file_descriptors,
  execute,
};

struct ChildFailure {
  ChildFailureStage stage;
  int error_number;
};

static_assert(std::is_trivially_copyable_v<ChildFailure>);
static_assert(sizeof(ChildFailure) <= PIPE_BUF);

struct PreparedInvocation {
  std::string executable;
  std::vector<std::string> arguments;
  std::vector<char *> argument_pointers;
  std::vector<std::string> environment;
  std::vector<char *> environment_pointers;
  std::optional<std::string> working_directory;
  std::optional<DynamicCpuSet> affinity;
};

struct CaptureState {
  std::string bytes;
  std::size_t limit;
  bool truncated{false};
};

struct ExecErrorState {
  ChildFailure failure{};
  std::size_t bytes_read{0};
  bool complete{false};
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
  case ENOENT:
  case ENOTDIR:
    return StableErrorCode::not_found;
  case E2BIG:
  case EINVAL:
  case ENAMETOOLONG:
  case ENOEXEC:
    return StableErrorCode::invalid_argument;
  case EAGAIN:
  case EMFILE:
  case ENFILE:
  case ENOMEM:
    return StableErrorCode::resource_exhausted;
  case EBUSY:
  case ETXTBSY:
    return StableErrorCode::unavailable;
  default:
    return StableErrorCode::internal;
  }
}

[[nodiscard]] bool system_error_retryable(int error_number) noexcept {
  return error_number == EAGAIN || error_number == EBUSY ||
         error_number == EMFILE || error_number == ENFILE ||
         error_number == ENOMEM || error_number == ETXTBSY;
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

[[nodiscard]] bool contains_null(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] int
duplicate_above_standard_descriptors(int descriptor) noexcept {
  if (descriptor > STDERR_FILENO) {
    return descriptor;
  }

  int duplicated_descriptor = -1;
  do {
    duplicated_descriptor =
        ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  } while (duplicated_descriptor < 0 && errno == EINTR);
  return duplicated_descriptor;
}

[[nodiscard]] std::expected<Pipe, int> create_pipe() noexcept {
  int descriptors[2] = {-1, -1};
  int pipe_result = -1;
  do {
    pipe_result = ::pipe2(descriptors, O_CLOEXEC);
  } while (pipe_result != 0 && errno == EINTR);
  if (pipe_result != 0) {
    return std::unexpected(errno);
  }

  for (std::size_t index = 0; index < 2; ++index) {
    if (descriptors[index] <= STDERR_FILENO) {
      const int duplicated_descriptor =
          duplicate_above_standard_descriptors(descriptors[index]);
      if (duplicated_descriptor < 0) {
        const int duplicate_error = errno;
        static_cast<void>(::close(descriptors[0]));
        static_cast<void>(::close(descriptors[1]));
        return std::unexpected(duplicate_error);
      }
      static_cast<void>(::close(descriptors[index]));
      descriptors[index] = duplicated_descriptor;
    }
  }

  return Pipe{
      .read_end = FileDescriptor{descriptors[0]},
      .write_end = FileDescriptor{descriptors[1]},
  };
}

[[nodiscard]] int set_nonblocking(int descriptor) noexcept {
  int flags = -1;
  do {
    flags = ::fcntl(descriptor, F_GETFL);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) {
    return errno;
  }

  int set_result = -1;
  do {
    set_result = ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
  } while (set_result != 0 && errno == EINTR);
  return set_result == 0 ? 0 : errno;
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
prepare_child_affinity(std::uint32_t logical_cpu) {
  auto count_result = initial_cpu_count(logical_cpu);
  if (!count_result) {
    return std::unexpected(std::move(count_result.error()));
  }

  std::size_t cpu_count = *count_result;
  while (true) {
    auto current_result = allocate_cpu_set(cpu_count);
    if (!current_result) {
      return std::unexpected(std::move(current_result.error()));
    }
    auto current = std::move(*current_result);

    if (::sched_getaffinity(0, current.size, current.set.get()) == 0) {
      if (CPU_ISSET_S(static_cast<std::size_t>(logical_cpu), current.size,
                      current.set.get()) == 0) {
        return std::unexpected(make_error(
            StableErrorCode::invalid_argument,
            "child logical CPU is offline or unavailable to the current "
            "thread"));
      }

      auto target_result = allocate_cpu_set(cpu_count);
      if (!target_result) {
        return std::unexpected(std::move(target_result.error()));
      }
      auto target = std::move(*target_result);
      CPU_SET_S(static_cast<std::size_t>(logical_cpu), target.size,
                target.set.get());
      return target;
    }

    const int affinity_error = errno;
    if (affinity_error != EINVAL ||
        cpu_count >= static_cast<std::size_t>(maximum_supported_cpu_count)) {
      return std::unexpected(
          make_system_error("read parent thread CPU affinity", affinity_error));
    }
    cpu_count *= 2U;
  }
}

[[nodiscard]] CheckpointResult<PreparedInvocation>
prepare_invocation(const ProcessRequest &request) {
  const std::string executable = request.executable.native();
  if (executable.empty()) {
    return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                      "process executable must not be empty"));
  }
  if (contains_null(executable)) {
    return std::unexpected(
        make_error(StableErrorCode::invalid_argument,
                   "process executable must not contain a null byte"));
  }
  if (request.timeout < std::chrono::milliseconds::zero()) {
    return std::unexpected(make_error(StableErrorCode::invalid_argument,
                                      "process timeout must not be negative"));
  }
  if (request.termination_grace_period < std::chrono::milliseconds::zero()) {
    return std::unexpected(
        make_error(StableErrorCode::invalid_argument,
                   "process termination grace period must not be negative"));
  }

  PreparedInvocation prepared;
  prepared.executable = executable;
  prepared.arguments.reserve(request.argv.size() + 1U);
  prepared.arguments.push_back(executable);
  for (const auto &argument : request.argv) {
    if (contains_null(argument)) {
      return std::unexpected(
          make_error(StableErrorCode::invalid_argument,
                     "process argument must not contain a null byte"));
    }
    prepared.arguments.push_back(argument);
  }

  prepared.argument_pointers.reserve(prepared.arguments.size() + 1U);
  for (auto &argument : prepared.arguments) {
    prepared.argument_pointers.push_back(argument.data());
  }
  prepared.argument_pointers.push_back(nullptr);

  prepared.environment.reserve(request.environment.size());
  for (const auto &variable : request.environment) {
    if (variable.name.empty() || contains_null(variable.name) ||
        variable.name.find('=') != std::string::npos ||
        contains_null(variable.value)) {
      return std::unexpected(make_error(
          StableErrorCode::invalid_argument,
          "environment variable names must be nonempty and contain neither "
          "'=' nor null bytes, and values must not contain null bytes"));
    }
    const std::size_t maximum_entry_size = std::string{}.max_size();
    if (variable.name.size() >= maximum_entry_size ||
        variable.value.size() >
            maximum_entry_size - variable.name.size() - 1U) {
      return std::unexpected(
          make_error(StableErrorCode::resource_exhausted,
                     "environment variable exceeds the supported string size"));
    }

    std::string entry;
    entry.reserve(variable.name.size() + 1U + variable.value.size());
    entry.append(variable.name);
    entry.push_back('=');
    entry.append(variable.value);
    prepared.environment.push_back(std::move(entry));
  }

  prepared.environment_pointers.reserve(prepared.environment.size() + 1U);
  for (auto &variable : prepared.environment) {
    prepared.environment_pointers.push_back(variable.data());
  }
  prepared.environment_pointers.push_back(nullptr);

  if (request.working_directory.has_value()) {
    auto directory = request.working_directory->native();
    if (directory.empty() || contains_null(directory)) {
      return std::unexpected(make_error(
          StableErrorCode::invalid_argument,
          "process working directory must be nonempty and contain no null "
          "bytes"));
    }
    prepared.working_directory = std::move(directory);
  }

  if (request.logical_cpu.has_value()) {
    auto affinity_result = prepare_child_affinity(*request.logical_cpu);
    if (!affinity_result) {
      return std::unexpected(std::move(affinity_result.error()));
    }
    prepared.affinity = std::move(*affinity_result);
  }

  return prepared;
}

[[noreturn]] void report_child_failure(int error_descriptor,
                                       ChildFailureStage stage,
                                       int error_number) noexcept {
  ChildFailure failure{};
  failure.stage = stage;
  failure.error_number = error_number;
  const auto *bytes = reinterpret_cast<const std::byte *>(&failure);
  std::size_t offset = 0;
  while (offset < sizeof(failure)) {
    const auto written =
        ::write(error_descriptor, bytes + offset, sizeof(failure) - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::_exit(127);
}

[[nodiscard]] int duplicate_child_descriptor(int source,
                                             int destination) noexcept {
  int result = -1;
  do {
    result = ::dup2(source, destination);
  } while (result < 0 && errno == EINTR);
  return result;
}

[[nodiscard]] int preserve_child_exec_error_descriptor(int source) noexcept {
  if (source == child_exec_error_descriptor) {
    return source;
  }

  int result = -1;
  do {
    result = ::dup3(source, child_exec_error_descriptor, O_CLOEXEC);
  } while (result < 0 && errno == EINTR);
  return result;
}

[[nodiscard]] int close_unrelated_child_descriptors() noexcept {
  long result = -1;
  do {
    result =
        ::syscall(SYS_close_range,
                  static_cast<unsigned int>(child_exec_error_descriptor + 1),
                  std::numeric_limits<unsigned int>::max(), 0U);
  } while (result < 0 && errno == EINTR);
  return result == 0 ? 0 : errno;
}

[[noreturn]] void execute_child(const PreparedInvocation &prepared,
                                const Pipe &standard_output,
                                const Pipe &standard_error,
                                const Pipe &exec_error) noexcept {
  int error_descriptor = exec_error.write_end.get();

  if (::setpgid(0, 0) != 0) {
    report_child_failure(error_descriptor, ChildFailureStage::process_group,
                         errno);
  }
  if (prepared.affinity.has_value() &&
      ::syscall(SYS_sched_setaffinity, static_cast<pid_t>(0),
                prepared.affinity->size, prepared.affinity->set.get()) != 0) {
    report_child_failure(error_descriptor, ChildFailureStage::affinity, errno);
  }
  if (prepared.working_directory.has_value() &&
      ::chdir(prepared.working_directory->c_str()) != 0) {
    report_child_failure(error_descriptor, ChildFailureStage::working_directory,
                         errno);
  }
  if (duplicate_child_descriptor(standard_output.write_end.get(),
                                 STDOUT_FILENO) < 0) {
    report_child_failure(error_descriptor, ChildFailureStage::standard_output,
                         errno);
  }
  if (duplicate_child_descriptor(standard_error.write_end.get(),
                                 STDERR_FILENO) < 0) {
    report_child_failure(error_descriptor, ChildFailureStage::standard_error,
                         errno);
  }

  const int preserved_error_descriptor =
      preserve_child_exec_error_descriptor(error_descriptor);
  if (preserved_error_descriptor < 0) {
    report_child_failure(error_descriptor, ChildFailureStage::file_descriptors,
                         errno);
  }
  error_descriptor = child_exec_error_descriptor;

  const int close_error = close_unrelated_child_descriptors();
  if (close_error != 0) {
    report_child_failure(error_descriptor, ChildFailureStage::file_descriptors,
                         close_error);
  }

  ::execve(prepared.executable.c_str(), prepared.argument_pointers.data(),
           prepared.environment_pointers.data());
  report_child_failure(error_descriptor, ChildFailureStage::execute, errno);
}

[[nodiscard]] int drain_capture(FileDescriptor &descriptor,
                                CaptureState &capture) noexcept {
  std::array<char, read_buffer_size> buffer{};
  std::size_t drained_bytes = 0;
  while (descriptor.valid() &&
         drained_bytes < maximum_drain_bytes_per_iteration) {
    const std::size_t read_size = std::min(
        buffer.size(), maximum_drain_bytes_per_iteration - drained_bytes);
    const auto count = ::read(descriptor.get(), buffer.data(), read_size);
    if (count > 0) {
      const auto byte_count = static_cast<std::size_t>(count);
      drained_bytes += byte_count;
      const std::size_t available = capture.limit - capture.bytes.size();
      const std::size_t retained = std::min(byte_count, available);
      if (retained > 0) {
        capture.bytes.append(buffer.data(), retained);
      }
      if (retained < byte_count) {
        capture.truncated = true;
      }
      continue;
    }
    if (count == 0) {
      descriptor.reset();
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return errno;
  }
  return 0;
}

[[nodiscard]] int drain_exec_error(FileDescriptor &descriptor,
                                   ExecErrorState &state) noexcept {
  while (descriptor.valid()) {
    std::array<std::byte, sizeof(ChildFailure)> overflow{};
    auto *const failure_bytes = reinterpret_cast<std::byte *>(&state.failure);
    void *destination = overflow.data();
    std::size_t capacity = overflow.size();
    if (state.bytes_read < sizeof(ChildFailure)) {
      destination = failure_bytes + state.bytes_read;
      capacity = sizeof(ChildFailure) - state.bytes_read;
    }

    const auto count = ::read(descriptor.get(), destination, capacity);
    if (count > 0) {
      const auto byte_count = static_cast<std::size_t>(count);
      if (state.bytes_read >= sizeof(ChildFailure)) {
        return EPROTO;
      }
      state.bytes_read += byte_count;
      if (state.bytes_read == sizeof(ChildFailure)) {
        state.complete = true;
      }
      continue;
    }
    if (count == 0) {
      descriptor.reset();
      if (state.bytes_read != 0 && !state.complete) {
        return EPROTO;
      }
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return errno;
  }
  return 0;
}

[[nodiscard]] int wait_for_child_nonblocking(pid_t child_pid, bool &reaped,
                                             int &status) noexcept {
  if (reaped) {
    return 0;
  }

  while (true) {
    const pid_t wait_result = ::waitpid(child_pid, &status, WNOHANG);
    if (wait_result == child_pid) {
      reaped = true;
      return 0;
    }
    if (wait_result == 0) {
      return 0;
    }
    if (wait_result < 0 && errno == EINTR) {
      continue;
    }
    return errno;
  }
}

void send_signal_best_effort(pid_t child_pid, int signal_number,
                             bool child_reaped) noexcept {
  int group_result = -1;
  do {
    group_result = ::kill(-child_pid, signal_number);
  } while (group_result != 0 && errno == EINTR);

  if (!child_reaped) {
    int child_result = -1;
    do {
      child_result = ::kill(child_pid, signal_number);
    } while (child_result != 0 && errno == EINTR);
  }
}

[[nodiscard]] int send_signal(pid_t child_pid, int signal_number,
                              bool child_reaped) noexcept {
  int group_result = -1;
  do {
    group_result = ::kill(-child_pid, signal_number);
  } while (group_result != 0 && errno == EINTR);
  if (group_result == 0) {
    return 0;
  }

  const int group_error = errno;
  if (group_error != ESRCH) {
    return group_error;
  }
  if (child_reaped) {
    return 0;
  }

  int child_result = -1;
  do {
    child_result = ::kill(child_pid, signal_number);
  } while (child_result != 0 && errno == EINTR);
  if (child_result == 0 || errno == ESRCH) {
    return 0;
  }
  return errno;
}

[[nodiscard]] int reap_child_blocking(pid_t child_pid) noexcept {
  int status = 0;
  while (true) {
    const pid_t wait_result = ::waitpid(child_pid, &status, 0);
    if (wait_result == child_pid) {
      return 0;
    }
    if (wait_result < 0 && errno == EINTR) {
      continue;
    }
    return errno;
  }
}

void force_cleanup(pid_t child_pid, bool child_reaped) noexcept {
  send_signal_best_effort(child_pid, SIGKILL, child_reaped);
  if (!child_reaped) {
    static_cast<void>(reap_child_blocking(child_pid));
  }
}

[[nodiscard]] MonotonicClock::time_point
deadline_after(MonotonicClock::time_point now,
               std::chrono::milliseconds delay) noexcept {
  const auto maximum_delay =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          MonotonicClock::time_point::max() - now);
  if (delay >= maximum_delay) {
    return MonotonicClock::time_point::max();
  }
  return now + delay;
}

[[nodiscard]] int poll_timeout(MonotonicClock::time_point now,
                               MonotonicClock::time_point deadline) noexcept {
  if (deadline <= now) {
    return 0;
  }
  const auto remaining = deadline - now;
  if (remaining >= maximum_poll_interval) {
    return static_cast<int>(maximum_poll_interval.count());
  }
  const auto rounded =
      std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
  return static_cast<int>(rounded);
}

[[nodiscard]] const char *
child_failure_operation(ChildFailureStage stage) noexcept {
  switch (stage) {
  case ChildFailureStage::process_group:
    return "create child process group";
  case ChildFailureStage::affinity:
    return "set child CPU affinity";
  case ChildFailureStage::working_directory:
    return "change child working directory";
  case ChildFailureStage::standard_output:
    return "redirect child standard output";
  case ChildFailureStage::standard_error:
    return "redirect child standard error";
  case ChildFailureStage::file_descriptors:
    return "isolate child file descriptors";
  case ChildFailureStage::execute:
    return "execute child process";
  }
  return "prepare child process";
}

[[nodiscard]] bool valid_child_failure_stage(ChildFailureStage stage) noexcept {
  switch (stage) {
  case ChildFailureStage::process_group:
  case ChildFailureStage::affinity:
  case ChildFailureStage::working_directory:
  case ChildFailureStage::standard_output:
  case ChildFailureStage::standard_error:
  case ChildFailureStage::file_descriptors:
  case ChildFailureStage::execute:
    return true;
  }
  return false;
}

[[nodiscard]] int set_child_process_group(pid_t child_pid) noexcept {
  int result = -1;
  do {
    result = ::setpgid(child_pid, child_pid);
  } while (result != 0 && errno == EINTR);
  return result == 0 ? 0 : errno;
}

[[nodiscard]] pid_t fork_process() noexcept {
  pid_t child_pid = -1;
  do {
    child_pid = ::fork();
  } while (child_pid < 0 && errno == EINTR);
  return child_pid;
}

} // namespace

CheckpointResult<ProcessResult>
PosixProcessRunner::run(const ProcessRequest &request) {
  PreparedInvocation prepared;
  CaptureState standard_output{
      .bytes = {},
      .limit = request.maximum_standard_output_bytes,
      .truncated = false,
  };
  CaptureState standard_error{
      .bytes = {},
      .limit = request.maximum_standard_error_bytes,
      .truncated = false,
  };

  try {
    auto prepared_result = prepare_invocation(request);
    if (!prepared_result) {
      return std::unexpected(std::move(prepared_result.error()));
    }
    prepared = std::move(*prepared_result);
    standard_output.bytes.reserve(standard_output.limit);
    standard_error.bytes.reserve(standard_error.limit);
  } catch (const std::length_error &) {
    return std::unexpected(
        make_error(StableErrorCode::invalid_argument,
                   "process request exceeds the supported container size"));
  } catch (const std::bad_alloc &) {
    return std::unexpected(
        make_error(StableErrorCode::resource_exhausted,
                   "allocate process invocation or output buffers", true));
  }

  Pipe output_pipe;
  Pipe error_pipe;
  Pipe exec_pipe;

  auto output_pipe_result = create_pipe();
  if (!output_pipe_result) {
    return std::unexpected(make_system_error("create standard output pipe",
                                             output_pipe_result.error()));
  }
  output_pipe = std::move(*output_pipe_result);

  auto error_pipe_result = create_pipe();
  if (!error_pipe_result) {
    return std::unexpected(make_system_error("create standard error pipe",
                                             error_pipe_result.error()));
  }
  error_pipe = std::move(*error_pipe_result);

  auto exec_pipe_result = create_pipe();
  if (!exec_pipe_result) {
    return std::unexpected(make_system_error("create child exec-error pipe",
                                             exec_pipe_result.error()));
  }
  exec_pipe = std::move(*exec_pipe_result);

  const int output_nonblocking_error =
      set_nonblocking(output_pipe.read_end.get());
  if (output_nonblocking_error != 0) {
    return std::unexpected(make_system_error(
        "make standard output pipe nonblocking", output_nonblocking_error));
  }
  const int error_nonblocking_error =
      set_nonblocking(error_pipe.read_end.get());
  if (error_nonblocking_error != 0) {
    return std::unexpected(make_system_error(
        "make standard error pipe nonblocking", error_nonblocking_error));
  }
  const int exec_nonblocking_error = set_nonblocking(exec_pipe.read_end.get());
  if (exec_nonblocking_error != 0) {
    return std::unexpected(make_system_error(
        "make child exec-error pipe nonblocking", exec_nonblocking_error));
  }

  const pid_t child_pid = fork_process();
  if (child_pid < 0) {
    return std::unexpected(make_system_error("fork child process", errno));
  }
  if (child_pid == 0) {
    execute_child(prepared, output_pipe, error_pipe, exec_pipe);
  }

  output_pipe.write_end.reset();
  error_pipe.write_end.reset();
  exec_pipe.write_end.reset();

  const int process_group_error = set_child_process_group(child_pid);
  if (process_group_error != 0 && process_group_error != EACCES &&
      process_group_error != ESRCH) {
    force_cleanup(child_pid, false);
    return std::unexpected(
        make_system_error("confirm child process group", process_group_error));
  }

  bool child_reaped = false;
  int child_status = 0;
  bool timed_out = false;
  bool kill_sent = false;
  ExecErrorState exec_error;
  const auto timeout_deadline =
      deadline_after(MonotonicClock::now(), request.timeout);
  auto grace_deadline = MonotonicClock::time_point::max();

  while (true) {
    int io_error = drain_capture(output_pipe.read_end, standard_output);
    if (io_error == 0) {
      io_error = drain_capture(error_pipe.read_end, standard_error);
    }
    if (io_error == 0) {
      io_error = drain_exec_error(exec_pipe.read_end, exec_error);
    }
    if (io_error != 0) {
      force_cleanup(child_pid, child_reaped);
      return std::unexpected(
          make_system_error("read child process pipe", io_error));
    }

    if (!timed_out || kill_sent) {
      const int wait_error =
          wait_for_child_nonblocking(child_pid, child_reaped, child_status);
      if (wait_error != 0) {
        force_cleanup(child_pid, child_reaped);
        return std::unexpected(
            make_system_error("reap child process", wait_error));
      }
    }

    if (kill_sent && child_reaped) {
      output_pipe.read_end.reset();
      error_pipe.read_end.reset();
      exec_pipe.read_end.reset();
    }

    if (child_reaped && (!timed_out || kill_sent) &&
        !output_pipe.read_end.valid() && !error_pipe.read_end.valid() &&
        !exec_pipe.read_end.valid()) {
      break;
    }

    const auto now = MonotonicClock::now();
    if (!timed_out && now >= timeout_deadline) {
      timed_out = true;
      grace_deadline = deadline_after(now, request.termination_grace_period);
      const int signal_error = send_signal(child_pid, SIGTERM, child_reaped);
      if (signal_error != 0) {
        force_cleanup(child_pid, child_reaped);
        return std::unexpected(
            make_system_error("terminate child process group", signal_error));
      }
    }
    if (timed_out && !kill_sent && now >= grace_deadline) {
      const int signal_error = send_signal(child_pid, SIGKILL, child_reaped);
      if (signal_error != 0) {
        force_cleanup(child_pid, child_reaped);
        return std::unexpected(
            make_system_error("kill child process group", signal_error));
      }
      kill_sent = true;
    }

    std::array<struct pollfd, 3> descriptors{};
    nfds_t descriptor_count = 0;
    const auto add_descriptor = [&descriptors,
                                 &descriptor_count](int descriptor) {
      descriptors[static_cast<std::size_t>(descriptor_count)] = pollfd{
          .fd = descriptor,
          .events = static_cast<short>(POLLIN | POLLHUP),
          .revents = 0,
      };
      ++descriptor_count;
    };
    if (output_pipe.read_end.valid()) {
      add_descriptor(output_pipe.read_end.get());
    }
    if (error_pipe.read_end.valid()) {
      add_descriptor(error_pipe.read_end.get());
    }
    if (exec_pipe.read_end.valid()) {
      add_descriptor(exec_pipe.read_end.get());
    }

    const auto next_deadline =
        kill_sent ? MonotonicClock::time_point::max()
                  : (timed_out ? grace_deadline : timeout_deadline);
    const int timeout = poll_timeout(now, next_deadline);
    const int poll_result =
        ::poll(descriptors.data(), descriptor_count, timeout);
    if (poll_result < 0 && errno == EINTR) {
      continue;
    }
    if (poll_result < 0) {
      const int poll_error = errno;
      force_cleanup(child_pid, child_reaped);
      return std::unexpected(
          make_system_error("poll child process pipes", poll_error));
    }

    for (nfds_t index = 0; index < descriptor_count; ++index) {
      if ((descriptors[static_cast<std::size_t>(index)].revents & POLLNVAL) !=
          0) {
        force_cleanup(child_pid, child_reaped);
        return std::unexpected(
            make_system_error("poll child process pipes", EBADF));
      }
    }
  }

  if (!timed_out && exec_error.bytes_read != 0) {
    if (!exec_error.complete || exec_error.failure.error_number <= 0 ||
        !valid_child_failure_stage(exec_error.failure.stage)) {
      return std::unexpected(
          make_error(StableErrorCode::internal,
                     "child returned a malformed pre-exec failure record"));
    }
    return std::unexpected(
        make_system_error(child_failure_operation(exec_error.failure.stage),
                          exec_error.failure.error_number));
  }

  ProcessResult result{
      .termination = ProcessTermination::exited,
      .exit_code = std::nullopt,
      .signal_number = std::nullopt,
      .standard_output = std::move(standard_output.bytes),
      .standard_error = std::move(standard_error.bytes),
      .standard_output_truncated = standard_output.truncated,
      .standard_error_truncated = standard_error.truncated,
  };

  if (timed_out) {
    result.termination = ProcessTermination::timed_out;
    return result;
  }
  if (WIFEXITED(child_status)) {
    result.termination = ProcessTermination::exited;
    result.exit_code = WEXITSTATUS(child_status);
    return result;
  }
  if (WIFSIGNALED(child_status)) {
    result.termination = ProcessTermination::signaled;
    result.signal_number = WTERMSIG(child_status);
    return result;
  }

  return std::unexpected(
      make_error(StableErrorCode::internal,
                 "child process ended with an unsupported wait status"));
}

} // namespace gravesentinel::ccm
