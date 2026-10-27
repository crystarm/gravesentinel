#include "src/manifest/types.hxx"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] ManifestError
make_error(ManifestErrorCode code, std::string field, std::string diagnostic) {
  return ManifestError{
      .code = code,
      .field = std::move(field),
      .diagnostic = std::move(diagnostic),
  };
}

[[nodiscard]] std::uint8_t byte_at(std::string_view value,
                                   std::size_t index) noexcept {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(value[index]));
}

[[nodiscard]] bool
is_valid_utf8_without_controls(std::string_view value) noexcept {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = byte_at(value, index);
    std::size_t sequence_size = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum_code_point = 0;

    if (first <= 0x7FU) {
      sequence_size = 1;
      code_point = first;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      sequence_size = 2;
      code_point = static_cast<std::uint32_t>(first & 0x1FU);
      minimum_code_point = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      sequence_size = 3;
      code_point = static_cast<std::uint32_t>(first & 0x0FU);
      minimum_code_point = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      sequence_size = 4;
      code_point = static_cast<std::uint32_t>(first & 0x07U);
      minimum_code_point = 0x10000U;
    } else {
      return false;
    }

    if (sequence_size > value.size() - index) {
      return false;
    }
    for (std::size_t offset = 1; offset < sequence_size; ++offset) {
      const auto continuation = byte_at(value, index + offset);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point =
          (code_point << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
    }

    if (code_point < minimum_code_point || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU) ||
        code_point <= 0x1FU || (code_point >= 0x7FU && code_point <= 0x9FU)) {
      return false;
    }
    index += sequence_size;
  }
  return true;
}

[[nodiscard]] ManifestResult<void> validate_text(std::string_view value,
                                                 std::string_view field,
                                                 std::size_t maximum_bytes) {
  if (value.empty()) {
    return std::unexpected(make_error(ManifestErrorCode::empty_value,
                                      std::string(field),
                                      "value must not be empty"));
  }
  if (value.size() > maximum_bytes) {
    return std::unexpected(make_error(ManifestErrorCode::limit_exceeded,
                                      std::string(field),
                                      "value exceeds the manifest byte limit"));
  }
  if (!is_valid_utf8_without_controls(value)) {
    return std::unexpected(
        make_error(ManifestErrorCode::invalid_text, std::string(field),
                   "value must be valid UTF-8 without control characters"));
  }
  return {};
}

[[nodiscard]] ManifestResult<void>
validate_runtime_fingerprint(const RuntimeFingerprint &fingerprint) {
  const std::pair<std::string_view, std::string_view> fields[]{
      {"runtime_fingerprint.runtime_name", fingerprint.runtime_name},
      {"runtime_fingerprint.runtime_version", fingerprint.runtime_version},
      {"runtime_fingerprint.runtime_build_origin",
       fingerprint.runtime_build_origin},
      {"runtime_fingerprint.architecture", fingerprint.architecture},
      {"runtime_fingerprint.kernel_release", fingerprint.kernel_release},
      {"runtime_fingerprint.criu_version", fingerprint.criu_version},
      {"runtime_fingerprint.oci_runtime_name", fingerprint.oci_runtime_name},
      {"runtime_fingerprint.oci_runtime_version",
       fingerprint.oci_runtime_version},
      {"runtime_fingerprint.oci_runtime_commit",
       fingerprint.oci_runtime_commit},
      {"runtime_fingerprint.oci_runtime_features",
       fingerprint.oci_runtime_features},
      {"runtime_fingerprint.cgroup_version", fingerprint.cgroup_version},
      {"runtime_fingerprint.cgroup_manager", fingerprint.cgroup_manager},
      {"runtime_fingerprint.cgroup_controllers",
       fingerprint.cgroup_controllers},
      {"runtime_fingerprint.privilege_mode", fingerprint.privilege_mode},
      {"runtime_fingerprint.user_namespace_mode",
       fingerprint.user_namespace_mode},
      {"runtime_fingerprint.capability_profile",
       fingerprint.capability_profile},
      {"runtime_fingerprint.selinux_mode", fingerprint.selinux_mode},
      {"runtime_fingerprint.apparmor_profile", fingerprint.apparmor_profile},
      {"runtime_fingerprint.seccomp_profile", fingerprint.seccomp_profile},
      {"runtime_fingerprint.command_profile", fingerprint.command_profile},
  };

  for (const auto &[field, value] : fields) {
    if (auto result = validate_text(
            value, field, ManifestLimits::maximum_runtime_field_bytes);
        !result) {
      return result;
    }
  }
  return {};
}

