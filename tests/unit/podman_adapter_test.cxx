#include "src/hypervisor/podman_adapter.hxx"
#include "src/storage/filesystem_storage.hxx"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kSourceId =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kTargetId =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kImageDigest =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr std::string_view kEnvironmentFilePlaceholder =
    "<transient-environment-file>";

[[nodiscard]] std::string image_identity() {
  return "sha256:" + std::string(kImageDigest);
}

[[nodiscard]] std::string read_test_file(const std::filesystem::path &path) {
  constexpr std::uintmax_t maximum_bytes = 1024U * 1024U;
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_bytes) {
    throw std::runtime_error("test file exceeds its read bound");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open test file");
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
      throw std::runtime_error("test file ended before its recorded size");
    }
  }
  return contents;
}

[[nodiscard]] CheckpointErrorDetail test_error(StableErrorCode code) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = "scripted failure",
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

[[nodiscard]] ProcessResult exited(int code, std::string output = {},
                                   std::string error = {}) {
  return ProcessResult{
      .termination = ProcessTermination::exited,
      .exit_code = code,
      .signal_number = std::nullopt,
      .standard_output = std::move(output),
      .standard_error = std::move(error),
      .standard_output_truncated = false,
      .standard_error_truncated = false,
  };
}

[[nodiscard]] ProcessResult signaled(int signal_number,
                                     std::string error = {}) {
  return ProcessResult{
      .termination = ProcessTermination::signaled,
      .exit_code = std::nullopt,
      .signal_number = signal_number,
      .standard_output = {},
      .standard_error = std::move(error),
      .standard_output_truncated = false,
      .standard_error_truncated = false,
  };
}

[[nodiscard]] ProcessResult timed_out(std::string error = {}) {
  return ProcessResult{
      .termination = ProcessTermination::timed_out,
      .exit_code = std::nullopt,
      .signal_number = std::nullopt,
      .standard_output = {},
      .standard_error = std::move(error),
      .standard_output_truncated = false,
      .standard_error_truncated = false,
  };
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    path_ = std::filesystem::temp_directory_path() /
            ("gravesentinel-podman-adapter-test-" +
             std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(path_);
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
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

struct ScriptStep {
  std::filesystem::path executable;
  std::vector<std::string> argv;
  CheckpointResult<ProcessResult> result;
  std::function<void(const ProcessRequest &)> inspect_request;
};

class ScriptedProcessRunner final : public IProcessRunner {
public:
  void push(ScriptStep step) { steps_.push_back(std::move(step)); }

  [[nodiscard]] CheckpointResult<ProcessResult>
  run(const ProcessRequest &request) override {
    requests_.push_back(request);
    if (steps_.empty()) {
      ADD_FAILURE() << "unexpected process invocation";
      return std::unexpected(test_error(StableErrorCode::internal));
    }
    auto step = std::move(steps_.front());
    steps_.pop_front();
    EXPECT_EQ(request.executable, step.executable);
    auto expected_argv = step.argv;
    if (request.argv.size() == expected_argv.size()) {
      for (std::size_t index = 0U; index < expected_argv.size(); ++index) {
        if (expected_argv[index] == kEnvironmentFilePlaceholder) {
          expected_argv[index] = request.argv[index];
        }
      }
    }
    EXPECT_EQ(request.argv, expected_argv);
    EXPECT_EQ(request.working_directory, std::nullopt);
    EXPECT_EQ(request.logical_cpu, 1U);
    EXPECT_EQ(request.termination_grace_period, 13ms);
    EXPECT_EQ(request.maximum_standard_output_bytes, 256U * 1024U);
    EXPECT_EQ(request.maximum_standard_error_bytes, 32U * 1024U);
    if (step.inspect_request) {
      step.inspect_request(request);
    }
    return std::move(step.result);
  }

  [[nodiscard]] bool empty() const noexcept { return steps_.empty(); }

  [[nodiscard]] const std::vector<ProcessRequest> &requests() const noexcept {
    return requests_;
  }

private:
  std::deque<ScriptStep> steps_;
  std::vector<ProcessRequest> requests_;
};

class FakeAffinityBinding final : public IThreadAffinityBinding {
public:
  explicit FakeAffinityBinding(std::size_t &restore_count)
      : restore_count_(restore_count) {}

  [[nodiscard]] CheckpointResult<void> restore() override {
    ++restore_count_;
    return {};
  }

private:
  std::size_t &restore_count_;
};

class FakeAffinityController final : public IAffinityController {
public:
  [[nodiscard]] CheckpointResult<std::unique_ptr<IThreadAffinityBinding>>
  bind_current_thread(std::uint32_t logical_cpu) override {
    EXPECT_EQ(logical_cpu, 1U);
    ++bind_count;
    return std::make_unique<FakeAffinityBinding>(restore_count);
  }

  [[nodiscard]] CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const override {
    validated_cpus.push_back(logical_cpu);
    return {};
  }

  mutable std::vector<std::uint32_t> validated_cpus;
  std::size_t bind_count{0U};
  std::size_t restore_count{0U};
};

class RecordingLogger final : public ILogger {
public:
  void log(LogLevel, std::string_view message,
           std::span<const LogField> fields) noexcept override {
    try {
      text_.append(message);
      for (const auto &field : fields) {
        text_.append(field.key);
        text_.append(field.value);
      }
    } catch (...) {
    }
  }

  [[nodiscard]] const std::string &text() const noexcept { return text_; }

private:
  std::string text_;
};

[[nodiscard]] PodmanAdapterConfig
adapter_config(const std::filesystem::path &staging_root) {
  return PodmanAdapterConfig{
      .podman_executable = "/qualified/bin/podman",
      .criu_executable = "/qualified/sbin/criu",
      .staging_root = staging_root,
      .environment = {{.name = "PATH", .value = "/qualified/bin"}},
      .profile_timeout = 17ms,
      .inspection_timeout = 19ms,
      .operation_timeout = 23ms,
      .cleanup_timeout = 29ms,
      .termination_grace_period = 13ms,
      .maximum_json_bytes = 128U * 1024U,
      .maximum_standard_output_bytes = 256U * 1024U,
      .maximum_standard_error_bytes = 32U * 1024U,
      .maximum_arguments = 128U,
      .maximum_argument_bytes = 4096U,
      .maximum_environment_variables = 32U,
      .maximum_environment_value_bytes = 8192U,
  };
}

[[nodiscard]] RuntimeFingerprint qualified_fingerprint() {
  return RuntimeFingerprint{
      .runtime_name = "podman",
      .runtime_version = "5.4.2",
      .runtime_build_origin = "Debian 13",
      .architecture = "amd64",
      .kernel_release = "6.12.111+deb13-amd64",
      .criu_version = "4.1.1",
      .oci_runtime_name = "crun",
      .oci_runtime_version = "1.21",
      .oci_runtime_commit = "10269840aa07fb7e6b7e1acff6198692d8ff5c88",
      .oci_runtime_features = "+APPARMOR +CAP +CRIU +SECCOMP +SELINUX",
      .cgroup_version = "v2",
      .cgroup_manager = "systemd",
      .cgroup_controllers = "cpu cpuset hugetlb io memory misc pids rdma",
      .privilege_mode = "rootful",
      .user_namespace_mode = "host",
      .capability_profile = "CapEff=000001ffffffffff",
      .selinux_mode = "disabled",
      .apparmor_profile = "host-default",
      .seccomp_profile = "default",
      .command_profile = "podman-criu-rootful-v2",
  };
}

[[nodiscard]] std::string version_json(std::string_view version = "5.4.2") {
  return R"({"Client":{"Version":")" + std::string(version) +
         R"(","OsArch":"linux/amd64","BuildOrigin":"Debian"}})";
}

