#include "src/chunking/snapshot_chunker.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

constexpr std::uint32_t kDirectoryPermissions = 0700U;
constexpr std::uint32_t kInitialFilePermissions = 0600U;
constexpr std::size_t kMaximumSnapshotDirectoryEntries =
    ManifestLimits::maximum_artifacts + ManifestLimits::maximum_directories;
constexpr std::string_view kReassembledDirectoryName = "snapshot";

[[nodiscard]] bool valid_output_directory_name(std::string_view name) noexcept {
  if (name.empty() ||
      name.size() > ManifestLimits::maximum_path_component_bytes ||
      name == "." || name == "..") {
    return false;
  }
  return std::ranges::all_of(name, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_';
  });
}

[[nodiscard]] CheckpointErrorDetail logical_error(StableErrorCode code,
                                                  std::string diagnostic,
                                                  bool retryable = false) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
  };
}

[[nodiscard]] StableErrorCode code_for_errno(int error,
                                             StableErrorCode fallback) {
  switch (error) {
  case EACCES:
  case EPERM:
  case EROFS:
    return StableErrorCode::permission_denied;
  case ENOSPC:
#ifdef EDQUOT
  case EDQUOT:
#endif
    return StableErrorCode::resource_exhausted;
  default:
    return fallback;
  }
}

[[nodiscard]] CheckpointErrorDetail filesystem_error(std::string_view operation,
                                                     int error,
                                                     StableErrorCode fallback) {
  return logical_error(
      code_for_errno(error, fallback),
      std::string{operation} + ": " +
          std::error_code(error, std::generic_category()).message(),
      error == EAGAIN || error == EBUSY || error == EIO);
}

class UniqueFileDescriptor final {
public:
  UniqueFileDescriptor() = default;
  UniqueFileDescriptor(std::shared_ptr<IFileOperations> operations, int value)
      : operations_(std::move(operations)), value_(value) {}

  ~UniqueFileDescriptor() { reset(); }

  UniqueFileDescriptor(const UniqueFileDescriptor &) = delete;
  UniqueFileDescriptor &operator=(const UniqueFileDescriptor &) = delete;

  UniqueFileDescriptor(UniqueFileDescriptor &&other) noexcept
      : operations_(std::move(other.operations_)),
        value_(std::exchange(other.value_, -1)) {}

  UniqueFileDescriptor &operator=(UniqueFileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      operations_ = std::move(other.operations_);
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }

  void reset() noexcept {
    if (value_ >= 0) {
      operations_->close(value_);
      value_ = -1;
    }
  }

private:
  std::shared_ptr<IFileOperations> operations_;
  int value_{-1};
};

[[nodiscard]] FileOperationResult<UniqueFileDescriptor>
open_root(const std::shared_ptr<IFileOperations> &operations,
          const std::filesystem::path &path) {
  while (true) {
    auto opened = operations->open_root_directory(path);
    if (opened) {
      return UniqueFileDescriptor{operations, *opened};
    }
    if (opened.error() != EINTR) {
      return std::unexpected(opened.error());
    }
  }
}

[[nodiscard]] FileOperationResult<UniqueFileDescriptor>
open_directory(const std::shared_ptr<IFileOperations> &operations,
               int parent_fd, std::string_view name) {
  while (true) {
    auto opened = operations->open_directory_at(parent_fd, name);
    if (opened) {
      return UniqueFileDescriptor{operations, *opened};
    }
    if (opened.error() != EINTR) {
      return std::unexpected(opened.error());
    }
  }
}

[[nodiscard]] FileOperationResult<UniqueFileDescriptor>
open_regular_file(const std::shared_ptr<IFileOperations> &operations,
                  int parent_fd, std::string_view name) {
  while (true) {
    auto opened = operations->open_regular_file_at(parent_fd, name);
    if (opened) {
      return UniqueFileDescriptor{operations, *opened};
    }
    if (opened.error() != EINTR) {
      return std::unexpected(opened.error());
    }
  }
}

[[nodiscard]] FileOperationResult<UniqueFileDescriptor>
create_regular_file(const std::shared_ptr<IFileOperations> &operations,
                    int parent_fd, std::string_view name) {
  while (true) {
    auto opened = operations->create_exclusive_file_at(parent_fd, name,
                                                       kInitialFilePermissions);
    if (opened) {
      return UniqueFileDescriptor{operations, *opened};
    }
    if (opened.error() != EINTR) {
      return std::unexpected(opened.error());
    }
  }
}

[[nodiscard]] FileOperationResult<FileStatus>
file_status(const std::shared_ptr<IFileOperations> &operations, int fd) {
  while (true) {
    auto status = operations->status(fd);
    if (status || status.error() != EINTR) {
      return status;
    }
  }
}

[[nodiscard]] FileOperationResult<std::vector<std::string>>
list_directory(const std::shared_ptr<IFileOperations> &operations, int fd) {
  while (true) {
    auto entries =
        operations->list_directory(fd, kMaximumSnapshotDirectoryEntries);
    if (entries || entries.error() != EINTR) {
      return entries;
    }
  }
}

