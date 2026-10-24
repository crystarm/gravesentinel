#include "src/hypervisor/linux_affinity_controller.hxx"
#include "src/hypervisor/posix_process_runner.hxx"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace gravesentinel::ccm {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view helper_marker = "--linux-runtime-helper";
constexpr std::size_t helper_stream_size = 128U * 1024U;
constexpr std::size_t maximum_test_cpu_count = 1U << 20U;

[[nodiscard]] bool write_all(int descriptor, std::string_view bytes) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] std::string encode_argument(std::string_view prefix,
                                          std::string_view argument) {
  std::string encoded{prefix};
  encoded += std::to_string(argument.size());
  encoded.push_back(':');
  encoded.append(argument);
  encoded.push_back('\n');
  return encoded;
}

[[nodiscard]] int argument_helper(int argc, char **argv) {
  std::string output = encode_argument("argv0=", argv[0]);
  for (int index = 3; index < argc; ++index) {
    output += encode_argument("arg=", argv[index]);
  }
  return write_all(STDOUT_FILENO, output) ? 0 : 91;
}

[[nodiscard]] int environment_helper() {
  std::error_code directory_error;
  const auto directory = std::filesystem::current_path(directory_error);
  if (directory_error) {
    return 92;
  }

  std::string output = "cwd=" + directory.native() + "\n";
  for (char **entry = ::environ; *entry != nullptr; ++entry) {
    output += "env=";
    output += *entry;
    output.push_back('\n');
  }
  return write_all(STDOUT_FILENO, output) ? 0 : 93;
}

[[nodiscard]] int streams_helper() {
  const std::string standard_output(helper_stream_size, 'O');
  const std::string standard_error(helper_stream_size, 'E');
  if (!write_all(STDOUT_FILENO, standard_output)) {
    return 94;
  }
  return write_all(STDERR_FILENO, standard_error) ? 0 : 95;
}

[[nodiscard]] int timeout_group_helper() {
  if (::signal(SIGTERM, SIG_IGN) == SIG_ERR) {
    return 96;
  }

  const pid_t descendant_pid = ::fork();
  if (descendant_pid < 0) {
    return 97;
  }
  if (descendant_pid == 0) {
    while (true) {
      static_cast<void>(::pause());
    }
  }

  const std::string ready = std::to_string(descendant_pid) + "\n";
  if (!write_all(STDOUT_FILENO, ready)) {
    return 98;
  }
  while (true) {
    static_cast<void>(::pause());
  }
}

[[nodiscard]] int closed_stdio_descendant_helper() {
  if (::signal(SIGTERM, SIG_DFL) == SIG_ERR) {
    return 104;
  }

  int ready_pipe[2] = {-1, -1};
  if (::pipe2(ready_pipe, O_CLOEXEC) != 0) {
    return 105;
  }

  const pid_t descendant_pid = ::fork();
  if (descendant_pid < 0) {
    static_cast<void>(::close(ready_pipe[0]));
    static_cast<void>(::close(ready_pipe[1]));
    return 106;
  }
  if (descendant_pid == 0) {
    static_cast<void>(::close(ready_pipe[0]));
    if (::signal(SIGTERM, SIG_IGN) == SIG_ERR) {
      ::_exit(107);
    }
    static_cast<void>(::close(STDOUT_FILENO));
    static_cast<void>(::close(STDERR_FILENO));
    if (!write_all(ready_pipe[1], "r")) {
      ::_exit(108);
    }
    static_cast<void>(::close(ready_pipe[1]));
    while (true) {
      static_cast<void>(::pause());
    }
  }

  static_cast<void>(::close(ready_pipe[1]));
  char ready = '\0';
  ssize_t ready_count = -1;
  do {
    ready_count = ::read(ready_pipe[0], &ready, sizeof(ready));
  } while (ready_count < 0 && errno == EINTR);
  static_cast<void>(::close(ready_pipe[0]));
  if (ready_count != 1 || ready != 'r') {
    static_cast<void>(::kill(descendant_pid, SIGKILL));
    return 109;
  }

  const std::string output = std::to_string(descendant_pid) + "\n";
  if (!write_all(STDOUT_FILENO, output)) {
    static_cast<void>(::kill(descendant_pid, SIGKILL));
    return 110;
  }
  while (true) {
    static_cast<void>(::pause());
  }
}