[[nodiscard]] std::string info_json(bool apparmor_enabled = true) {
  std::string document =
      R"({"host":{"arch":"amd64","kernel":"6.12.111+deb13-amd64","distribution":{"distribution":"debian","version":"13","codename":"trixie"},"cgroupVersion":"v2","cgroupManager":"systemd","cgroupControllers":["cpuset","cpu","io","memory","hugetlb","pids","rdma","misc"],"serviceIsRemote":false,"idMappings":{"uidmap":null,"gidmap":null},"security":{"rootless":false,"apparmorEnabled":true,"seccompEnabled":true,"seccompProfilePath":"/usr/share/containers/seccomp.json","selinuxEnabled":false,"capabilities":"CAP_CHOWN,CAP_DAC_OVERRIDE,CAP_FOWNER,CAP_FSETID,CAP_KILL,CAP_NET_BIND_SERVICE,CAP_SETFCAP,CAP_SETGID,CAP_SETPCAP,CAP_SETUID,CAP_SYS_CHROOT"},"ociRuntime":{"name":"crun","version":"crun version 1.21\ncommit: 10269840aa07fb7e6b7e1acff6198692d8ff5c88\n+SYSTEMD +SELINUX +APPARMOR +CAP +SECCOMP +CRIU"}},"version":{"Version":"5.4.2"}})";
  if (!apparmor_enabled) {
    const auto position = document.find("\"apparmorEnabled\":true");
    document.replace(position,
                     std::string_view{"\"apparmorEnabled\":true"}.size(),
                     "\"apparmorEnabled\":false");
  }
  return document;
}