[[nodiscard]] FileOperationResult<bool>
create_directory(const std::shared_ptr<IFileOperations> &operations,
                 int parent_fd, std::string_view name) {
  while (true) {
    auto created =
        operations->create_directory_at(parent_fd, name, kDirectoryPermissions);
    if (created || created.error() != EINTR) {
      return created;
    }
  }
}

[[nodiscard]] CheckpointResult<void>
write_all(const std::shared_ptr<IFileOperations> &operations, int fd,
          std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto written = operations->write(fd, bytes.subspan(offset));
    if (!written) {
      if (written.error() == EINTR) {
        continue;
      }
      return std::unexpected(filesystem_error("write reassembled artifact",
                                              written.error(),
                                              StableErrorCode::storage_error));
    }
    if (*written == 0U || *written > bytes.size() - offset) {
      return std::unexpected(logical_error(
          StableErrorCode::storage_error,
          "file operations returned an invalid write byte count"));
    }
    offset += *written;
  }
  return {};
}

[[nodiscard]] CheckpointResult<void>
set_permissions(const std::shared_ptr<IFileOperations> &operations, int fd,
                std::uint32_t permissions) {
  while (true) {
    auto changed = operations->set_permissions(fd, permissions);
    if (changed) {
      return {};
    }
    if (changed.error() != EINTR) {
      return std::unexpected(
          filesystem_error("set reassembled artifact permissions",
                           changed.error(), StableErrorCode::storage_error));
    }
  }
}

[[nodiscard]] CheckpointResult<void>
sync_file(const std::shared_ptr<IFileOperations> &operations, int fd) {
  while (true) {
    auto synced = operations->sync_file(fd);
    if (synced) {
      return {};
    }
    if (synced.error() != EINTR) {
      return std::unexpected(filesystem_error("sync reassembled artifact",
                                              synced.error(),
                                              StableErrorCode::storage_error));
    }
  }
}

void remove_file_best_effort(const std::shared_ptr<IFileOperations> &operations,
                             int parent_fd, std::string_view name) noexcept {
  while (true) {
    auto removed = operations->remove_file_at(parent_fd, name);
    if (removed || removed.error() != EINTR) {
      return;
    }
  }
}

[[nodiscard]] bool same_stable_status(const FileStatus &left,
                                      const FileStatus &right) noexcept {
  return left == right;
}

[[nodiscard]] CheckpointResult<std::uint64_t>
required_mount_id(const FileStatus &status, std::string_view description) {
  if (!status.mount_id.has_value()) {
    return std::unexpected(logical_error(
        StableErrorCode::unavailable,
        std::string{description} +
            " requires Linux statx mount identifiers for containment"));
  }
  return *status.mount_id;
}

[[nodiscard]] CheckpointResult<void>
validate_mount(const FileStatus &status, std::uint64_t root_mount_id,
               std::string_view description, StableErrorCode error_code) {
  auto mount_id = required_mount_id(status, description);
  if (!mount_id) {
    return std::unexpected(mount_id.error());
  }
  if (*mount_id != root_mount_id) {
    return std::unexpected(
        logical_error(error_code, std::string{description} +
                                      " crosses the snapshot root mount"));
  }
  return {};
}

using DirectoryIdentity =
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;

class SnapshotIngestor final {
public:
  SnapshotIngestor(IStorageOperation &operation, const IHasher &hasher,
                   std::shared_ptr<IFileOperations> file_operations,
                   std::uint64_t root_mount_id)
      : operation_(operation), hasher_(hasher),
        file_operations_(std::move(file_operations)),
        root_mount_id_(root_mount_id) {}