[[nodiscard]] bool has_windows_drive_prefix(std::string_view value) noexcept {
  if (value.size() < 2 || value[1] != ':') {
    return false;
  }
  return std::isalpha(static_cast<unsigned char>(
             static_cast<unsigned char>(value[0]))) != 0;
}

} // namespace

ManifestResult<RelativeSnapshotPath>
RelativeSnapshotPath::create(std::string value) {
  if (auto result = validate_text(value, "path",
                                  ManifestLimits::maximum_relative_path_bytes);
      !result) {
    auto error = std::move(result.error());
    error.code = ManifestErrorCode::invalid_relative_path;
    return std::unexpected(std::move(error));
  }
  if (value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string::npos ||
      has_windows_drive_prefix(value)) {
    return std::unexpected(
        make_error(ManifestErrorCode::invalid_relative_path, "path",
                   "path must be a canonical relative POSIX path"));
  }

  std::size_t component_begin = 0;
  while (component_begin < value.size()) {
    const auto separator = value.find('/', component_begin);
    const auto component_end =
        separator == std::string::npos ? value.size() : separator;
    const auto component = std::string_view(value).substr(
        component_begin, component_end - component_begin);
    if (component.empty() || component == "." || component == "..") {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_relative_path, "path",
          "path must not contain empty, dot, or dot-dot components"));
    }
    if (component.size() > ManifestLimits::maximum_path_component_bytes) {
      return std::unexpected(
          make_error(ManifestErrorCode::invalid_relative_path, "path",
                     "path component exceeds the manifest byte limit"));
    }
    if (separator == std::string::npos) {
      break;
    }
    component_begin = separator + 1;
  }

  return RelativeSnapshotPath(std::move(value));
}

ManifestResult<std::filesystem::path>
join_within_root(const std::filesystem::path &root,
                 const RelativeSnapshotPath &relative_path) {
  if (root.empty() || !root.is_absolute() ||
      root.native().find('\0') != std::filesystem::path::string_type::npos) {
    return std::unexpected(make_error(
        ManifestErrorCode::invalid_root, "root",
        "snapshot root must be a nonempty absolute path without NUL"));
  }

  auto normalized_root = root.lexically_normal();
  while (normalized_root != normalized_root.root_path() &&
         normalized_root.filename().empty()) {
    normalized_root = normalized_root.parent_path();
  }
  const auto joined =
      (normalized_root / std::filesystem::path(relative_path.value()))
          .lexically_normal();

  auto root_component = normalized_root.begin();
  auto joined_component = joined.begin();
  for (; root_component != normalized_root.end();
       ++root_component, ++joined_component) {
    if (joined_component == joined.end() ||
        *root_component != *joined_component) {
      return std::unexpected(
          make_error(ManifestErrorCode::invalid_relative_path, "path",
                     "joined path escapes the snapshot root"));
    }
  }
  return joined;
}