[[nodiscard]] int descriptor_state_helper(int argc, char **argv) {
  if (argc < 4) {
    return 111;
  }
  int descriptor = -1;
  const std::string_view encoded_descriptor{argv[3]};
  const char *const end = encoded_descriptor.data() + encoded_descriptor.size();
  const auto conversion =
      std::from_chars(encoded_descriptor.data(), end, descriptor);
  if (conversion.ec != std::errc{} || conversion.ptr != end || descriptor < 3) {
    return 112;
  }

  errno = 0;
  if (::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF) {
    return write_all(STDOUT_FILENO, "closed\n") ? 0 : 113;
  }
  return write_all(STDOUT_FILENO, "inherited\n") ? 114 : 115;
}

[[nodiscard]] int affinity_helper() {
  const int logical_cpu = ::sched_getcpu();
  if (logical_cpu < 0) {
    return 99;
  }
  const std::string output = std::to_string(logical_cpu) + "\n";
  return write_all(STDOUT_FILENO, output) ? 0 : 100;
}

[[nodiscard]] int run_helper(int argc, char **argv) {
  if (argc < 3) {
    return 90;
  }

  const std::string_view mode{argv[2]};
  if (mode == "arguments") {
    return argument_helper(argc, argv);
  }
  if (mode == "environment") {
    return environment_helper();
  }
  if (mode == "streams") {
    return streams_helper();
  }
  if (mode == "exit") {
    return 37;
  }
  if (mode == "signal") {
    if (::signal(SIGUSR1, SIG_DFL) == SIG_ERR) {
      return 101;
    }
    return ::raise(SIGUSR1) == 0 ? 102 : 103;
  }
  if (mode == "timeout-group") {
    return timeout_group_helper();
  }
  if (mode == "closed-stdio-descendant") {
    return closed_stdio_descendant_helper();
  }
  if (mode == "descriptor-state") {
    return descriptor_state_helper(argc, argv);
  }
  if (mode == "affinity") {
    return affinity_helper();
  }
  return 89;
}

[[nodiscard]] std::filesystem::path self_executable() {
  std::error_code error;
  auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    throw std::system_error{error, "read /proc/self/exe"};
  }
  return executable;
}

[[nodiscard]] ProcessRequest helper_request(std::string mode) {
  ProcessRequest request{};
  request.executable = self_executable();
  request.argv = {std::string{helper_marker}, std::move(mode)};
  request.timeout = 2s;
  request.termination_grace_period = 100ms;
  return request;
}

class ScopedDescriptor {
public:
  explicit ScopedDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  ~ScopedDescriptor() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  ScopedDescriptor(const ScopedDescriptor &) = delete;
  ScopedDescriptor &operator=(const ScopedDescriptor &) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
  int descriptor_;
};

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto unique = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("gravesentinel-linux-runtime-" + std::to_string(::getpid()) + "-" +
             std::to_string(unique));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct TestCpuSetDeleter {
  void operator()(cpu_set_t *set) const noexcept {
    if (set != nullptr) {
      CPU_FREE(set);
    }
  }
};

struct TestCpuSet {
  std::unique_ptr<cpu_set_t, TestCpuSetDeleter> set;
  std::size_t size;
  std::size_t cpu_count;
};

[[nodiscard]] std::expected<TestCpuSet, int> read_current_affinity() {
  std::size_t cpu_count = static_cast<std::size_t>(CPU_SETSIZE);
  while (cpu_count <= maximum_test_cpu_count) {
    cpu_set_t *const raw_set = CPU_ALLOC(cpu_count);
    if (raw_set == nullptr) {
      return std::unexpected(ENOMEM);
    }
    TestCpuSet mask{
        .set = std::unique_ptr<cpu_set_t, TestCpuSetDeleter>{raw_set},
        .size = CPU_ALLOC_SIZE(cpu_count),
        .cpu_count = cpu_count,
    };
    CPU_ZERO_S(mask.size, mask.set.get());
    if (::sched_getaffinity(0, mask.size, mask.set.get()) == 0) {
      return mask;
    }
    const int affinity_error = errno;
    if (affinity_error != EINVAL || cpu_count == maximum_test_cpu_count) {
      return std::unexpected(affinity_error);
    }
    cpu_count *= 2U;
  }
  return std::unexpected(EOVERFLOW);
}

[[nodiscard]] bool equal_cpu_sets(const TestCpuSet &left,
                                  const TestCpuSet &right) noexcept {
  return left.size == right.size &&
         std::memcmp(left.set.get(), right.set.get(), left.size) == 0;
}