  [[nodiscard]] CheckpointResult<std::vector<ManifestArtifactV1>>
  run(int root_fd, const FileStatus &root_status) {
    if (auto visited = visit_directory(root_fd, root_status, "", 0U);
        !visited) {
      return std::unexpected(visited.error());
    }
    if (auto verified = verify_snapshot_tree(root_fd, "", 0U); !verified) {
      return std::unexpected(verified.error());
    }

    std::ranges::sort(artifacts_, {}, [](const ManifestArtifactV1 &artifact) {
      return artifact.path.value();
    });
    if (auto validated = validate_manifest_artifacts_v1(artifacts_);
        !validated) {
      return std::unexpected(
          logical_error(StableErrorCode::internal,
                        "chunker produced invalid manifest artifacts: " +
                            validated.error().diagnostic));
    }
    return std::move(artifacts_);
  }

private:
  [[nodiscard]] CheckpointResult<void>
  validate_source_status(const FileStatus &status, FileType expected_type,
                         std::string_view description) const {
    if (status.type != expected_type) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          std::string{description} + " has an unsupported file type"));
    }
    return validate_mount(status, root_mount_id_, description,
                          StableErrorCode::capture_failed);
  }

  [[nodiscard]] CheckpointResult<void>
  verify_unchanged(int fd, const FileStatus &before,
                   std::string_view description) const {
    auto after = file_status(file_operations_, fd);
    if (!after) {
      return std::unexpected(filesystem_error(description, after.error(),
                                              StableErrorCode::capture_failed));
    }
    if (!same_stable_status(before, *after)) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          std::string{description} + " changed while it was ingested", true));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  verify_directory_entry(int parent_fd, std::string_view name,
                         const FileStatus &expected,
                         std::string_view relative_path) const {
    auto reopened = open_directory(file_operations_, parent_fd, name);
    if (!reopened) {
      return std::unexpected(filesystem_error(
          "reopen snapshot directory " + std::string{relative_path},
          reopened.error(), StableErrorCode::capture_failed));
    }
    auto status = file_status(file_operations_, reopened->get());
    if (!status) {
      return std::unexpected(filesystem_error(
          "inspect snapshot directory " + std::string{relative_path},
          status.error(), StableErrorCode::capture_failed));
    }
    if (!same_stable_status(expected, *status)) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          "snapshot directory entry changed while it was ingested", true));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  visit_directory(int directory_fd, const FileStatus &before,
                  std::string relative_path, std::size_t depth) {
    if (auto valid = validate_source_status(
            before, FileType::directory,
            relative_path.empty() ? "snapshot root" : "snapshot directory");
        !valid) {
      return valid;
    }
    if (depth >= ManifestLimits::maximum_path_components &&
        !relative_path.empty()) {
      return std::unexpected(
          logical_error(StableErrorCode::capture_failed,
                        "snapshot tree exceeds the supported directory depth"));
    }
    if (directory_count_ >= ManifestLimits::maximum_directories) {
      return std::unexpected(
          logical_error(StableErrorCode::resource_exhausted,
                        "snapshot tree contains too many directories"));
    }
    ++directory_count_;

    const DirectoryIdentity identity{*before.mount_id, before.device,
                                     before.inode};
    if (!visited_directories_.insert(identity).second) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          "snapshot tree contains a repeated directory identity"));
    }
    recorded_statuses_.emplace(relative_path, before);

    auto entries = list_directory(file_operations_, directory_fd);
    if (!entries) {
      return std::unexpected(filesystem_error("list snapshot directory",
                                              entries.error(),
                                              StableErrorCode::capture_failed));
    }
    std::ranges::sort(*entries);
    recorded_directory_entries_.emplace(relative_path, *entries);

    for (const auto &name : *entries) {
      const auto path_text =
          relative_path.empty() ? name : relative_path + "/" + name;
      auto path = RelativeSnapshotPath::create(path_text);
      if (!path) {
        return std::unexpected(
            logical_error(StableErrorCode::capture_failed,
                          "snapshot tree contains an unsafe relative path"));
      }

      auto directory = open_directory(file_operations_, directory_fd, name);
      if (directory) {
        auto child_before = file_status(file_operations_, directory->get());
        if (!child_before) {
          return std::unexpected(filesystem_error(
              "inspect snapshot directory", child_before.error(),
              StableErrorCode::capture_failed));
        }
        if (auto visited = visit_directory(directory->get(), *child_before,
                                           path_text, depth + 1U);
            !visited) {
          return visited;
        }
        if (auto verified = verify_directory_entry(directory_fd, name,
                                                   *child_before, path_text);
            !verified) {
          return verified;
        }
        continue;
      }
      if (directory.error() != ENOTDIR) {
        return std::unexpected(filesystem_error(
            "open snapshot entry as a directory", directory.error(),
            StableErrorCode::capture_failed));
      }

      auto file = open_regular_file(file_operations_, directory_fd, name);
      if (!file) {
        return std::unexpected(
            logical_error(StableErrorCode::capture_failed,
                          "snapshot tree contains a symlink, special file, or "
                          "unstable entry"));
      }
      if (auto ingested =
              ingest_file(directory_fd, name, std::move(*path), file->get());
          !ingested) {
        return ingested;
      }
    }

    return verify_unchanged(directory_fd, before,
                            relative_path.empty() ? "snapshot root"
                                                  : "snapshot directory");
  }

  [[nodiscard]] CheckpointResult<void>
  verify_snapshot_tree(int directory_fd, const std::string &relative_path,
                       std::size_t depth) const {
    const auto expected_status = recorded_statuses_.find(relative_path);
    const auto expected_entries =
        recorded_directory_entries_.find(relative_path);
    if (expected_status == recorded_statuses_.end() ||
        expected_entries == recorded_directory_entries_.end()) {
      return std::unexpected(logical_error(
          StableErrorCode::internal,
          "snapshot verification is missing recorded directory metadata"));
    }
    if (auto unchanged = verify_unchanged(
            directory_fd, expected_status->second,
            relative_path.empty() ? "snapshot root" : "snapshot directory");
        !unchanged) {
      return unchanged;
    }

    auto entries = list_directory(file_operations_, directory_fd);
    if (!entries) {
      return std::unexpected(filesystem_error("relist snapshot directory",
                                              entries.error(),
                                              StableErrorCode::capture_failed));
    }
    std::ranges::sort(*entries);
    if (*entries != expected_entries->second) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          "snapshot directory entries changed during ingest", true));
    }

    for (const auto &name : *entries) {
      const auto path_text =
          relative_path.empty() ? name : relative_path + "/" + name;
      const auto expected = recorded_statuses_.find(path_text);
      if (expected == recorded_statuses_.end()) {
        return std::unexpected(logical_error(
            StableErrorCode::capture_failed,
            "snapshot entry was not present during the original traversal",
            true));
      }

      if (expected->second.type == FileType::directory) {
        if (depth + 1U >= ManifestLimits::maximum_path_components) {
          return std::unexpected(logical_error(
              StableErrorCode::capture_failed,
              "snapshot tree exceeds the supported directory depth"));
        }
        auto directory = open_directory(file_operations_, directory_fd, name);
        if (!directory) {
          return std::unexpected(logical_error(
              StableErrorCode::capture_failed,
              "snapshot directory changed before final verification", true));
        }
        if (auto verified =
                verify_snapshot_tree(directory->get(), path_text, depth + 1U);
            !verified) {
          return verified;
        }
        continue;
      }

      auto file = open_regular_file(file_operations_, directory_fd, name);
      if (!file) {
        return std::unexpected(logical_error(
            StableErrorCode::capture_failed,
            "snapshot artifact changed before final verification", true));
      }
      auto status = file_status(file_operations_, file->get());
      if (!status) {
        return std::unexpected(filesystem_error(
            "inspect snapshot artifact during final verification",
            status.error(), StableErrorCode::capture_failed));
      }
      if (status->type != FileType::regular_file || status->link_count != 1U ||
          !same_stable_status(expected->second, *status)) {
        return std::unexpected(logical_error(
            StableErrorCode::capture_failed,
            "snapshot artifact changed before final verification", true));
      }
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  read_exact_chunk(int fd, std::span<std::byte> bytes) const {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      auto read = file_operations_->read(fd, bytes.subspan(offset));
      if (!read) {
        if (read.error() == EINTR) {
          continue;
        }
        return std::unexpected(
            filesystem_error("read snapshot artifact", read.error(),
                             StableErrorCode::capture_failed));
      }
      if (*read == 0U) {
        return std::unexpected(logical_error(
            StableErrorCode::capture_failed,
            "snapshot artifact ended before its recorded size", true));
      }
      if (*read > bytes.size() - offset) {
        return std::unexpected(logical_error(
            StableErrorCode::internal,
            "file operations returned an invalid read byte count"));
      }
      offset += *read;
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void> verify_eof(int fd) const {
    std::array<std::byte, 1> extra{};
    while (true) {
      auto read = file_operations_->read(fd, extra);
      if (!read) {
        if (read.error() == EINTR) {
          continue;
        }
        return std::unexpected(
            filesystem_error("check snapshot artifact length", read.error(),
                             StableErrorCode::capture_failed));
      }
      if (*read != 0U) {
        return std::unexpected(logical_error(
            StableErrorCode::capture_failed,
            "snapshot artifact grew while it was ingested", true));
      }
      return {};
    }
  }

  [[nodiscard]] CheckpointResult<void>
  store_chunk(std::span<const std::byte> bytes,
              std::vector<ManifestChunkV1> &chunks) {
    auto hash = hasher_.hash(bytes);
    if (!hash) {
      return std::unexpected(hash.error());
    }

    auto existing = operation_.read_chunk(*hash);
    if (existing) {
      if (existing->size() != bytes.size()) {
        return std::unexpected(logical_error(
            StableErrorCode::corrupted_snapshot,
            "existing immutable chunk failed size, hash, or byte validation"));
      }
      auto existing_hash = hasher_.hash(*existing);
      if (!existing_hash) {
        return std::unexpected(existing_hash.error());
      }
      if (*existing_hash != *hash || !std::ranges::equal(*existing, bytes)) {
        return std::unexpected(logical_error(
            StableErrorCode::corrupted_snapshot,
            "existing immutable chunk failed size, hash, or byte validation"));
      }
    } else if (existing.error().code == StableErrorCode::not_found) {
      if (auto published = operation_.publish_chunk(*hash, bytes); !published) {
        return published;
      }
    } else if (existing.error().code == StableErrorCode::resource_exhausted) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "existing immutable chunk exceeds the chunk size limit"));
    } else {
      return std::unexpected(existing.error());
    }

    chunks.push_back(ManifestChunkV1{
        .hash = *hash,
        .size = static_cast<std::uint64_t>(bytes.size()),
    });
    return {};
  }

  [[nodiscard]] CheckpointResult<void> ingest_file(int parent_fd,
                                                   std::string_view name,
                                                   RelativeSnapshotPath path,
                                                   int fd) {
    auto before = file_status(file_operations_, fd);
    if (!before) {
      return std::unexpected(filesystem_error("inspect snapshot artifact",
                                              before.error(),
                                              StableErrorCode::capture_failed));
    }
    if (auto valid = validate_source_status(*before, FileType::regular_file,
                                            "snapshot artifact");
        !valid) {
      return valid;
    }
    if (before->link_count != 1U) {
      return std::unexpected(
          logical_error(StableErrorCode::capture_failed,
                        "snapshot artifact hardlinks are not supported"));
    }

    const auto chunk_count_u64 =
        before->size / ManifestLimits::chunk_bytes +
        (before->size % ManifestLimits::chunk_bytes == 0U ? 0U : 1U);
    if (chunk_count_u64 > ManifestLimits::maximum_chunks ||
        chunk_count_ > ManifestLimits::maximum_chunks -
                           static_cast<std::size_t>(chunk_count_u64)) {
      return std::unexpected(
          logical_error(StableErrorCode::resource_exhausted,
                        "snapshot contains too many chunks"));
    }
    if (artifacts_.size() >= ManifestLimits::maximum_artifacts) {
      return std::unexpected(
          logical_error(StableErrorCode::resource_exhausted,
                        "snapshot contains too many artifacts"));
    }
    if (before->size > ManifestLimits::maximum_snapshot_bytes - total_size_) {
      return std::unexpected(
          logical_error(StableErrorCode::resource_exhausted,
                        "snapshot exceeds the total artifact byte limit"));
    }

    ManifestArtifactV1 artifact{
        .path = std::move(path),
        .size = before->size,
        .mode = before->mode & (ManifestArtifactV1::posix_file_type_mask |
                                ManifestArtifactV1::posix_permission_bits),
        .chunks = {},
    };
    artifact.chunks.reserve(static_cast<std::size_t>(chunk_count_u64));

    std::vector<std::byte> buffer;
    std::uint64_t remaining = before->size;
    while (remaining != 0U) {
      const auto next_size = static_cast<std::size_t>(
          std::min(remaining, ManifestLimits::chunk_bytes));
      buffer.resize(next_size);
      if (auto read = read_exact_chunk(fd, buffer); !read) {
        return read;
      }
      if (auto stored = store_chunk(buffer, artifact.chunks); !stored) {
        return stored;
      }
      remaining -= static_cast<std::uint64_t>(next_size);
    }
    if (auto eof = verify_eof(fd); !eof) {
      return eof;
    }
    if (auto unchanged = verify_unchanged(fd, *before, "snapshot artifact");
        !unchanged) {
      return unchanged;
    }

    auto reopened = open_regular_file(file_operations_, parent_fd, name);
    if (!reopened) {
      return std::unexpected(filesystem_error("reopen snapshot artifact",
                                              reopened.error(),
                                              StableErrorCode::capture_failed));
    }
    auto rebound = file_status(file_operations_, reopened->get());
    if (!rebound) {
      return std::unexpected(
          filesystem_error("inspect reopened snapshot artifact",
                           rebound.error(), StableErrorCode::capture_failed));
    }
    if (!same_stable_status(*before, *rebound)) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          "snapshot artifact entry changed while it was ingested", true));
    }

    total_size_ += before->size;
    chunk_count_ += static_cast<std::size_t>(chunk_count_u64);
    recorded_statuses_.emplace(artifact.path.value(), *before);
    artifacts_.push_back(std::move(artifact));
    return {};
  }

  IStorageOperation &operation_;
  const IHasher &hasher_;
  std::shared_ptr<IFileOperations> file_operations_;
  std::uint64_t root_mount_id_;
  std::set<DirectoryIdentity> visited_directories_;
  std::map<std::string, FileStatus> recorded_statuses_;
  std::map<std::string, std::vector<std::string>> recorded_directory_entries_;
  std::vector<ManifestArtifactV1> artifacts_;
  std::uint64_t total_size_{0};
  std::size_t chunk_count_{0};
  std::size_t directory_count_{0};
};