[[nodiscard]] std::string
inspect_json(std::string_view runtime_id, bool running,
             std::string_view cpuset = "0", std::string_view managed = "true",
             std::string_view logical_id = "logical-1", int exit_code = 0) {
  return "[{\"Id\":\"" + std::string(runtime_id) + "\",\"Image\":\"" +
         std::string(kImageDigest) +
         "\",\"State\":{\"Running\":" + (running ? "true" : "false") +
         ",\"ExitCode\":" + std::to_string(exit_code) +
         "},\"Config\":{\"Labels\":{\"" +
         std::string(PodmanAdapter::managed_label) + "\":\"" +
         std::string(managed) + "\",\"" +
         std::string(PodmanAdapter::logical_id_label) + "\":\"" +
         std::string(logical_id) + "\"}},\"HostConfig\":{\"CpusetCpus\":\"" +
         std::string(cpuset) + "\"}}]";
}

[[nodiscard]] std::string image_json() {
  return R"([{"Id":")" + image_identity() + R"("}])";
}

void push_profile(ScriptedProcessRunner &runner,
                  const PodmanAdapterConfig &config,
                  std::string version = version_json()) {
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"version", "--format=json"},
      .result = exited(0, std::move(version)),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.profile_timeout);
            EXPECT_EQ(request.environment, config.environment);
          },
  });
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"info", "--format=json"},
      .result = exited(0, info_json()),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.profile_timeout);
          },
  });
  runner.push(ScriptStep{
      .executable = config.criu_executable,
      .argv = {"--version"},
      .result = exited(0, "Version: 4.1.1\nGitID: v4.1.1\n"),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.profile_timeout);
          },
  });
}

void push_container_inspect(ScriptedProcessRunner &runner,
                            const PodmanAdapterConfig &config,
                            std::string reference, std::string document) {
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "inspect", "--format=json", std::move(reference)},
      .result = exited(0, std::move(document)),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.inspection_timeout);
          },
  });
}

void push_image_inspect(ScriptedProcessRunner &runner,
                        const PodmanAdapterConfig &config,
                        std::string reference) {
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"image", "inspect", "--format=json", std::move(reference)},
      .result = exited(0, image_json()),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.inspection_timeout);
          },
  });
}

void push_exists(ScriptedProcessRunner &runner,
                 const PodmanAdapterConfig &config, std::string reference,
                 int normal_exit_code,
                 std::optional<int> external_exit_code = std::nullopt) {
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "exists", reference},
      .result = exited(normal_exit_code),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.inspection_timeout);
          },
  });
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "exists", "--external", std::move(reference)},
      .result = exited(external_exit_code.value_or(normal_exit_code)),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.inspection_timeout);
          },
  });
}

[[nodiscard]] ContainerId container_id(std::string value = "logical-1") {
  return ContainerId::create(std::move(value)).value();
}

[[nodiscard]] RuntimeContainerRef runtime_ref(std::string value = "runtime-1") {
  return RuntimeContainerRef::create(std::move(value)).value();
}

[[nodiscard]] RuntimeIncarnation
runtime_incarnation(std::string runtime_id = std::string(kSourceId)) {
  return RuntimeIncarnation{
      .reference = runtime_ref(),
      .runtime_id = std::move(runtime_id),
  };
}

const RuntimeCpuAssignment kCpuAssignment{
    .container_cpus = CpuSet::create({0U}).value(),
    .checkpoint_cpu = 1U,
};

TEST(PodmanAdapterTest,
     CaptureAcceptsFilesystemStorageArtifactDirectoryAndRecordsIdentity) {
  TemporaryDirectory temporary;
  auto storage = FilesystemStorage::open(temporary.path());
  ASSERT_TRUE(storage) << storage.error().diagnostic;
  const auto logical_container = container_id();
  const auto operation_id = OperationId::create("capture-operation").value();
  auto operation = (*storage)->begin_operation(
      logical_container, operation_id, StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation) << operation.error().diagnostic;
  const auto staging = (*operation)->runtime_artifact_directory();
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true));
  const auto artifact = staging / PodmanAdapter::checkpoint_artifact_name;
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "checkpoint", "--compress=none", "--leave-running",
               "--export", artifact.string(), "runtime-1"},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.operation_timeout);
            std::ofstream output(artifact, std::ios::binary);
            output << "opaque checkpoint bytes";
          },
  });
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto captured = adapter.capture(logical_container, runtime_ref(),
                                        staging, kCpuAssignment);

  ASSERT_TRUE(captured) << captured.error().diagnostic;
  EXPECT_EQ(captured->artifact_path, artifact);
  EXPECT_EQ(captured->source_image_identity, image_identity());
  EXPECT_EQ(captured->runtime_fingerprint, qualified_fingerprint());
  EXPECT_EQ(captured->source_incarnation.runtime_id, kSourceId);
  EXPECT_TRUE(runner.empty());
  EXPECT_EQ(affinity.restore_count, 1U);
}

