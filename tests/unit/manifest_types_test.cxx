#include "src/manifest/types.hxx"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

constexpr std::uint32_t kRegularFileMode = 0100644U;

static_assert(kRegularFileMode ==
              (ManifestArtifactV1::posix_regular_file | 0644U));

[[nodiscard]] ChunkHash make_hash(std::byte value) {
  ChunkHash::Bytes bytes{};
  bytes.fill(value);
  return ChunkHash{bytes};
}

[[nodiscard]] RelativeSnapshotPath make_path(std::string value) {
  return std::move(RelativeSnapshotPath::create(std::move(value))).value();
}

[[nodiscard]] RuntimeFingerprint make_valid_runtime_fingerprint() {
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

[[nodiscard]] ManifestV1 make_valid_manifest() {
  return ManifestV1{
      .container_id = ContainerId::create("container-01").value(),
      .checkpoint_id = CheckpointId::create("checkpoint-0007").value(),
      .generation = 7,
      .created_at_unix_nanoseconds = 1730000000123456789ULL,
      .source_image_identity =
          "registry.example/gravesentinel@sha256:0123456789abcdef",
      .runtime_fingerprint = make_valid_runtime_fingerprint(),
      .artifacts =
          {
              ManifestArtifactV1{
                  .path = make_path("memory/pages-1.img"),
                  .size = ManifestLimits::chunk_bytes + 17U,
                  .mode = kRegularFileMode,
                  .chunks =
                      {
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xAB}),
                              .size = ManifestLimits::chunk_bytes,
                          },
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xCD}),
                              .size = 17,
                          },
                      },
              },
              ManifestArtifactV1{
                  .path = make_path("metadata/inventory.json"),
                  .size = 29,
                  .mode = kRegularFileMode,
                  .chunks =
                      {
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xEF}),
                              .size = 29,
                          },
                      },
              },
          },
  };
}

void expect_manifest_error(const ManifestV1 &manifest,
                           ManifestErrorCode expected_code) {
  const auto result = validate_manifest_v1(manifest);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, expected_code);
}

TEST(RelativeSnapshotPathTest, AcceptsCanonicalNestedPath) {
  const auto result = RelativeSnapshotPath::create("memory/parent/pages.img");

  ASSERT_TRUE(result);
  EXPECT_EQ(result->value(), "memory/parent/pages.img");
}

TEST(RelativeSnapshotPathTest, RejectsUnsafeAndNoncanonicalForms) {
  const std::array invalid_paths{
      std::string{},
      std::string{"/absolute/path"},
      std::string{"safe/../escape"},
      std::string{"safe/./file"},
      std::string{"safe//file"},
      std::string{"safe/file/"},
      std::string{"safe\\file"},
      std::string{"C:/snapshot/file"},
      std::string{"nul\0byte", 8},
      std::string{"control\x1f", 8},
      std::string{"\xC3\x28", 2},
  };

  for (const auto &path : invalid_paths) {
    SCOPED_TRACE(testing::PrintToString(path));
    const auto result = RelativeSnapshotPath::create(path);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ManifestErrorCode::invalid_relative_path);
  }
}

TEST(RelativeSnapshotPathTest, RejectsComponentOverByteLimit) {
  const std::string oversized_component(
      ManifestLimits::maximum_path_component_bytes + 1U, 'a');

  const auto result = RelativeSnapshotPath::create(oversized_component);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ManifestErrorCode::invalid_relative_path);
}

TEST(RelativeSnapshotPathTest, RejectsPathOverByteLimit) {
  std::string oversized_path;
  while (oversized_path.size() <= ManifestLimits::maximum_relative_path_bytes) {
    if (!oversized_path.empty()) {
      oversized_path.push_back('/');
    }
    oversized_path.append("segment");
  }
  ASSERT_GT(oversized_path.size(), ManifestLimits::maximum_relative_path_bytes);

  const auto result = RelativeSnapshotPath::create(oversized_path);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ManifestErrorCode::invalid_relative_path);
}