[[nodiscard]] std::vector<std::string_view>
path_components(std::string_view path) {
  std::vector<std::string_view> components;
  std::size_t begin = 0;
  while (begin < path.size()) {
    const auto separator = path.find('/', begin);
    const auto end =
        separator == std::string_view::npos ? path.size() : separator;
    components.push_back(path.substr(begin, end - begin));
    if (separator == std::string_view::npos) {
      break;
    }
    begin = separator + 1U;
  }
  return components;
}

[[nodiscard]] CheckpointErrorDetail
corrupted_chunk_error(const CheckpointErrorDetail &error) {
  if (error.code == StableErrorCode::not_found ||
      error.code == StableErrorCode::resource_exhausted) {
    return logical_error(StableErrorCode::corrupted_snapshot,
                         "manifest references a missing or oversized chunk");
  }
  return error;
}

class SnapshotAssembly final {
public:
  SnapshotAssembly(IStorageOperation &operation, const IHasher &hasher,
                   std::shared_ptr<IFileOperations> file_operations,
                   std::uint64_t output_mount_id, int output_root_fd)
      : operation_(operation), hasher_(hasher),
        file_operations_(std::move(file_operations)),
        output_mount_id_(output_mount_id), output_root_fd_(output_root_fd) {}

  [[nodiscard]] CheckpointResult<void>
  run(std::span<const ManifestArtifactV1> artifacts) {
    std::vector<const ManifestArtifactV1 *> ordered;
    ordered.reserve(artifacts.size());
    for (const auto &artifact : artifacts) {
      ordered.push_back(&artifact);
    }
    std::ranges::sort(ordered, {}, [](const ManifestArtifactV1 *artifact) {
      return artifact->path.value();
    });

    for (const auto *artifact : ordered) {
      if (auto result = assemble_artifact(*artifact); !result) {
        return result;
      }
    }
    return {};
  }

private:
  struct ArtifactParent {
    UniqueFileDescriptor descriptor;
    int fd;
    std::string_view filename;
  };