TEST(PodmanAdapterTest,
     ColdStartUsesHardenedArgvAndKeepsMetacharactersInElements) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_image_inspect(runner, config, "registry.example/fixture:1");
  push_exists(runner, config, "runtime-1", 1);
  const std::string secret = "top secret;$(do-not-execute)";
  const std::string preload = "/tmp/not-a-real-library.so";
  const std::string containers_conf = "/tmp/not-a-real-containers.conf";
  std::filesystem::path observed_environment_file;
  const auto logical = container_id("logical;$(still-one-label)");
  const RuntimeCpuAssignment multi_cpu_assignment{
      .container_cpus = CpuSet::create({0U, 2U}).value(),
      .checkpoint_cpu = 1U,
  };
  const std::vector<std::string> expected_argv{
      "run",
      "--detach",
      "--name",
      "runtime-1",
      "--label",
      "io.gravesentinel.ccm.managed=true",
      "--label",
      "io.gravesentinel.ccm.logical-id=logical;$(still-one-label)",
      "--network=none",
      "--read-only",
      "--cap-drop=all",
      "--passwd=false",
      "--security-opt=no-new-privileges",
      "--cpuset-cpus=0,2",
      "--env-file",
      std::string(kEnvironmentFilePlaceholder),
      "registry.example/fixture:1",
      "; touch /tmp/must-not-exist",
      "$(id)",
  };
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = expected_argv,
      .result = exited(0, std::string(kTargetId) + "\n"),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.operation_timeout);
            EXPECT_EQ(request.environment, config.environment);
            ASSERT_GE(request.argv.size(), 16U);
            observed_environment_file = request.argv[15];
            EXPECT_EQ(observed_environment_file.parent_path(),
                      temporary.path());
            const auto status =
                std::filesystem::status(observed_environment_file);
            EXPECT_EQ(status.permissions() & std::filesystem::perms::all,
                      std::filesystem::perms::owner_read |
                          std::filesystem::perms::owner_write);
            const auto contents = read_test_file(observed_environment_file);
            EXPECT_EQ(contents,
                      "APP_SECRET=" + secret + "\nLD_PRELOAD=" + preload +
                          "\nCONTAINERS_CONF=" + containers_conf + "\n");
            for (const auto &variable : request.environment) {
              EXPECT_NE(variable.name, "APP_SECRET");
              EXPECT_NE(variable.name, "LD_PRELOAD");
              EXPECT_NE(variable.name, "CONTAINERS_CONF");
              EXPECT_NE(variable.value, secret);
            }
            for (const auto &argument : request.argv) {
              EXPECT_EQ(argument.find("APP_SECRET"), std::string::npos);
              EXPECT_EQ(argument.find("LD_PRELOAD"), std::string::npos);
              EXPECT_EQ(argument.find("CONTAINERS_CONF"), std::string::npos);
              EXPECT_EQ(argument.find(secret), std::string::npos);
              EXPECT_EQ(argument.find(preload), std::string::npos);
              EXPECT_EQ(argument.find(containers_conf), std::string::npos);
            }
          },
  });
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kTargetId, true, "0,2", "true",
                                      "logical;$(still-one-label)"));
  PodmanAdapter adapter(runner, affinity, logger, config);
  const ColdStartSpec spec{
      .image_reference = "registry.example/fixture:1",
      .argv = {"; touch /tmp/must-not-exist", "$(id)"},
      .environment =
          {
              {.name = "APP_SECRET", .value = secret},
              {.name = "LD_PRELOAD", .value = preload},
              {.name = "CONTAINERS_CONF", .value = containers_conf},
          },
      .mounts = {},
      .runtime_options = {},
  };

  const auto started =
      adapter.cold_start(logical, runtime_ref(), spec, multi_cpu_assignment);

  ASSERT_TRUE(started) << started.error().diagnostic;
  EXPECT_EQ(started->runtime_id, kTargetId);
  EXPECT_EQ(logger.text().find(secret), std::string::npos);
  EXPECT_FALSE(observed_environment_file.empty());
  EXPECT_FALSE(std::filesystem::exists(observed_environment_file));
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, RestoreUsesQualifiedArgvAndCheckpointCpu) {
  TemporaryDirectory temporary;
  const auto artifact = temporary.path() / "restored.tar";
  {
    std::ofstream output(artifact, std::ios::binary);
    output << "opaque checkpoint bytes";
  }
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_image_inspect(runner, config, image_identity());
  push_exists(runner, config, "runtime-1", 1);
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "restore", "--import", artifact.string(), "--name",
               "runtime-1"},
      .result = exited(0, std::string(kTargetId) + "\n"),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.operation_timeout);
            EXPECT_EQ(request.logical_cpu, 1U);
          },
  });
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kTargetId, true));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto restored = adapter.restore(container_id(), runtime_ref(), artifact,
                                        qualified_fingerprint(),
                                        image_identity(), kCpuAssignment);

  ASSERT_TRUE(restored) << restored.error().diagnostic;
  EXPECT_EQ(restored->runtime_id, kTargetId);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     RestoreFailureIsIndeterminateUntilCleanupProvesAbsence) {
  TemporaryDirectory temporary;
  const auto artifact = temporary.path() / "partial.tar";
  {
    std::ofstream output(artifact, std::ios::binary);
    output << "opaque checkpoint bytes";
  }
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_image_inspect(runner, config, image_identity());
  push_exists(runner, config, "runtime-1", 1);
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "restore", "--import", artifact.string(), "--name",
               "runtime-1"},
      .result = exited(125, {}, "secret-runtime-diagnostic"),
      .inspect_request = {},
  });
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kTargetId, true));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"stop", "--time=10", std::string(kTargetId)},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.cleanup_timeout);
          },
  });
  push_container_inspect(runner, config, std::string(kTargetId),
                         inspect_json(kTargetId, false));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kTargetId)},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.cleanup_timeout);
          },
  });
  push_exists(runner, config, "runtime-1", 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto restored = adapter.restore(container_id(), runtime_ref(), artifact,
                                        qualified_fingerprint(),
                                        image_identity(), kCpuAssignment);
  ASSERT_FALSE(restored);
  EXPECT_EQ(restored.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_EQ(restored.error().diagnostic.find("secret-runtime-diagnostic"),
            std::string::npos);

  const auto cleaned = adapter.cleanup_failed_restore(
      container_id(), runtime_ref(), kCpuAssignment);
  EXPECT_TRUE(cleaned) << cleaned.error().diagnostic;
  EXPECT_EQ(logger.text().find("secret-runtime-diagnostic"), std::string::npos);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, CleanupFailureRemainsUnsuccessfulAndIndeterminate) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kTargetId, true));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"stop", "--time=10", std::string(kTargetId)},
      .result = exited(0),
      .inspect_request = {},
  });
  push_container_inspect(runner, config, std::string(kTargetId),
                         inspect_json(kTargetId, false));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kTargetId)},
      .result = signaled(9, "raw cleanup stderr"),
      .inspect_request = {},
  });
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto cleaned = adapter.cleanup_failed_restore(
      container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_FALSE(cleaned);
  EXPECT_EQ(cleaned.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_EQ(cleaned.error().diagnostic.find("raw cleanup stderr"),
            std::string::npos);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, RejectsProfileMismatchBeforeInspectingImage) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"version", "--format=json"},
      .result = exited(0, version_json("5.4.3")),
      .inspect_request = {},
  });
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"info", "--format=json"},
      .result = exited(0, info_json()),
      .inspect_request = {},
  });
  runner.push(ScriptStep{
      .executable = config.criu_executable,
      .argv = {"--version"},
      .result = exited(0, "Version: 4.1.1\n"),
      .inspect_request = {},
  });
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto compatible = adapter.validate_restore_compatibility(
      qualified_fingerprint(), image_identity(), kCpuAssignment);

  ASSERT_FALSE(compatible);
  EXPECT_EQ(compatible.error().code, StableErrorCode::incompatible_runtime);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, ReportsTheMismatchedQualifiedProfileField) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"version", "--format=json"},
      .result = exited(0, version_json()),
      .inspect_request = {},
  });
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"info", "--format=json"},
      .result = exited(0, info_json(false)),
      .inspect_request = {},
  });
  runner.push(ScriptStep{
      .executable = config.criu_executable,
      .argv = {"--version"},
      .result = exited(0, "Version: 4.1.1\n"),
      .inspect_request = {},
  });
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto compatible = adapter.validate_restore_compatibility(
      qualified_fingerprint(), image_identity(), kCpuAssignment);

  ASSERT_FALSE(compatible);
  EXPECT_EQ(compatible.error().code, StableErrorCode::incompatible_runtime);
  EXPECT_NE(compatible.error().diagnostic.find("AppArmor"), std::string::npos);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, RejectsWrongCpusetAndForeignOwnershipBeforeMutation) {
  TemporaryDirectory temporary;
  const auto staging = temporary.path() / "operation";
  std::filesystem::create_directory(staging);
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true, "2"));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto captured =
      adapter.capture(container_id(), runtime_ref(), staging, kCpuAssignment);

  ASSERT_FALSE(captured);
  EXPECT_EQ(captured.error().code, StableErrorCode::conflict);
  EXPECT_TRUE(runner.empty());

  ScriptedProcessRunner foreign_runner;
  FakeAffinityController foreign_affinity;
  RecordingLogger foreign_logger;
  push_exists(foreign_runner, config, "runtime-1", 0);
  push_container_inspect(foreign_runner, config, "runtime-1",
                         inspect_json(kTargetId, true, "0", "false"));
  PodmanAdapter foreign_adapter(foreign_runner, foreign_affinity,
                                foreign_logger, config);
  const auto cleaned = foreign_adapter.cleanup_failed_restore(
      container_id(), runtime_ref(), kCpuAssignment);
  ASSERT_FALSE(cleaned);
  EXPECT_EQ(cleaned.error().code, StableErrorCode::permission_denied);
  EXPECT_EQ(cleaned.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_TRUE(foreign_runner.empty());
}