ManifestResult<void>
validate_manifest_artifacts_v1(std::span<const ManifestArtifactV1> artifacts) {
  if (artifacts.size() > ManifestLimits::maximum_artifacts) {
    return std::unexpected(make_error(ManifestErrorCode::limit_exceeded,
                                      "artifacts",
                                      "manifest contains too many artifacts"));
  }

  std::unordered_set<std::string> paths;
  paths.reserve(artifacts.size());
  std::size_t total_chunks = 0;
  std::uint64_t total_size = 0;

  for (std::size_t artifact_index = 0; artifact_index < artifacts.size();
       ++artifact_index) {
    const auto &artifact = artifacts[artifact_index];
    const auto field_prefix =
        "artifacts[" + std::to_string(artifact_index) + "]";

    if (!paths.insert(artifact.path.value()).second) {
      return std::unexpected(make_error(
          ManifestErrorCode::duplicate_artifact_path, field_prefix + ".path",
          "artifact paths must be unique within a manifest"));
    }

    constexpr auto allowed_mode_bits =
        ManifestArtifactV1::posix_file_type_mask |
        ManifestArtifactV1::posix_permission_bits;
    if ((artifact.mode & ManifestArtifactV1::posix_file_type_mask) !=
            ManifestArtifactV1::posix_regular_file ||
        (artifact.mode & ~allowed_mode_bits) != 0U) {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_file_mode, field_prefix + ".mode",
          "artifact mode must describe a regular file"));
    }

    if (artifact.size > ManifestLimits::maximum_snapshot_bytes) {
      return std::unexpected(
          make_error(ManifestErrorCode::limit_exceeded, field_prefix + ".size",
                     "artifact exceeds the snapshot byte limit"));
    }
    if (artifact.size >
        std::numeric_limits<std::uint64_t>::max() - total_size) {
      return std::unexpected(
          make_error(ManifestErrorCode::integer_overflow, "artifacts",
                     "total artifact size overflows uint64"));
    }
    total_size += artifact.size;
    if (total_size > ManifestLimits::maximum_snapshot_bytes) {
      return std::unexpected(
          make_error(ManifestErrorCode::limit_exceeded, "artifacts",
                     "total artifact size exceeds the snapshot byte limit"));
    }

    if (artifact.chunks.size() >
        ManifestLimits::maximum_chunks - total_chunks) {
      return std::unexpected(make_error(ManifestErrorCode::limit_exceeded,
                                        field_prefix + ".chunks",
                                        "manifest contains too many chunks"));
    }
    total_chunks += artifact.chunks.size();

    if (artifact.size == 0 && !artifact.chunks.empty()) {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_chunk_layout, field_prefix + ".chunks",
          "an empty artifact must contain zero chunks"));
    }
    if (artifact.size != 0 && artifact.chunks.empty()) {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_chunk_layout, field_prefix + ".chunks",
          "a nonempty artifact must contain at least one chunk"));
    }

    std::uint64_t chunk_total = 0;
    for (std::size_t chunk_index = 0; chunk_index < artifact.chunks.size();
         ++chunk_index) {
      const auto &chunk = artifact.chunks[chunk_index];
      const auto chunk_field =
          field_prefix + ".chunks[" + std::to_string(chunk_index) + "].size";
      if (chunk.size == 0 || chunk.size > ManifestLimits::chunk_bytes) {
        return std::unexpected(
            make_error(ManifestErrorCode::invalid_chunk_layout, chunk_field,
                       "chunk size must be in the range 1..4 MiB"));
      }
      if (chunk_index + 1 < artifact.chunks.size() &&
          chunk.size != ManifestLimits::chunk_bytes) {
        return std::unexpected(
            make_error(ManifestErrorCode::invalid_chunk_layout, chunk_field,
                       "every non-final chunk must be exactly 4 MiB"));
      }
      if (chunk.size >
          std::numeric_limits<std::uint64_t>::max() - chunk_total) {
        return std::unexpected(
            make_error(ManifestErrorCode::integer_overflow, chunk_field,
                       "artifact chunk sizes overflow uint64"));
      }
      chunk_total += chunk.size;
    }
    if (chunk_total != artifact.size) {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_chunk_layout, field_prefix + ".size",
          "artifact size must equal the sum of its chunk sizes"));
    }
  }

  std::unordered_set<std::string> directories;
  directories.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    const auto &path = artifact.path.value();
    const auto component_count =
        1U + static_cast<std::size_t>(std::ranges::count(path, '/'));
    if (component_count > ManifestLimits::maximum_path_components) {
      return std::unexpected(
          make_error(ManifestErrorCode::limit_exceeded, "artifacts",
                     "artifact path exceeds the supported component depth"));
    }

    auto separator = path.find('/');
    while (separator != std::string::npos) {
      const auto prefix = path.substr(0, separator);
      if (paths.contains(prefix)) {
        return std::unexpected(make_error(
            ManifestErrorCode::artifact_path_prefix_conflict, "artifacts",
            "an artifact path must not be the parent of another artifact"));
      }
      directories.insert(prefix);
      if (directories.size() + 1U > ManifestLimits::maximum_directories) {
        return std::unexpected(
            make_error(ManifestErrorCode::limit_exceeded, "artifacts",
                       "artifact paths require too many directories"));
      }
      separator = path.find('/', separator + 1U);
    }
  }

  return {};
}

ManifestResult<void> validate_manifest_v2(const ManifestV2 &manifest) {
  if (manifest.generation == 0) {
    return std::unexpected(
        make_error(ManifestErrorCode::invalid_generation, "generation",
                   "manifest generation must be greater than zero"));
  }

  if (auto result = validate_text(manifest.container_id.value(), "container_id",
                                  ManifestLimits::maximum_identifier_bytes);
      !result) {
    return result;
  }
  if (auto result =
          validate_text(manifest.checkpoint_id.value(), "checkpoint_id",
                        ManifestLimits::maximum_identifier_bytes);
      !result) {
    return result;
  }
  if (auto result =
          validate_text(manifest.source_image_identity, "source_image_identity",
                        ManifestLimits::maximum_source_image_bytes);
      !result) {
    return result;
  }
  if (auto result = validate_runtime_fingerprint(manifest.runtime_fingerprint);
      !result) {
    return result;
  }

  return validate_manifest_artifacts_v1(manifest.artifacts);
}

} // namespace gravesentinel::ccm