[[nodiscard]] std::optional<std::uint32_t>
first_allowed_cpu(const TestCpuSet &mask) noexcept {
  for (std::size_t cpu = 0; cpu < mask.cpu_count; ++cpu) {
    if (CPU_ISSET_S(cpu, mask.size, mask.set.get()) != 0) {
      return static_cast<std::uint32_t>(cpu);
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool contains_only_cpu(const TestCpuSet &mask,
                                     std::uint32_t expected_cpu) noexcept {
  for (std::size_t cpu = 0; cpu < mask.cpu_count; ++cpu) {
    const bool included = CPU_ISSET_S(cpu, mask.size, mask.set.get()) != 0;
    if (included != (cpu == static_cast<std::size_t>(expected_cpu))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<pid_t> parse_pid(std::string_view text) noexcept {
  pid_t parsed = 0;
  const char *const end = text.data() + text.size();
  const auto conversion = std::from_chars(text.data(), end, parsed);
  if (conversion.ec != std::errc{} || parsed <= 0 ||
      (conversion.ptr != end && *conversion.ptr != '\n')) {
    return std::nullopt;
  }
  return parsed;
}

class SubreaperScope {
public:
  SubreaperScope() {
    if (::prctl(PR_GET_CHILD_SUBREAPER, &previous_) != 0) {
      error_ = errno;
      return;
    }
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
      error_ = errno;
      return;
    }
    active_ = true;
  }

  ~SubreaperScope() {
    if (active_) {
      static_cast<void>(::prctl(PR_SET_CHILD_SUBREAPER, previous_));
    }
  }

  SubreaperScope(const SubreaperScope &) = delete;
  SubreaperScope &operator=(const SubreaperScope &) = delete;

  [[nodiscard]] int error() const noexcept { return error_; }

private:
  int previous_{0};
  int error_{0};
  bool active_{false};
};

[[nodiscard]] std::optional<int> reap_with_timeout(pid_t child_pid) noexcept {
  for (int attempt = 0; attempt < 200; ++attempt) {
    int status = 0;
    const pid_t result = ::waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      return status;
    }
    if (result < 0 && errno != EINTR) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(5ms);
  }
  static_cast<void>(::kill(child_pid, SIGKILL));
  int ignored_status = 0;
  while (::waitpid(child_pid, &ignored_status, 0) < 0 && errno == EINTR) {
  }
  return std::nullopt;
}

TEST(PosixProcessRunnerTest, PreservesArgumentsWithoutShellInterpretation) {
  PosixProcessRunner runner;
  auto request = helper_request("arguments");
  const std::vector<std::string> arguments{
      "plain",
      "two words",
      "; echo shell-injection",
      "$(printf command-substitution)",
      "wildcard-*?[abc]",
      "quote-'\"-and-$HOME",
  };
  request.argv.insert(request.argv.end(), arguments.begin(), arguments.end());

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  EXPECT_EQ(result->termination, ProcessTermination::exited);
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_FALSE(result->signal_number.has_value());

  std::string expected = encode_argument("argv0=", request.executable.native());
  for (const auto &argument : arguments) {
    expected += encode_argument("arg=", argument);
  }
  EXPECT_EQ(result->standard_output, expected);
  EXPECT_TRUE(result->standard_error.empty());
}

TEST(PosixProcessRunnerTest, UsesExactEnvironmentAndWorkingDirectory) {
  TemporaryDirectory directory;
  PosixProcessRunner runner;
  auto request = helper_request("environment");
  request.environment = {
      EnvironmentVariable{.name = "ONLY_ONE", .value = "value with spaces"},
      EnvironmentVariable{.name = "EMPTY", .value = ""},
  };
  request.working_directory = directory.path();

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  ASSERT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->standard_output,
            "cwd=" + directory.path().native() +
                "\nenv=ONLY_ONE=value with spaces\nenv=EMPTY=\n");
}

TEST(PosixProcessRunnerTest, CapturesAndTruncatesBothOutputStreams) {
  PosixProcessRunner runner;
  auto request = helper_request("streams");
  request.maximum_standard_output_bytes = 31;
  request.maximum_standard_error_bytes = 29;

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  ASSERT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->standard_output, std::string(31, 'O'));
  EXPECT_EQ(result->standard_error, std::string(29, 'E'));
  EXPECT_TRUE(result->standard_output_truncated);
  EXPECT_TRUE(result->standard_error_truncated);
}

TEST(PosixProcessRunnerTest, DistinguishesExitAndSignalTermination) {
  PosixProcessRunner runner;

  const auto exited = runner.run(helper_request("exit"));
  ASSERT_TRUE(exited) << exited.error().diagnostic;
  EXPECT_EQ(exited->termination, ProcessTermination::exited);
  EXPECT_EQ(exited->exit_code, 37);
  EXPECT_FALSE(exited->signal_number.has_value());

  const auto signaled = runner.run(helper_request("signal"));
  ASSERT_TRUE(signaled) << signaled.error().diagnostic;
  EXPECT_EQ(signaled->termination, ProcessTermination::signaled);
  EXPECT_FALSE(signaled->exit_code.has_value());
  EXPECT_EQ(signaled->signal_number, SIGUSR1);
}

TEST(PosixProcessRunnerTest, ReportsExecFailureAsStructuredError) {
  PosixProcessRunner runner;
  ProcessRequest request{};
  request.executable = "/definitely/not/a/gravesentinel/executable";
  request.timeout = 1s;

  const auto result = runner.run(request);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::not_found);
  EXPECT_FALSE(result.error().diagnostic.empty());
}

TEST(PosixProcessRunnerTest, ClosesInheritedNonCloseOnExecDescriptors) {
  const int source_descriptor = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(source_descriptor, 0);
  const ScopedDescriptor source{source_descriptor};

  const int inherited_descriptor = ::fcntl(source.get(), F_DUPFD, 100);
  ASSERT_GE(inherited_descriptor, 100);
  const ScopedDescriptor inherited{inherited_descriptor};
  const int descriptor_flags = ::fcntl(inherited.get(), F_GETFD);
  ASSERT_GE(descriptor_flags, 0);
  ASSERT_EQ(descriptor_flags & FD_CLOEXEC, 0);

  PosixProcessRunner runner;
  auto request = helper_request("descriptor-state");
  request.argv.push_back(std::to_string(inherited.get()));

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  EXPECT_EQ(result->termination, ProcessTermination::exited);
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->standard_output, "closed\n");
  EXPECT_GE(::fcntl(inherited.get(), F_GETFD), 0);
}