  [[nodiscard]] CheckpointResult<ArtifactParent>
  open_artifact_parent(std::string_view path) {
    const auto components = path_components(path);
    ArtifactParent parent{
        .descriptor = {},
        .fd = output_root_fd_,
        .filename = components.back(),
    };

    std::string prefix;
    for (std::size_t index = 0; index + 1U < components.size(); ++index) {
      if (!prefix.empty()) {
        prefix.push_back('/');
      }
      prefix.append(components[index]);

      auto created =
          create_directory(file_operations_, parent.fd, components[index]);
      if (!created) {
        return std::unexpected(
            filesystem_error("create reassembly directory", created.error(),
                             StableErrorCode::storage_error));
      }
      if (!*created && !created_directories_.contains(prefix)) {
        return std::unexpected(logical_error(
            StableErrorCode::conflict,
            "reassembly output contains an unexpected existing directory"));
      }
      created_directories_.insert(prefix);

      auto opened =
          open_directory(file_operations_, parent.fd, components[index]);
      if (!opened) {
        return std::unexpected(
            filesystem_error("open reassembly directory", opened.error(),
                             StableErrorCode::storage_error));
      }
      auto status = file_status(file_operations_, opened->get());
      if (!status) {
        return std::unexpected(
            filesystem_error("inspect reassembly directory", status.error(),
                             StableErrorCode::storage_error));
      }
      if (status->type != FileType::directory) {
        return std::unexpected(
            logical_error(StableErrorCode::conflict,
                          "reassembly path component is not a directory"));
      }
      if (auto contained =
              validate_mount(*status, output_mount_id_, "reassembly directory",
                             StableErrorCode::storage_error);
          !contained) {
        return std::unexpected(contained.error());
      }
      parent.descriptor = std::move(*opened);
      parent.fd = parent.descriptor.get();
    }
    return parent;
  }