TEST(JoinWithinRootTest, JoinsCanonicalPathUnderNormalizedAbsoluteRoot) {
  const std::filesystem::path root =
      "/var/lib/gravesentinel/../gravesentinel/snapshots";
  const auto relative_path = make_path("memory/pages.img");

  const auto result = join_within_root(root, relative_path);

  ASSERT_TRUE(result);
  EXPECT_EQ(*result, std::filesystem::path{
                         "/var/lib/gravesentinel/snapshots/memory/pages.img"});
}

TEST(JoinWithinRootTest, AcceptsAbsoluteRootWithTrailingSeparators) {
  const auto relative_path = make_path("memory/pages.img");

  const auto trailing_slash =
      join_within_root("/snapshot/root/", relative_path);
  const auto trailing_dot = join_within_root("/snapshot/root/.", relative_path);

  ASSERT_TRUE(trailing_slash);
  ASSERT_TRUE(trailing_dot);
  EXPECT_EQ(*trailing_slash,
            std::filesystem::path{"/snapshot/root/memory/pages.img"});
  EXPECT_EQ(*trailing_dot,
            std::filesystem::path{"/snapshot/root/memory/pages.img"});
}

TEST(JoinWithinRootTest, RejectsRelativeRoot) {
  const auto result =
      join_within_root("relative/root", make_path("memory/pages.img"));

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ManifestErrorCode::invalid_root);
}

TEST(ManifestValidationTest, AcceptsValidManifestWithoutReorderingIt) {
  auto manifest = make_valid_manifest();
  const auto original = manifest;

  const auto result = validate_manifest_v1(manifest);

  ASSERT_TRUE(result);
  EXPECT_EQ(manifest, original);
  ASSERT_EQ(manifest.artifacts.size(), 2U);
  EXPECT_EQ(manifest.artifacts[0].path.value(), "memory/pages-1.img");
  EXPECT_EQ(manifest.artifacts[1].path.value(), "metadata/inventory.json");
  ASSERT_EQ(manifest.artifacts[0].chunks.size(), 2U);
  EXPECT_EQ(manifest.artifacts[0].chunks[0].hash, make_hash(std::byte{0xAB}));
  EXPECT_EQ(manifest.artifacts[0].chunks[1].hash, make_hash(std::byte{0xCD}));
}

TEST(ManifestValidationTest, RejectsZeroGeneration) {
  auto manifest = make_valid_manifest();
  manifest.generation = 0;

  expect_manifest_error(manifest, ManifestErrorCode::invalid_generation);
}

TEST(ManifestValidationTest, RejectsDuplicateArtifactPaths) {
  auto manifest = make_valid_manifest();
  manifest.artifacts[1].path = manifest.artifacts[0].path;

  expect_manifest_error(manifest, ManifestErrorCode::duplicate_artifact_path);
}

TEST(ManifestValidationTest, RejectsSymlinkAndSpecialFileModes) {
  constexpr std::array invalid_modes{
      0120777U,
      0040755U,
      0010644U,
  };

  for (const auto mode : invalid_modes) {
    SCOPED_TRACE(mode);
    auto manifest = make_valid_manifest();
    manifest.artifacts[0].mode = mode;
    expect_manifest_error(manifest, ManifestErrorCode::invalid_file_mode);
  }
}

TEST(ManifestValidationTest, AcceptsEmptyFileOnlyWithoutChunks) {
  auto manifest = make_valid_manifest();
  auto &artifact = manifest.artifacts[0];
  artifact.size = 0;
  artifact.chunks.clear();

  EXPECT_TRUE(validate_manifest_v1(manifest));

  artifact.chunks.push_back(ManifestChunkV1{
      .hash = make_hash(std::byte{0x01}),
      .size = 1,
  });
  expect_manifest_error(manifest, ManifestErrorCode::invalid_chunk_layout);
}