TEST(PosixProcessRunnerTest, TimesOutAndKillsTheEntireProcessGroup) {
  SubreaperScope subreaper;
  ASSERT_EQ(subreaper.error(), 0);

  PosixProcessRunner runner;
  auto request = helper_request("timeout-group");
  request.timeout = 100ms;
  request.termination_grace_period = 50ms;

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  ASSERT_EQ(result->termination, ProcessTermination::timed_out);
  EXPECT_FALSE(result->exit_code.has_value());
  EXPECT_FALSE(result->signal_number.has_value());

  const auto descendant_pid = parse_pid(result->standard_output);
  ASSERT_TRUE(descendant_pid.has_value());
  const auto descendant_status = reap_with_timeout(*descendant_pid);
  ASSERT_TRUE(descendant_status.has_value());
  EXPECT_TRUE(WIFSIGNALED(*descendant_status));
  EXPECT_EQ(WTERMSIG(*descendant_status), SIGKILL);
}

TEST(PosixProcessRunnerTest,
     CompletesKillPhaseAfterTermExitsLeaderAndDescendantClosesStdio) {
  SubreaperScope subreaper;
  ASSERT_EQ(subreaper.error(), 0);

  PosixProcessRunner runner;
  auto request = helper_request("closed-stdio-descendant");
  request.timeout = 100ms;
  request.termination_grace_period = 50ms;

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  ASSERT_EQ(result->termination, ProcessTermination::timed_out);

  const auto descendant_pid = parse_pid(result->standard_output);
  ASSERT_TRUE(descendant_pid.has_value());
  const auto descendant_status = reap_with_timeout(*descendant_pid);
  ASSERT_TRUE(descendant_status.has_value());
  EXPECT_TRUE(WIFSIGNALED(*descendant_status));
  EXPECT_EQ(WTERMSIG(*descendant_status), SIGKILL);
}

TEST(PosixProcessRunnerTest, AppliesRequestedAffinityOnlyToTheChild) {
  auto original = read_current_affinity();
  ASSERT_TRUE(original) << original.error();
  const auto logical_cpu = first_allowed_cpu(*original);
  ASSERT_TRUE(logical_cpu.has_value());

  PosixProcessRunner runner;
  auto request = helper_request("affinity");
  request.logical_cpu = *logical_cpu;

  const auto result = runner.run(request);
  ASSERT_TRUE(result) << result.error().diagnostic;
  ASSERT_EQ(result->exit_code, 0);
  EXPECT_EQ(result->standard_output, std::to_string(*logical_cpu) + "\n");

  auto after = read_current_affinity();
  ASSERT_TRUE(after) << after.error();
  EXPECT_TRUE(equal_cpu_sets(*original, *after));
}