  [[nodiscard]] CheckpointResult<void>
  assemble_artifact(const ManifestArtifactV1 &artifact) {
    auto parent = open_artifact_parent(artifact.path.value());
    if (!parent) {
      return std::unexpected(parent.error());
    }
    auto file =
        create_regular_file(file_operations_, parent->fd, parent->filename);
    if (!file) {
      return std::unexpected(filesystem_error(
          "create reassembled artifact", file.error(),
          file.error() == EEXIST ? StableErrorCode::conflict
                                 : StableErrorCode::storage_error));
    }

    auto assembled = [&]() -> CheckpointResult<void> {
      auto initial = file_status(file_operations_, file->get());
      if (!initial) {
        return std::unexpected(
            filesystem_error("inspect new reassembled artifact",
                             initial.error(), StableErrorCode::storage_error));
      }
      if (initial->type != FileType::regular_file || initial->size != 0U ||
          initial->link_count != 1U) {
        return std::unexpected(logical_error(
            StableErrorCode::storage_error,
            "new reassembled artifact failed regular-file validation"));
      }
      if (auto contained =
              validate_mount(*initial, output_mount_id_, "reassembled artifact",
                             StableErrorCode::storage_error);
          !contained) {
        return contained;
      }

      std::uint64_t written_bytes = 0;
      for (const auto &chunk : artifact.chunks) {
        auto bytes = operation_.read_chunk(chunk.hash);
        if (!bytes) {
          return std::unexpected(corrupted_chunk_error(bytes.error()));
        }
        if (bytes->size() != chunk.size) {
          return std::unexpected(logical_error(
              StableErrorCode::corrupted_snapshot,
              "stored chunk failed manifest size or BLAKE3 validation"));
        }
        auto hash = hasher_.hash(*bytes);
        if (!hash) {
          return std::unexpected(hash.error());
        }
        if (*hash != chunk.hash) {
          return std::unexpected(logical_error(
              StableErrorCode::corrupted_snapshot,
              "stored chunk failed manifest size or BLAKE3 validation"));
        }
        if (auto written = write_all(file_operations_, file->get(), *bytes);
            !written) {
          return written;
        }
        written_bytes += chunk.size;
      }
      if (written_bytes != artifact.size) {
        return std::unexpected(logical_error(
            StableErrorCode::corrupted_snapshot,
            "reassembled artifact size differs from its manifest"));
      }

      const auto permissions =
          artifact.mode & ManifestArtifactV1::posix_permission_bits;
      if (auto changed =
              set_permissions(file_operations_, file->get(), permissions);
          !changed) {
        return changed;
      }
      if (auto synced = sync_file(file_operations_, file->get()); !synced) {
        return synced;
      }

      auto final_status = file_status(file_operations_, file->get());
      if (!final_status) {
        return std::unexpected(filesystem_error(
            "inspect completed reassembled artifact", final_status.error(),
            StableErrorCode::storage_error));
      }
      constexpr auto relevant_mode_bits =
          ManifestArtifactV1::posix_file_type_mask |
          ManifestArtifactV1::posix_permission_bits;
      if (final_status->type != FileType::regular_file ||
          final_status->link_count != 1U ||
          final_status->size != artifact.size ||
          (final_status->mode & relevant_mode_bits) != artifact.mode) {
        return std::unexpected(logical_error(
            StableErrorCode::storage_error,
            "completed reassembled artifact failed size or mode validation"));
      }
      return validate_mount(*final_status, output_mount_id_,
                            "completed reassembled artifact",
                            StableErrorCode::storage_error);
    }();

    if (!assembled) {
      file->reset();
      remove_file_best_effort(file_operations_, parent->fd, parent->filename);
      return assembled;
    }
    return {};
  }