TEST(PodmanAdapterTest,
     InspectRuntimeReportsAbsentAfterBothExistenceProbesAreFalse) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_TRUE(observed) << observed.error().diagnostic;
  EXPECT_EQ(observed->status.state, RuntimeState::absent);
  EXPECT_FALSE(observed->status.exit_code.has_value());
  EXPECT_FALSE(observed->incarnation.has_value());
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     InspectRuntimeReportsOwnedRunningContainerAndIncarnation) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_TRUE(observed) << observed.error().diagnostic;
  EXPECT_EQ(observed->status.state, RuntimeState::running);
  EXPECT_FALSE(observed->status.exit_code.has_value());
  ASSERT_TRUE(observed->incarnation.has_value());
  EXPECT_EQ(*observed->incarnation, runtime_incarnation());
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, InspectRuntimeAcceptsEquivalentMultiCpuRangeSyntax) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true, "0,2-4"));
  PodmanAdapter adapter(runner, affinity, logger, config);
  const RuntimeCpuAssignment assignment{
      .container_cpus = CpuSet::create({0U, 2U, 3U, 4U}).value(),
      .checkpoint_cpu = 1U,
  };

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), assignment);

  ASSERT_TRUE(observed) << observed.error().diagnostic;
  EXPECT_EQ(observed->status.state, RuntimeState::running);
  ASSERT_TRUE(observed->incarnation.has_value());
  EXPECT_EQ(*observed->incarnation, runtime_incarnation());
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, InspectRuntimeReportsExitedContainerAndExitCode) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(
      runner, config, "runtime-1",
      inspect_json(kSourceId, false, "0", "true", "logical-1", 7));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_TRUE(observed) << observed.error().diagnostic;
  EXPECT_EQ(observed->status.state, RuntimeState::exited);
  ASSERT_TRUE(observed->status.exit_code.has_value());
  EXPECT_EQ(*observed->status.exit_code, 7);
  ASSERT_TRUE(observed->incarnation.has_value());
  EXPECT_EQ(*observed->incarnation, runtime_incarnation());
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, InspectRuntimeRejectsForeignOwnership) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(
      runner, config, "runtime-1",
      inspect_json(kSourceId, true, "0", "true", "foreign-logical-id"));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_FALSE(observed);
  EXPECT_EQ(observed.error().code, StableErrorCode::permission_denied);
  EXPECT_EQ(observed.error().runtime_side_effect, RuntimeSideEffect::none);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, InspectRuntimeRejectsWrongCpuset) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 0);
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true, "2"));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_FALSE(observed);
  EXPECT_EQ(observed.error().code, StableErrorCode::conflict);
  EXPECT_EQ(observed.error().runtime_side_effect, RuntimeSideEffect::none);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     InspectRuntimeRejectsExternalPresenceWhenNormalContainerIsAbsent) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 1, 0);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto observed =
      adapter.inspect_runtime(container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_FALSE(observed);
  EXPECT_EQ(observed.error().code, StableErrorCode::conflict);
  EXPECT_EQ(observed.error().runtime_side_effect, RuntimeSideEffect::none);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, StopAndRemoveSkipsStopForExitedContainer) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(
      runner, config, "runtime-1",
      inspect_json(kSourceId, false, "0", "true", "logical-1", 7));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kSourceId)},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.cleanup_timeout);
          },
  });
  push_exists(runner, config, "runtime-1", 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  EXPECT_TRUE(removed) << removed.error().diagnostic;
  ASSERT_EQ(runner.requests().size(), 4U);
  EXPECT_EQ(runner.requests()[1].argv,
            (std::vector<std::string>{"rm", std::string(kSourceId)}));
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     StopAndRemovePreservesStopInspectRemoveAbsenceSequence) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"stop", "--time=10", std::string(kSourceId)},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.cleanup_timeout);
          },
  });
  push_container_inspect(runner, config, std::string(kSourceId),
                         inspect_json(kSourceId, false));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kSourceId)},
      .result = exited(0),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.timeout, config.cleanup_timeout);
          },
  });
  push_exists(runner, config, "runtime-1", 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  EXPECT_TRUE(removed) << removed.error().diagnostic;
  ASSERT_EQ(runner.requests().size(), 6U);
  EXPECT_EQ(
      runner.requests()[1].argv,
      (std::vector<std::string>{"stop", "--time=10", std::string(kSourceId)}));
  EXPECT_EQ(runner.requests()[2].argv,
            (std::vector<std::string>{"container", "inspect", "--format=json",
                                      std::string(kSourceId)}));
  EXPECT_EQ(runner.requests()[3].argv,
            (std::vector<std::string>{"rm", std::string(kSourceId)}));
  EXPECT_EQ(runner.requests()[4].argv,
            (std::vector<std::string>{"container", "exists", "runtime-1"}));
  EXPECT_EQ(runner.requests()[5].argv,
            (std::vector<std::string>{"container", "exists", "--external",
                                      "runtime-1"}));
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, PostStopInspectionFailureIsIndeterminate) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, true));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"stop", "--time=10", std::string(kSourceId)},
      .result = exited(0),
      .inspect_request = {},
  });
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"container", "inspect", "--format=json", std::string(kSourceId)},
      .result = timed_out(),
      .inspect_request = {},
  });
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, StableErrorCode::deadline_exceeded);
  EXPECT_EQ(removed.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, RemoveFailureIsIndeterminate) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, false));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kSourceId)},
      .result = timed_out(),
      .inspect_request = {},
  });
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, StableErrorCode::deadline_exceeded);
  EXPECT_EQ(removed.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, AbsenceProofFailureAfterRemoveIsIndeterminate) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kSourceId, false));
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"rm", std::string(kSourceId)},
      .result = exited(0),
      .inspect_request = {},
  });
  push_exists(runner, config, "runtime-1", 0, 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, StableErrorCode::unavailable);
  EXPECT_EQ(removed.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, ProcessNonzeroSignalTimeoutAndTruncationFailClosed) {
  const std::vector<std::pair<ProcessResult, StableErrorCode>> failures{
      {exited(125, {}, "do-not-copy-this-stderr"),
       StableErrorCode::unavailable},
      {signaled(9, "do-not-copy-this-stderr"), StableErrorCode::unavailable},
      {timed_out("do-not-copy-this-stderr"),
       StableErrorCode::deadline_exceeded},
      {ProcessResult{
           .termination = ProcessTermination::exited,
           .exit_code = 0,
           .signal_number = std::nullopt,
           .standard_output = {},
           .standard_error = "do-not-copy-this-stderr",
           .standard_output_truncated = false,
           .standard_error_truncated = true,
       },
       StableErrorCode::unavailable},
  };

  for (const auto &[failure, expected_code] : failures) {
    TemporaryDirectory temporary;
    const auto config = adapter_config(temporary.path());
    ScriptedProcessRunner runner;
    FakeAffinityController affinity;
    RecordingLogger logger;
    push_container_inspect(runner, config, "runtime-1",
                           inspect_json(kSourceId, true));
    runner.push(ScriptStep{
        .executable = config.podman_executable,
        .argv = {"stop", "--time=10", std::string(kSourceId)},
        .result = failure,
        .inspect_request = {},
    });
    PodmanAdapter adapter(runner, affinity, logger, config);

    const auto removed = adapter.stop_and_remove(
        container_id(), runtime_incarnation(), kCpuAssignment);

    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, expected_code);
    EXPECT_EQ(removed.error().runtime_side_effect,
              RuntimeSideEffect::incarnation_may_exist);
    EXPECT_EQ(removed.error().diagnostic.find("do-not-copy-this-stderr"),
              std::string::npos);
    EXPECT_EQ(logger.text().find("do-not-copy-this-stderr"), std::string::npos);
    EXPECT_TRUE(runner.empty());
  }
}

