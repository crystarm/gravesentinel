#include "src/protocol/types.hxx"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] RuntimeFingerprint make_runtime_fingerprint() {
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
      .command_profile = "podman-criu-rootful-v1",
  };
}

[[nodiscard]] ContainerRegistration
make_registration(std::string suffix, std::uint32_t container_cpu,
                  std::uint32_t checkpoint_cpu) {
  return ContainerRegistration{
      .container_id = ContainerId::create("container-" + suffix).value(),
      .runtime_container =
          RuntimeContainerRef::create("runtime-" + suffix).value(),
      .checkpoint =
          CheckpointConfig{
              .interval = std::chrono::seconds{60},
              .keep_last = 3,
              .container_cpu = container_cpu,
              .checkpoint_cpu = checkpoint_cpu,
          },
      .cold_start = std::nullopt,
      .runtime_fingerprint = std::nullopt,
  };
}

TEST(CheckpointConfigTest, UsesRequiredIntervalAndRetentionDefaults) {
  const CheckpointConfig config{};

  EXPECT_EQ(config.interval, std::chrono::seconds{60});
  EXPECT_EQ(config.keep_last, 3U);
}

TEST(CheckpointConfigTest, AcceptsAllFourStandardCpuPairs) {
  constexpr std::array<std::array<std::uint32_t, 2>, 4> pairs{
      {{0, 1}, {2, 3}, {4, 5}, {6, 7}}};

  for (const auto &pair : pairs) {
    const CheckpointConfig config{
        .interval = std::chrono::seconds{1},
        .keep_last = 1,
        .container_cpu = pair[0],
        .checkpoint_cpu = pair[1],
    };
    EXPECT_TRUE(validate_checkpoint_config(config));
  }
}

TEST(CheckpointConfigTest, RejectsNonpositiveIntervals) {
  auto config = make_registration("one", 0, 1).checkpoint;

  config.interval = std::chrono::seconds::zero();
  EXPECT_EQ(validate_checkpoint_config(config).error().code,
            ValidationErrorCode::invalid_interval);

  config.interval = std::chrono::seconds{-1};
  EXPECT_EQ(validate_checkpoint_config(config).error().code,
            ValidationErrorCode::invalid_interval);
}

TEST(CheckpointConfigTest, RejectsRetentionOutsideOneThroughThree) {
  auto config = make_registration("one", 0, 1).checkpoint;

  config.keep_last = 0;
  EXPECT_EQ(validate_checkpoint_config(config).error().code,
            ValidationErrorCode::invalid_keep_last);

  config.keep_last = 4;
  EXPECT_EQ(validate_checkpoint_config(config).error().code,
            ValidationErrorCode::invalid_keep_last);
}

TEST(CheckpointConfigTest, RejectsInvalidOrReversedCpuPairs) {
  constexpr std::array<std::array<std::uint32_t, 2>, 5> invalid_pairs{
      {{0, 0}, {1, 0}, {0, 3}, {2, 5}, {6, 8}}};

  for (const auto &pair : invalid_pairs) {
    const auto registration = make_registration("invalid", pair[0], pair[1]);
    const auto result = validate_checkpoint_config(registration.checkpoint);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ValidationErrorCode::invalid_cpu_pair);
  }
}

TEST(ContainerRegistrationTest, RejectsEmptyColdStartImage) {
  auto registration = make_registration("one", 0, 1);
  registration.cold_start = ColdStartSpec{};

  const auto result = validate_container_registration(registration);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ValidationErrorCode::invalid_cold_start_spec);
}

TEST(ContainerRegistrationTest, AcceptsCompleteRuntimeFingerprint) {
  auto registration = make_registration("one", 0, 1);
  registration.runtime_fingerprint = make_runtime_fingerprint();

  EXPECT_TRUE(validate_container_registration(registration));
}

TEST(ContainerRegistrationTest, RejectsIncompleteRuntimeFingerprint) {
  auto registration = make_registration("one", 0, 1);
  registration.runtime_fingerprint = RuntimeFingerprint{};
  registration.runtime_fingerprint->runtime_name = "podman";
  registration.runtime_fingerprint->architecture = "x86_64";

  const auto result = validate_container_registration(registration);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code,
            ValidationErrorCode::invalid_runtime_fingerprint);
}

TEST(CpuAssignmentsTest, AcceptsFourUniqueStandardPairs) {
  const std::vector registrations{
      make_registration("zero", 0, 1), make_registration("one", 2, 3),
      make_registration("two", 4, 5), make_registration("three", 6, 7)};

  EXPECT_TRUE(validate_cpu_assignments(registrations));
}

TEST(CpuAssignmentsTest, RejectsPairReuseAcrossRegistrations) {
  const std::vector registrations{make_registration("zero", 0, 1),
                                  make_registration("one", 0, 1)};

  const auto result = validate_cpu_assignments(registrations);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ValidationErrorCode::duplicate_cpu_assignment);
}

TEST(CpuAssignmentsTest, RejectsARegistrationBeyondMvpCapacity) {
  const std::vector registrations{
      make_registration("zero", 0, 1), make_registration("one", 2, 3),
      make_registration("two", 4, 5), make_registration("three", 6, 7),
      make_registration("four", 0, 1)};

  const auto result = validate_cpu_assignments(registrations);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ValidationErrorCode::too_many_registrations);
}

} // namespace
} // namespace gravesentinel::ccm