  IStorageOperation &operation_;
  const IHasher &hasher_;
  std::shared_ptr<IFileOperations> file_operations_;
  std::uint64_t output_mount_id_;
  int output_root_fd_;
  std::unordered_set<std::string> created_directories_;
};

} // namespace

SnapshotChunker::SnapshotChunker(const IHasher &hasher)
    : SnapshotChunker(hasher, make_posix_file_operations()) {}

SnapshotChunker::SnapshotChunker(
    const IHasher &hasher, std::shared_ptr<IFileOperations> file_operations)
    : hasher_(hasher), file_operations_(std::move(file_operations)) {}

CheckpointResult<std::vector<ManifestArtifactV1>>
SnapshotChunker::ingest(IStorageOperation &operation,
                        const std::filesystem::path &snapshot_root) {
  if (operation.kind() != StorageOperationKind::create_checkpoint) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "snapshot ingest requires a create-checkpoint storage operation"));
  }
  if (!file_operations_) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "snapshot ingest requires file operations"));
  }
  if (snapshot_root.empty() || !snapshot_root.is_absolute() ||
      snapshot_root.native().find('\0') !=
          std::filesystem::path::string_type::npos) {
    return std::unexpected(logical_error(
        StableErrorCode::invalid_argument,
        "snapshot root must be an absolute path without NUL bytes"));
  }

  auto root = open_root(file_operations_, snapshot_root);
  if (!root) {
    return std::unexpected(filesystem_error("open snapshot root", root.error(),
                                            StableErrorCode::capture_failed));
  }
  auto root_status = file_status(file_operations_, root->get());
  if (!root_status) {
    return std::unexpected(filesystem_error("inspect snapshot root",
                                            root_status.error(),
                                            StableErrorCode::capture_failed));
  }
  if (root_status->type != FileType::directory) {
    return std::unexpected(logical_error(StableErrorCode::capture_failed,
                                         "snapshot root is not a directory"));
  }
  if (snapshot_root.lexically_normal() ==
      operation.runtime_artifact_directory().lexically_normal()) {
    const auto expected_identity =
        operation.runtime_artifact_directory_identity();
    if (root_status->device != expected_identity.device ||
        root_status->inode != expected_identity.inode) {
      return std::unexpected(logical_error(
          StableErrorCode::capture_failed,
          "runtime artifact directory identity changed before ingest", true));
    }
  }
  auto mount_id = required_mount_id(*root_status, "snapshot root");
  if (!mount_id) {
    return std::unexpected(mount_id.error());
  }

  SnapshotIngestor ingestor{operation, hasher_, file_operations_, *mount_id};
  auto artifacts = ingestor.run(root->get(), *root_status);
  if (!artifacts) {
    return std::unexpected(artifacts.error());
  }

  auto rebound_root = open_root(file_operations_, snapshot_root);
  if (!rebound_root) {
    return std::unexpected(
        logical_error(StableErrorCode::capture_failed,
                      "snapshot root path changed during ingest", true));
  }
  auto rebound_status = file_status(file_operations_, rebound_root->get());
  if (!rebound_status || !same_stable_status(*root_status, *rebound_status)) {
    return std::unexpected(
        logical_error(StableErrorCode::capture_failed,
                      "snapshot root identity changed during ingest", true));
  }
  return artifacts;
}

SnapshotReassembler::SnapshotReassembler(IStorageOperation &operation,
                                         const IHasher &hasher)
    : SnapshotReassembler(operation, hasher, make_posix_file_operations()) {}