TEST(PodmanAdapterTest, ColdStartFailureDoesNotExposeEnvironmentValue) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_profile(runner, config);
  push_image_inspect(runner, config, "registry.example/fixture:1");
  push_exists(runner, config, "runtime-1", 1);
  const std::string secret = "credential-value-never-log";
  std::filesystem::path observed_environment_file;
  runner.push(ScriptStep{
      .executable = config.podman_executable,
      .argv = {"run", "--detach", "--name", "runtime-1", "--label",
               "io.gravesentinel.ccm.managed=true", "--label",
               "io.gravesentinel.ccm.logical-id=logical-1", "--network=none",
               "--read-only", "--cap-drop=all", "--passwd=false",
               "--security-opt=no-new-privileges", "--cpuset-cpus=0",
               "--env-file", std::string(kEnvironmentFilePlaceholder),
               "registry.example/fixture:1"},
      .result = exited(125, {}, secret),
      .inspect_request =
          [&](const ProcessRequest &request) {
            EXPECT_EQ(request.environment, config.environment);
            ASSERT_GE(request.argv.size(), 16U);
            observed_environment_file = request.argv[15];
            const auto contents = read_test_file(observed_environment_file);
            EXPECT_EQ(contents, "TOKEN=" + secret + "\n");
            for (const auto &argument : request.argv) {
              EXPECT_EQ(argument.find("TOKEN"), std::string::npos);
              EXPECT_EQ(argument.find(secret), std::string::npos);
            }
          },
  });
  PodmanAdapter adapter(runner, affinity, logger, config);
  const ColdStartSpec spec{
      .image_reference = "registry.example/fixture:1",
      .argv = {},
      .environment = {{.name = "TOKEN", .value = secret}},
      .mounts = {},
      .runtime_options = {},
  };

  const auto started =
      adapter.cold_start(container_id(), runtime_ref(), spec, kCpuAssignment);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_EQ(started.error().diagnostic.find(secret), std::string::npos);
  EXPECT_EQ(logger.text().find(secret), std::string::npos);
  EXPECT_FALSE(observed_environment_file.empty());
  EXPECT_FALSE(std::filesystem::exists(observed_environment_file));
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     ConfirmAbsenceRequiresBothNormalAndExternalRecordsToBeMissing) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 1);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto absent =
      adapter.confirm_absence(container_id(), runtime_ref(), kCpuAssignment);

  EXPECT_TRUE(absent) << absent.error().diagnostic;
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     ConfirmAbsenceRefusesPresenceWithoutStoppingOrRemovingAnything) {
  for (const auto &presence :
       {std::pair{0, 1}, std::pair{1, 0}, std::pair{0, 0}}) {
    TemporaryDirectory temporary;
    const auto config = adapter_config(temporary.path());
    ScriptedProcessRunner runner;
    FakeAffinityController affinity;
    RecordingLogger logger;
    push_exists(runner, config, "runtime-1", presence.first, presence.second);
    PodmanAdapter adapter(runner, affinity, logger, config);

    const auto absent =
        adapter.confirm_absence(container_id(), runtime_ref(), kCpuAssignment);

    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, StableErrorCode::conflict);
    EXPECT_EQ(absent.error().runtime_side_effect, RuntimeSideEffect::none);
    ASSERT_EQ(runner.requests().size(), 2U);
    EXPECT_EQ(runner.requests()[0].argv,
              (std::vector<std::string>{"container", "exists", "runtime-1"}));
    EXPECT_EQ(runner.requests()[1].argv,
              (std::vector<std::string>{"container", "exists", "--external",
                                        "runtime-1"}));
    EXPECT_TRUE(runner.empty());
  }
}