TEST(ManifestValidationTest, RejectsNonemptyFileWithoutChunks) {
  auto manifest = make_valid_manifest();
  manifest.artifacts[0].chunks.clear();

  expect_manifest_error(manifest, ManifestErrorCode::invalid_chunk_layout);
}

TEST(ManifestValidationTest, RejectsZeroOversizedAndHugeChunkSizes) {
  constexpr std::array invalid_sizes{
      std::uint64_t{0},
      ManifestLimits::chunk_bytes + 1U,
      std::numeric_limits<std::uint64_t>::max(),
  };

  for (const auto size : invalid_sizes) {
    SCOPED_TRACE(size);
    auto manifest = make_valid_manifest();
    manifest.artifacts[0].chunks.back().size = size;
    expect_manifest_error(manifest, ManifestErrorCode::invalid_chunk_layout);
  }
}

TEST(ManifestValidationTest, RequiresEveryNonfinalChunkToBeExactlyFourMiB) {
  auto manifest = make_valid_manifest();
  manifest.artifacts[0].chunks.front().size = ManifestLimits::chunk_bytes - 1U;

  expect_manifest_error(manifest, ManifestErrorCode::invalid_chunk_layout);
}

TEST(ManifestValidationTest, RejectsArtifactAndChunkSizeMismatch) {
  auto manifest = make_valid_manifest();
  ++manifest.artifacts[0].size;

  expect_manifest_error(manifest, ManifestErrorCode::invalid_chunk_layout);
}

TEST(ManifestValidationTest, RejectsTooManyArtifacts) {
  auto manifest = make_valid_manifest();
  manifest.artifacts.clear();
  manifest.artifacts.reserve(ManifestLimits::maximum_artifacts + 1U);
  for (std::size_t index = 0; index < ManifestLimits::maximum_artifacts + 1U;
       ++index) {
    manifest.artifacts.push_back(ManifestArtifactV1{
        .path = make_path("artifact/" + std::to_string(index)),
        .size = 0,
        .mode = kRegularFileMode,
        .chunks = {},
    });
  }

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsTooManyChunks) {
  auto manifest = make_valid_manifest();
  auto &artifact = manifest.artifacts[0];
  artifact.size = 1;
  artifact.chunks.assign(ManifestLimits::maximum_chunks + 1U,
                         ManifestChunkV1{
                             .hash = make_hash(std::byte{0x01}),
                             .size = 1,
                         });

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsTotalSnapshotSizeOverLimit) {
  auto manifest = make_valid_manifest();
  manifest.artifacts[1].size = ManifestLimits::maximum_snapshot_bytes;

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsUint64MaxArtifactSizeBeforeArithmetic) {
  auto manifest = make_valid_manifest();
  manifest.artifacts[0].size = std::numeric_limits<std::uint64_t>::max();

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsOversizedIdentifiers) {
  const std::string oversized(ManifestLimits::maximum_identifier_bytes + 1U,
                              'i');

  auto container_manifest = make_valid_manifest();
  container_manifest.container_id = ContainerId::create(oversized).value();
  expect_manifest_error(container_manifest, ManifestErrorCode::limit_exceeded);

  auto checkpoint_manifest = make_valid_manifest();
  checkpoint_manifest.checkpoint_id = CheckpointId::create(oversized).value();
  expect_manifest_error(checkpoint_manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsOversizedImageIdentity) {
  auto manifest = make_valid_manifest();
  manifest.source_image_identity.assign(
      ManifestLimits::maximum_source_image_bytes + 1U, 'i');

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestValidationTest, RejectsOversizedRuntimeMetadata) {
  auto manifest = make_valid_manifest();
  manifest.runtime_fingerprint.command_profile.assign(
      ManifestLimits::maximum_runtime_field_bytes + 1U, 'r');

  expect_manifest_error(manifest, ManifestErrorCode::limit_exceeded);
}

} // namespace
} // namespace gravesentinel::ccm