TEST(LinuxAffinityControllerTest,
     ProcessAvailabilityIgnoresNarrowerCallingWorkerBinding) {
  auto process_affinity = read_current_affinity();
  ASSERT_TRUE(process_affinity) << process_affinity.error();

  std::vector<std::uint32_t> allowed_cpus;
  for (std::size_t cpu = 0; cpu < process_affinity->cpu_count; ++cpu) {
    if (CPU_ISSET_S(cpu, process_affinity->size, process_affinity->set.get()) !=
        0) {
      allowed_cpus.push_back(static_cast<std::uint32_t>(cpu));
      if (allowed_cpus.size() == 2U) {
        break;
      }
    }
  }
  if (allowed_cpus.size() < 2U) {
    GTEST_SKIP() << "test requires two allowed logical CPUs";
  }

  LinuxAffinityController controller;
  std::promise<CheckpointResult<void>> result_promise;
  auto result_future = result_promise.get_future();
  std::jthread worker([&] {
    auto binding = controller.bind_current_thread(allowed_cpus[1]);
    if (!binding || *binding == nullptr) {
      result_promise.set_value(std::unexpected(
          binding ? CheckpointErrorDetail{
                        .code = StableErrorCode::internal,
                        .diagnostic = "affinity controller returned null binding",
                        .retryable = false,
                        .correlation_id = std::nullopt,
                        .storage_side_effect = StorageSideEffect::none,
                        .runtime_side_effect = RuntimeSideEffect::none,
                    }
                  : std::move(binding.error())));
      return;
    }

    auto available = controller.validate_cpu_available(allowed_cpus[0]);
    const auto restored = (*binding)->restore();
    if (available && !restored) {
      available = std::unexpected(restored.error());
    }
    result_promise.set_value(std::move(available));
  });
  worker.join();

  const auto result = result_future.get();
  ASSERT_TRUE(result) << result.error().diagnostic;
}

TEST(LinuxAffinityControllerTest,
     RestoresWithExplicitAndRaiiPathsWithoutChangingAnotherThread) {
  auto original = read_current_affinity();
  ASSERT_TRUE(original) << original.error();
  const auto logical_cpu = first_allowed_cpu(*original);
  ASSERT_TRUE(logical_cpu.has_value());

  std::promise<std::expected<TestCpuSet, int>> worker_before_promise;
  std::promise<std::expected<TestCpuSet, int>> worker_during_promise;
  std::promise<void> inspect_worker;
  auto worker_before_future = worker_before_promise.get_future();
  auto worker_during_future = worker_during_promise.get_future();
  auto inspect_worker_future = inspect_worker.get_future();
  std::jthread worker([&worker_before_promise, &worker_during_promise,
                       inspect = std::move(inspect_worker_future)]() mutable {
    worker_before_promise.set_value(read_current_affinity());
    inspect.wait();
    worker_during_promise.set_value(read_current_affinity());
  });

  auto worker_before = worker_before_future.get();
  if (!worker_before) {
    inspect_worker.set_value();
    worker.join();
    FAIL() << worker_before.error();
    return;
  }

  LinuxAffinityController controller;
  auto binding = controller.bind_current_thread(*logical_cpu);
  if (!binding) {
    inspect_worker.set_value();
    worker.join();
    FAIL() << binding.error().diagnostic;
    return;
  }

  auto bound = read_current_affinity();
  EXPECT_TRUE(bound.has_value());
  if (bound) {
    EXPECT_TRUE(contains_only_cpu(*bound, *logical_cpu));
  }

  inspect_worker.set_value();
  auto worker_during = worker_during_future.get();
  worker.join();
  ASSERT_TRUE(worker_during) << worker_during.error();
  EXPECT_TRUE(equal_cpu_sets(*worker_before, *worker_during));

  const auto restored = (*binding)->restore();
  ASSERT_TRUE(restored) << restored.error().diagnostic;
  const auto restored_again = (*binding)->restore();
  EXPECT_TRUE(restored_again) << restored_again.error().diagnostic;
  auto after_explicit_restore = read_current_affinity();
  ASSERT_TRUE(after_explicit_restore) << after_explicit_restore.error();
  EXPECT_TRUE(equal_cpu_sets(*original, *after_explicit_restore));

  {
    auto raii_binding = controller.bind_current_thread(*logical_cpu);
    ASSERT_TRUE(raii_binding) << raii_binding.error().diagnostic;
  }
  auto after_raii_restore = read_current_affinity();
  ASSERT_TRUE(after_raii_restore) << after_raii_restore.error();
  EXPECT_TRUE(equal_cpu_sets(*original, *after_raii_restore));
}

} // namespace
} // namespace gravesentinel::ccm

int main(int argc, char **argv) {
  if (argc >= 2 &&
      std::string_view{argv[1]} == gravesentinel::ccm::helper_marker) {
    return gravesentinel::ccm::run_helper(argc, argv);
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