TEST(PodmanAdapterTest,
     CleanupRejectsExternalPresenceWhenNormalContainerIsAbsent) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_exists(runner, config, "runtime-1", 1, 0);
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto cleaned = adapter.cleanup_failed_restore(
      container_id(), runtime_ref(), kCpuAssignment);

  ASSERT_FALSE(cleaned);
  EXPECT_EQ(cleaned.error().code, StableErrorCode::conflict);
  EXPECT_EQ(cleaned.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest,
     StopAndRemoveRejectsReplacementWithMatchingOwnershipLabels) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  push_container_inspect(runner, config, "runtime-1",
                         inspect_json(kTargetId, true));
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto removed = adapter.stop_and_remove(
      container_id(), runtime_incarnation(), kCpuAssignment);

  ASSERT_FALSE(removed);
  EXPECT_EQ(removed.error().code, StableErrorCode::conflict);
  EXPECT_TRUE(runner.empty());
}

TEST(PodmanAdapterTest, RejectsEnvironmentValuesContainingCrOrLf) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  PodmanAdapter adapter(runner, affinity, logger, config);
  const ColdStartSpec spec{
      .image_reference = "registry.example/fixture:1",
      .argv = {},
      .environment = {{.name = "TOKEN", .value = "first\r\nsecond"}},
      .mounts = {},
      .runtime_options = {},
  };

  const auto started =
      adapter.cold_start(container_id(), runtime_ref(), spec, kCpuAssignment);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().code, StableErrorCode::invalid_argument);
  EXPECT_TRUE(runner.requests().empty());
  EXPECT_TRUE(std::filesystem::is_empty(temporary.path()));
}