SnapshotReassembler::SnapshotReassembler(
    IStorageOperation &operation, const IHasher &hasher,
    std::shared_ptr<IFileOperations> file_operations)
    : operation_(operation), hasher_(hasher),
      file_operations_(std::move(file_operations)) {}

CheckpointResult<std::filesystem::path>
SnapshotReassembler::reassemble(std::span<const ManifestArtifactV1> artifacts) {
  return reassemble(artifacts, kReassembledDirectoryName);
}

CheckpointResult<std::filesystem::path>
SnapshotReassembler::reassemble(std::span<const ManifestArtifactV1> artifacts,
                                std::string_view output_directory_name) {
  if (operation_.kind() != StorageOperationKind::restore) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "snapshot reassembly requires a restore storage operation"));
  }
  if (!file_operations_) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "snapshot reassembly requires file operations"));
  }
  if (!valid_output_directory_name(output_directory_name)) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "snapshot reassembly output directory name is invalid"));
  }
  if (auto validated = validate_manifest_artifacts_v1(artifacts); !validated) {
    return std::unexpected(
        logical_error(StableErrorCode::corrupted_snapshot,
                      "manifest artifact layout is invalid: " +
                          validated.error().diagnostic));
  }

  const auto &staging_path = operation_.staging_directory();
  if (staging_path.empty() || !staging_path.is_absolute() ||
      staging_path.native().find('\0') !=
          std::filesystem::path::string_type::npos) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      "storage operation returned an invalid staging path"));
  }
  auto staging = open_root(file_operations_, staging_path);
  if (!staging) {
    return std::unexpected(filesystem_error("open operation staging directory",
                                            staging.error(),
                                            StableErrorCode::storage_error));
  }
  auto staging_status = file_status(file_operations_, staging->get());
  if (!staging_status) {
    return std::unexpected(filesystem_error(
        "inspect operation staging directory", staging_status.error(),
        StableErrorCode::storage_error));
  }
  const auto expected_staging_identity =
      operation_.staging_directory_identity();
  if (staging_status->type != FileType::directory ||
      staging_status->device != expected_staging_identity.device ||
      staging_status->inode != expected_staging_identity.inode) {
    return std::unexpected(logical_error(
        StableErrorCode::storage_error,
        "operation staging path is not bound to the storage operation"));
  }
  auto mount_id = required_mount_id(*staging_status, "operation staging");
  if (!mount_id) {
    return std::unexpected(mount_id.error());
  }

  const std::string output_directory{output_directory_name};
  auto created =
      create_directory(file_operations_, staging->get(), output_directory);
  if (!created) {
    return std::unexpected(
        filesystem_error("create reassembled snapshot directory",
                         created.error(), StableErrorCode::storage_error));
  }
  if (!*created) {
    return std::unexpected(
        logical_error(StableErrorCode::conflict,
                      "reassembled snapshot directory already exists"));
  }
  auto output =
      open_directory(file_operations_, staging->get(), output_directory);
  if (!output) {
    return std::unexpected(
        filesystem_error("open reassembled snapshot directory", output.error(),
                         StableErrorCode::storage_error));
  }
  auto output_status = file_status(file_operations_, output->get());
  if (!output_status) {
    return std::unexpected(filesystem_error(
        "inspect reassembled snapshot directory", output_status.error(),
        StableErrorCode::storage_error));
  }
  if (output_status->type != FileType::directory) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      "reassembled snapshot root is not a directory"));
  }
  if (auto contained =
          validate_mount(*output_status, *mount_id, "reassembled snapshot root",
                         StableErrorCode::storage_error);
      !contained) {
    return std::unexpected(contained.error());
  }

  SnapshotAssembly assembly{operation_, hasher_, file_operations_, *mount_id,
                            output->get()};
  if (auto assembled = assembly.run(artifacts); !assembled) {
    return std::unexpected(assembled.error());
  }

  auto rebound_staging = open_root(file_operations_, staging_path);
  if (!rebound_staging) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      "operation staging path changed during reassembly"));
  }
  auto rebound_staging_status =
      file_status(file_operations_, rebound_staging->get());
  if (!rebound_staging_status ||
      rebound_staging_status->type != FileType::directory ||
      rebound_staging_status->device != expected_staging_identity.device ||
      rebound_staging_status->inode != expected_staging_identity.inode) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      "operation staging identity changed during reassembly"));
  }
  auto rebound_output = open_directory(file_operations_, rebound_staging->get(),
                                       output_directory);
  if (!rebound_output) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      "reassembled snapshot path changed before publication"));
  }
  auto rebound_output_status =
      file_status(file_operations_, rebound_output->get());
  if (!rebound_output_status ||
      rebound_output_status->type != FileType::directory ||
      rebound_output_status->device != output_status->device ||
      rebound_output_status->inode != output_status->inode) {
    return std::unexpected(logical_error(
        StableErrorCode::storage_error,
        "reassembled snapshot identity changed before publication"));
  }
  return staging_path / output_directory;
}

} // namespace gravesentinel::ccm
