#pragma once

#include "src/protocol/types.hxx"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace gravesentinel::ccm {

struct ManifestLimits final {
  static constexpr std::size_t maximum_encoded_bytes = 16U * 1024U * 1024U;
  static constexpr std::size_t maximum_json_depth = 8;
  static constexpr std::size_t maximum_json_object_fields = 32;
  static constexpr std::size_t maximum_json_nodes = 550000;
  static constexpr std::size_t maximum_artifacts = 4096;
  static constexpr std::size_t maximum_chunks = 131072;
  static constexpr std::uint64_t chunk_bytes = 4U * 1024U * 1024U;
  static constexpr std::uint64_t maximum_snapshot_bytes = 1ULL << 40U;
  static constexpr std::size_t maximum_identifier_bytes = 256;
  static constexpr std::size_t maximum_source_image_bytes = 2048;
  static constexpr std::size_t maximum_runtime_field_bytes = 2048;
  static constexpr std::size_t maximum_relative_path_bytes = 4096;
  static constexpr std::size_t maximum_path_component_bytes = 255;
};

enum class ManifestErrorCode : std::uint8_t {
  empty_value,
  invalid_text,
  invalid_relative_path,
  invalid_root,
  limit_exceeded,
  integer_overflow,
  invalid_generation,
  invalid_file_mode,
  invalid_chunk_layout,
  duplicate_artifact_path,
  malformed_json,
  schema_violation,
  unsupported_version,
  invalid_hash_encoding,
  checksum_mismatch,
  non_canonical_encoding,
};

struct ManifestError {
  ManifestErrorCode code;
  std::string field;
  std::string diagnostic;

  bool operator==(const ManifestError &) const = default;
};

template <typename T> using ManifestResult = std::expected<T, ManifestError>;

class RelativeSnapshotPath final {
public:
  [[nodiscard]] static ManifestResult<RelativeSnapshotPath>
  create(std::string value);

  [[nodiscard]] const std::string &value() const noexcept { return value_; }

  auto operator<=>(const RelativeSnapshotPath &) const = default;

private:
  explicit RelativeSnapshotPath(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

// This is a lexical join only. Filesystem users must additionally open relative
// to a trusted directory descriptor with no-follow/beneath semantics.
[[nodiscard]] ManifestResult<std::filesystem::path>
join_within_root(const std::filesystem::path &root,
                 const RelativeSnapshotPath &relative_path);

struct ManifestChunkV1 {
  ChunkHash hash;
  std::uint64_t size;

  bool operator==(const ManifestChunkV1 &) const = default;
};

struct ManifestArtifactV1 {
  static constexpr std::uint32_t posix_file_type_mask = 0170000U;
  static constexpr std::uint32_t posix_regular_file = 0100000U;
  static constexpr std::uint32_t posix_permission_bits = 0007777U;

  RelativeSnapshotPath path;
  std::uint64_t size;
  std::uint32_t mode;
  std::vector<ManifestChunkV1> chunks;

  bool operator==(const ManifestArtifactV1 &) const = default;
};

struct ManifestV1 {
  static constexpr std::uint64_t format_version = 1;

  ContainerId container_id;
  CheckpointId checkpoint_id;
  std::uint64_t generation;
  std::uint64_t created_at_unix_nanoseconds;
  std::string source_image_identity;
  RuntimeFingerprint runtime_fingerprint;
  std::vector<ManifestArtifactV1> artifacts;

  bool operator==(const ManifestV1 &) const = default;
};

[[nodiscard]] ManifestResult<void>
validate_manifest_v1(const ManifestV1 &manifest);

} // namespace gravesentinel::ccm