TEST(PodmanAdapterTest, RejectsGroupWritableStagingRoot) {
  TemporaryDirectory temporary;
  std::filesystem::permissions(temporary.path(),
                               std::filesystem::perms::group_write,
                               std::filesystem::perm_options::add);
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  PodmanAdapter adapter(runner, affinity, logger, config);

  const auto compatible = adapter.validate_restore_compatibility(
      qualified_fingerprint(), image_identity(), kCpuAssignment);

  ASSERT_FALSE(compatible);
  EXPECT_EQ(compatible.error().code, StableErrorCode::invalid_argument);
  EXPECT_TRUE(runner.requests().empty());
}

TEST(PodmanAdapterTest, RejectsMountsAndRuntimeOptionsWithoutGuessing) {
  TemporaryDirectory temporary;
  const auto config = adapter_config(temporary.path());
  ScriptedProcessRunner runner;
  FakeAffinityController affinity;
  RecordingLogger logger;
  PodmanAdapter adapter(runner, affinity, logger, config);
  const ColdStartSpec spec{
      .image_reference = "registry.example/fixture:1",
      .argv = {},
      .environment = {},
      .mounts = {{.source = "/host", .target = "/guest", .read_only = true}},
      .runtime_options = {"--privileged"},
  };

  const auto started =
      adapter.cold_start(container_id(), runtime_ref(), spec, kCpuAssignment);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().code, StableErrorCode::invalid_argument);
  EXPECT_TRUE(runner.requests().empty());
}

} // namespace
} // namespace gravesentinel::ccm
