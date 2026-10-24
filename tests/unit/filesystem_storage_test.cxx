#include "src/storage/filesystem_storage.hxx"

#include "src/manifest/codec.hxx"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("gravesentinel-storage-" + std::to_string(::getpid()) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("could not create temporary storage root");
    }
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

enum class FaultPoint : std::uint8_t {
  open_root,
  open_directory,
  open_regular,
  open_read_write,
  create_file,
  create_directory,
  read,
  write,
  sync_file,
  sync_directory,
  rename,
  list_directory,
  status,
  remove_file,
  remove_directory,
};

struct FaultAction {
  int error{0};
  std::size_t maximum_transfer{0};
};

class FaultInjectingFileOperations final : public IFileOperations {
public:
  explicit FaultInjectingFileOperations(
      std::shared_ptr<IFileOperations> delegate)
      : delegate_(std::move(delegate)) {}

  void arm(FaultPoint point, int error) {
    point_ = point;
    actions_.clear();
    actions_.push_back(FaultAction{.error = error});
  }

  void arm(FaultPoint point, std::initializer_list<FaultAction> actions) {
    point_ = point;
    actions_ = std::deque<FaultAction>{actions};
  }

  void arm(FaultPoint point, std::vector<FaultAction> actions) {
    point_ = point;
    actions_ = std::deque<FaultAction>{actions.begin(), actions.end()};
  }

  [[nodiscard]] FileOperationResult<int>
  open_root_directory(const std::filesystem::path &path) override {
    if (const auto action = take(FaultPoint::open_root);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->open_root_directory(path);
  }

  [[nodiscard]] FileOperationResult<int>
  open_directory_at(int parent_fd, std::string_view name) override {
    if (const auto action = take(FaultPoint::open_directory);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->open_directory_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  open_regular_file_at(int parent_fd, std::string_view name) override {
    if (const auto action = take(FaultPoint::open_regular);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->open_regular_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  open_read_write_file_at(int parent_fd, std::string_view name) override {
    if (const auto action = take(FaultPoint::open_read_write);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->open_read_write_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  create_exclusive_file_at(int parent_fd, std::string_view name,
                           std::uint32_t mode) override {
    if (const auto action = take(FaultPoint::create_file);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->create_exclusive_file_at(parent_fd, name, mode);
  }

  [[nodiscard]] FileOperationResult<bool>
  create_directory_at(int parent_fd, std::string_view name,
                      std::uint32_t mode) override {
    if (const auto action = take(FaultPoint::create_directory);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->create_directory_at(parent_fd, name, mode);
  }

  [[nodiscard]] FileOperationResult<FileStatus> status(int fd) override {
    if (const auto action = take(FaultPoint::status);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->status(fd);
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  read(int fd, std::span<std::byte> buffer) override {
    if (const auto action = take(FaultPoint::read); action.has_value()) {
      if (action->error != 0) {
        return std::unexpected(action->error);
      }
      if (action->maximum_transfer != 0U) {
        buffer =
            buffer.first(std::min(buffer.size(), action->maximum_transfer));
      }
    }
    return delegate_->read(fd, buffer);
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  write(int fd, std::span<const std::byte> buffer) override {
    if (const auto action = take(FaultPoint::write); action.has_value()) {
      if (action->error != 0) {
        return std::unexpected(action->error);
      }
      if (action->maximum_transfer != 0U) {
        buffer =
            buffer.first(std::min(buffer.size(), action->maximum_transfer));
      }
    }
    return delegate_->write(fd, buffer);
  }

  [[nodiscard]] FileOperationResult<void>
  set_permissions(int fd, std::uint32_t permissions) override {
    return delegate_->set_permissions(fd, permissions);
  }

  [[nodiscard]] FileOperationResult<void>
  random_bytes(std::span<std::byte> buffer) override {
    return delegate_->random_bytes(buffer);
  }

  [[nodiscard]] FileOperationResult<void> sync_file(int fd) override {
    if (const auto action = take(FaultPoint::sync_file);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->sync_file(fd);
  }

  [[nodiscard]] FileOperationResult<void> sync_directory(int fd) override {
    if (const auto action = take(FaultPoint::sync_directory);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->sync_directory(fd);
  }

  [[nodiscard]] FileOperationResult<void>
  rename_no_replace_at(int source_directory_fd, std::string_view source_name,
                       int destination_directory_fd,
                       std::string_view destination_name) override {
    if (const auto action = take(FaultPoint::rename);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->rename_no_replace_at(source_directory_fd, source_name,
                                           destination_directory_fd,
                                           destination_name);
  }

  [[nodiscard]] FileOperationResult<void>
  remove_file_at(int parent_fd, std::string_view name) override {
    if (const auto action = take(FaultPoint::remove_file);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->remove_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<void>
  remove_directory_at(int parent_fd, std::string_view name) override {
    if (const auto action = take(FaultPoint::remove_directory);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->remove_directory_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<std::vector<std::string>>
  list_directory(int directory_fd, std::size_t maximum_entries) override {
    if (const auto action = take(FaultPoint::list_directory);
        action.has_value() && action->error != 0) {
      return std::unexpected(action->error);
    }
    return delegate_->list_directory(directory_fd, maximum_entries);
  }

  [[nodiscard]] FileOperationResult<bool> try_lock_exclusive(int fd) override {
    return delegate_->try_lock_exclusive(fd);
  }

  [[nodiscard]] FileOperationResult<void> unlock(int fd) override {
    return delegate_->unlock(fd);
  }

  void close(int fd) noexcept override { delegate_->close(fd); }

private:
  [[nodiscard]] std::optional<FaultAction> take(FaultPoint point) {
    if (!point_.has_value() || *point_ != point || actions_.empty()) {
      return std::nullopt;
    }
    auto action = actions_.front();
    actions_.pop_front();
    if (actions_.empty()) {
      point_.reset();
    }
    return action;
  }

  std::shared_ptr<IFileOperations> delegate_;
  std::optional<FaultPoint> point_;
  std::deque<FaultAction> actions_;
};

[[nodiscard]] ContainerId container_id(std::string value = "container-1") {
  return ContainerId::create(std::move(value)).value();
}

[[nodiscard]] OperationId operation_id(std::uint64_t sequence) {
  return OperationId::create("operation-" + std::to_string(sequence)).value();
}

[[nodiscard]] CheckpointId checkpoint_id(std::uint64_t generation) {
  return CheckpointId::create("checkpoint-" + std::to_string(generation))
      .value();
}

[[nodiscard]] RuntimeFingerprint runtime_fingerprint() {
  return RuntimeFingerprint{
      .runtime_name = "podman",
      .runtime_version = "5.4.2",
      .runtime_build_origin = "test",
      .architecture = "amd64",
      .kernel_release = "test-kernel",
      .criu_version = "4.1",
      .oci_runtime_name = "crun",
      .oci_runtime_version = "1.21",
      .oci_runtime_commit = "test-commit",
      .oci_runtime_features = "test-features",
      .cgroup_version = "v2",
      .cgroup_manager = "systemd",
      .cgroup_controllers = "cpu cpuset memory",
      .privilege_mode = "rootless",
      .user_namespace_mode = "host",
      .capability_profile = "test-capabilities",
      .selinux_mode = "disabled",
      .apparmor_profile = "default",
      .seccomp_profile = "default",
      .command_profile = "test-profile",
  };
}

[[nodiscard]] EncodedManifest encoded_manifest(const ContainerId &container,
                                               std::uint64_t generation) {
  auto encoded = encode_manifest_v1(ManifestV1{
      .container_id = container,
      .checkpoint_id = checkpoint_id(generation),
      .generation = generation,
      .created_at_unix_nanoseconds = generation,
      .source_image_identity = "image@test",
      .runtime_fingerprint = runtime_fingerprint(),
      .artifacts = {},
  });
  if (!encoded) {
    throw std::runtime_error(encoded.error().diagnostic);
  }
  return std::move(*encoded);
}

[[nodiscard]] EncodedManifest
encoded_manifest_with_chunk(const ContainerId &container,
                            std::uint64_t generation, const ChunkHash &hash,
                            std::uint64_t chunk_size) {
  auto path = RelativeSnapshotPath::create("artifact.bin");
  if (!path) {
    throw std::runtime_error(path.error().diagnostic);
  }
  auto encoded = encode_manifest_v1(ManifestV1{
      .container_id = container,
      .checkpoint_id = checkpoint_id(generation),
      .generation = generation,
      .created_at_unix_nanoseconds = generation,
      .source_image_identity = "image@test",
      .runtime_fingerprint = runtime_fingerprint(),
      .artifacts = {ManifestArtifactV1{
          .path = std::move(*path),
          .size = chunk_size,
          .mode = ManifestArtifactV1::posix_regular_file | 0600U,
          .chunks = {ManifestChunkV1{.hash = hash, .size = chunk_size}},
      }},
  });
  if (!encoded) {
    throw std::runtime_error(encoded.error().diagnostic);
  }
  return std::move(*encoded);
}

[[nodiscard]] std::unique_ptr<FilesystemStorage>
open_storage(const std::filesystem::path &root,
             std::shared_ptr<IFileOperations> operations =
                 make_posix_file_operations()) {
  auto storage = FilesystemStorage::open(root, std::move(operations));
  if (!storage) {
    throw std::runtime_error(storage.error().diagnostic);
  }
  return std::move(*storage);
}

void publish_manifest(FilesystemStorage &storage, const ContainerId &container,
                      std::uint64_t generation,
                      std::uint64_t operation_sequence) {
  auto operation =
      storage.begin_operation(container, operation_id(operation_sequence),
                              StorageOperationKind::create_checkpoint);
  if (!operation) {
    throw std::runtime_error(operation.error().diagnostic);
  }
  const auto bytes = encoded_manifest(container, generation);
  auto published =
      (*operation)->publish_manifest(checkpoint_id(generation), bytes);
  if (!published) {
    throw std::runtime_error(published.error().diagnostic);
  }
  auto cleaned = (*operation)->cleanup();
  if (!cleaned) {
    throw std::runtime_error(cleaned.error().diagnostic);
  }
}

[[nodiscard]] ChunkHash test_hash(std::byte value = std::byte{0x42}) {
  ChunkHash::Bytes bytes{};
  bytes.fill(value);
  return ChunkHash{bytes};
}

[[nodiscard]] std::string encoded_hash(const ChunkHash &hash) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(ChunkHash::digest_size * 2U);
  for (const auto byte : hash.bytes()) {
    const auto value = std::to_integer<unsigned int>(byte);
    encoded.push_back(digits[(value >> 4U) & 0x0FU]);
    encoded.push_back(digits[value & 0x0FU]);
  }
  return encoded;
}

[[nodiscard]] std::filesystem::path
publish_orphan_chunk(FilesystemStorage &storage, const ContainerId &container,
                     const ChunkHash &hash, std::span<const std::byte> bytes,
                     std::uint64_t operation_sequence) {
  auto operation =
      storage.begin_operation(container, operation_id(operation_sequence),
                              StorageOperationKind::create_checkpoint);
  if (!operation) {
    throw std::runtime_error(operation.error().diagnostic);
  }
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  if (auto published = (*operation)->publish_chunk(hash, bytes); !published) {
    throw std::runtime_error(published.error().diagnostic);
  }
  if (auto cleaned = (*operation)->cleanup(); !cleaned) {
    throw std::runtime_error(cleaned.error().diagnostic);
  }
  return container_path;
}

void publish_manifest_with_chunk(FilesystemStorage &storage,
                                 const ContainerId &container,
                                 std::uint64_t generation,
                                 std::uint64_t operation_sequence,
                                 const ChunkHash &hash,
                                 std::uint64_t chunk_size) {
  auto operation =
      storage.begin_operation(container, operation_id(operation_sequence),
                              StorageOperationKind::create_checkpoint);
  if (!operation) {
    throw std::runtime_error(operation.error().diagnostic);
  }
  const auto manifest =
      encoded_manifest_with_chunk(container, generation, hash, chunk_size);
  if (auto published =
          (*operation)->publish_manifest(checkpoint_id(generation), manifest);
      !published) {
    throw std::runtime_error(published.error().diagnostic);
  }
  if (auto cleaned = (*operation)->cleanup(); !cleaned) {
    throw std::runtime_error(cleaned.error().diagnostic);
  }
}

[[nodiscard]] GarbageCollectionReport
collect_garbage(FilesystemStorage &storage, const ContainerId &container,
                const GarbageCollectionOptions &options) {
  auto maintenance = storage.begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  if (!maintenance) {
    throw std::runtime_error(maintenance.error().diagnostic);
  }
  auto report = (*maintenance)->collect_garbage(options);
  if (!report) {
    throw std::runtime_error(report.error().diagnostic);
  }
  return *report;
}

void set_file_timestamp(const std::filesystem::path &path,
                        std::int64_t unix_seconds) {
  const std::array<timespec, 2> times{
      timespec{.tv_sec = unix_seconds, .tv_nsec = 0},
      timespec{.tv_sec = unix_seconds, .tv_nsec = 0},
  };
  if (::utimensat(AT_FDCWD, path.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) !=
      0) {
    throw std::runtime_error("could not set deterministic test timestamp");
  }
}

struct PublicationFailureCase {
  FaultPoint point;
  int error;
  StableErrorCode expected_code;
  bool may_be_visible;
};

class FilesystemStoragePublicationFailureTest
    : public testing::TestWithParam<PublicationFailureCase> {};

TEST_P(FilesystemStoragePublicationFailureTest,
       PreservesAllPreviouslyCommittedManifests) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();

  std::vector<EncodedManifest> previous;
  for (std::uint64_t generation = 1; generation <= 3; ++generation) {
    publish_manifest(*storage, container, generation, generation);
    previous.push_back(encoded_manifest(container, generation));
  }

  auto operation = storage->begin_operation(
      container, operation_id(4), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto candidate = encoded_manifest(container, 4);
  const auto test_case = GetParam();
  injected->arm(test_case.point, test_case.error);
  const auto result =
      (*operation)->publish_manifest(checkpoint_id(4), candidate);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, test_case.expected_code);
  EXPECT_EQ(result.error().storage_side_effect,
            test_case.may_be_visible
                ? StorageSideEffect::publication_may_be_visible
                : StorageSideEffect::none);

  const auto listed = storage->list_manifests(container);
  ASSERT_TRUE(listed);
  ASSERT_GE(listed->size(), 3U);
  EXPECT_EQ((*listed)[listed->size() - 3U].generation, 3U);
  EXPECT_EQ((*listed)[listed->size() - 2U].generation, 2U);
  EXPECT_EQ((*listed)[listed->size() - 1U].generation, 1U);
  if (test_case.may_be_visible) {
    ASSERT_EQ(listed->size(), 4U);
    EXPECT_EQ(listed->front().generation, 4U);
    const auto visible = storage->read_manifest(container, 4);
    ASSERT_TRUE(visible);
    EXPECT_EQ(*visible, candidate);
  } else {
    EXPECT_EQ(listed->size(), 3U);
  }

  for (std::uint64_t generation = 1; generation <= 3; ++generation) {
    const auto bytes = storage->read_manifest(container, generation);
    ASSERT_TRUE(bytes);
    EXPECT_EQ(*bytes, previous[static_cast<std::size_t>(generation - 1U)]);
  }
}

INSTANTIATE_TEST_SUITE_P(
    OpenWriteFsyncRenameAndDirectoryFsync,
    FilesystemStoragePublicationFailureTest,
    testing::Values(
        PublicationFailureCase{FaultPoint::create_file, EACCES,
                               StableErrorCode::permission_denied, false},
        PublicationFailureCase{FaultPoint::create_file, ENOSPC,
                               StableErrorCode::resource_exhausted, false},
        PublicationFailureCase{FaultPoint::write, ENOSPC,
                               StableErrorCode::resource_exhausted, false},
        PublicationFailureCase{FaultPoint::sync_file, EIO,
                               StableErrorCode::storage_error, false},
        PublicationFailureCase{FaultPoint::sync_file, ENOSPC,
                               StableErrorCode::resource_exhausted, false},
        PublicationFailureCase{FaultPoint::rename, EACCES,
                               StableErrorCode::permission_denied, false},
        PublicationFailureCase{FaultPoint::sync_directory, EIO,
                               StableErrorCode::storage_error, true},
        PublicationFailureCase{FaultPoint::sync_directory, EACCES,
                               StableErrorCode::permission_denied, true}));

TEST(FilesystemStorageTest, RetriesInterruptedAndPartialReadsAndWrites) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);

  const std::vector<std::byte> chunk{std::byte{0x10}, std::byte{0x20},
                                     std::byte{0x30}, std::byte{0x40}};
  injected->arm(FaultPoint::write, {FaultAction{.error = EINTR},
                                    FaultAction{.maximum_transfer = 2U}});
  ASSERT_TRUE((*operation)->publish_chunk(test_hash(), chunk));

  injected->arm(FaultPoint::read, {FaultAction{.error = EINTR},
                                   FaultAction{.maximum_transfer = 1U}});
  const auto restored = storage->read_chunk(container, test_hash());
  ASSERT_TRUE(restored);
  EXPECT_EQ(*restored, chunk);
}

TEST(FilesystemStorageTest, RetriesInterruptedStatusAndMaintenanceRemove) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  injected->arm(FaultPoint::status, EINTR);
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    publish_manifest(*storage, container, generation, generation);
  }

  auto maintenance =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(maintenance);
  injected->arm(FaultPoint::remove_file, EINTR);
  const auto retained = (*maintenance)->apply_retention(1U);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->manifests_deleted, 3U);
}

TEST(FilesystemStorageTest, StopsRetentionAfterPostUnlinkFsyncFailure) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    publish_manifest(*storage, container, generation, generation);
  }
  auto locator = storage->begin_operation(container, operation_id(5U),
                                          StorageOperationKind::restore);
  ASSERT_TRUE(locator);
  const auto manifests =
      (*locator)->staging_directory().parent_path().parent_path() / "manifests";
  ASSERT_TRUE((*locator)->cleanup());
  locator->reset();

  auto maintenance =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(maintenance);
  injected->arm(
      FaultPoint::sync_directory,
      {FaultAction{}, FaultAction{}, FaultAction{}, FaultAction{.error = EIO}});
  const auto retained = (*maintenance)->apply_retention(1U);
  ASSERT_FALSE(retained);
  EXPECT_TRUE(std::filesystem::exists(manifests / "00000000000000000003.json"));
  EXPECT_TRUE(std::filesystem::exists(manifests / "00000000000000000002.json"));
  EXPECT_FALSE(
      std::filesystem::exists(manifests / "00000000000000000001.json"));
}

TEST(FilesystemStorageTest,
     FailedManifestDeletionSyncBlocksGarbageCollectionUntilConfirmed) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};

  auto operation = storage->begin_operation(
      container, operation_id(1U), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  ASSERT_TRUE((*operation)->publish_chunk(hash, bytes));
  const auto manifest =
      encoded_manifest_with_chunk(container, 1U, hash, bytes.size());
  ASSERT_TRUE((*operation)->publish_manifest(checkpoint_id(1U), manifest));
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();
  publish_manifest(*storage, container, 2U, 2U);

  auto retention =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(retention);
  injected->arm(
      FaultPoint::sync_directory,
      {FaultAction{}, FaultAction{}, FaultAction{}, FaultAction{.error = EIO}});
  const auto retained = (*retention)->apply_retention(1U);
  ASSERT_FALSE(retained);
  const auto chunk_path = container_path / "chunks" /
                          encoded_hash(hash).substr(0U, 2U) /
                          encoded_hash(hash);
  EXPECT_FALSE(std::filesystem::exists(container_path / "manifests" /
                                       "00000000000000000001.json"));
  EXPECT_TRUE(std::filesystem::exists(chunk_path));
  retention->reset();

  auto garbage = storage->begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  ASSERT_TRUE(garbage);
  const GarbageCollectionOptions first_pass{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{100},
      .maximum_deletions = 8U,
  };
  injected->arm(FaultPoint::sync_directory, EIO);
  const auto blocked = (*garbage)->collect_garbage(first_pass);
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error().code, StableErrorCode::storage_error);
  EXPECT_TRUE(std::filesystem::exists(chunk_path));
  const auto candidate = container_path / "gc-candidates" / encoded_hash(hash);
  EXPECT_FALSE(std::filesystem::exists(candidate));

  const auto marked = (*garbage)->collect_garbage(first_pass);
  ASSERT_TRUE(marked);
  EXPECT_EQ(marked->chunks_marked, 1U);
  EXPECT_EQ(marked->chunks_deleted, 0U);
  EXPECT_TRUE(std::filesystem::exists(chunk_path));
  EXPECT_TRUE(std::filesystem::exists(candidate));

  const auto deleted = (*garbage)->collect_garbage(GarbageCollectionOptions{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1100}},
      .grace_period = std::chrono::seconds{100},
      .maximum_deletions = 8U,
  });
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->chunks_deleted, 1U);
  EXPECT_FALSE(std::filesystem::exists(chunk_path));
}

TEST(FilesystemStorageTest,
     RetentionInvalidatesResurrectedCandidateBeforeRemovingLastReference) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  const auto candidate = container_path / "gc-candidates" / encoded_hash(hash);
  const auto backup = root.path() / "candidate-backup";

  const auto marked = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{100}},
          .grace_period = std::chrono::seconds{1000},
          .maximum_deletions = 8U,
      });
  ASSERT_EQ(marked.chunks_marked, 1U);
  ASSERT_TRUE(std::filesystem::copy_file(candidate, backup));
  publish_manifest_with_chunk(*storage, container, 1U, 2U, hash, bytes.size());

  auto garbage = storage->begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  ASSERT_TRUE(garbage);
  injected->arm(
      FaultPoint::sync_directory,
      {FaultAction{}, FaultAction{}, FaultAction{}, FaultAction{.error = EIO}});
  const auto failed_cleanup =
      (*garbage)->collect_garbage(GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{200}},
          .grace_period = std::chrono::seconds{1000},
          .maximum_deletions = 8U,
      });
  ASSERT_FALSE(failed_cleanup);
  EXPECT_FALSE(std::filesystem::exists(candidate));
  garbage->reset();

  ASSERT_TRUE(std::filesystem::copy_file(backup, candidate));
  publish_manifest(*storage, container, 2U, 3U);
  auto retention =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(retention);
  const auto retained = (*retention)->apply_retention(1U);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->manifests_deleted, 1U);
  EXPECT_FALSE(std::filesystem::exists(candidate));
  retention->reset();

  const auto fresh = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(fresh.chunks_marked, 1U);
  EXPECT_EQ(fresh.chunks_deleted, 0U);
  EXPECT_TRUE(std::filesystem::exists(candidate));
  EXPECT_TRUE(storage->contains_chunk(container, hash).value());
}

TEST(FilesystemStorageTest, RetriesInterruptedFileAndDirectoryFsync) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);

  const std::vector<std::byte> chunk{std::byte{0xAA}};
  injected->arm(FaultPoint::sync_file, EINTR);
  ASSERT_TRUE((*operation)->publish_chunk(test_hash(), chunk));

  auto second =
      storage->begin_operation(container_id("other-container"), operation_id(2),
                               StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(second);
  injected->arm(FaultPoint::sync_directory, EINTR);
  ASSERT_TRUE((*second)->publish_chunk(test_hash(std::byte{0x43}), chunk));
}

TEST(FilesystemStorageTest,
     CreatesDedicatedEmptyRuntimeArtifactDirectoryWithStableIdentity) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  auto operation = storage->begin_operation(
      container_id(), operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);

  const auto staging_path = (*operation)->staging_directory();
  const auto runtime_artifact_path = (*operation)->runtime_artifact_directory();
  EXPECT_EQ(runtime_artifact_path, staging_path / "runtime-artifact");
  ASSERT_TRUE(std::filesystem::is_directory(runtime_artifact_path));
  EXPECT_TRUE(std::filesystem::is_empty(runtime_artifact_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(staging_path / "state"));
  EXPECT_FALSE(std::filesystem::exists(runtime_artifact_path / "state"));
  EXPECT_NE((*operation)->runtime_artifact_directory_identity(),
            (*operation)->staging_directory_identity());

  struct stat status{};
  ASSERT_EQ(::stat(runtime_artifact_path.c_str(), &status), 0);
  EXPECT_EQ((*operation)->runtime_artifact_directory_identity().device,
            static_cast<std::uint64_t>(status.st_dev));
  EXPECT_EQ((*operation)->runtime_artifact_directory_identity().inode,
            static_cast<std::uint64_t>(status.st_ino));

  ASSERT_TRUE((*operation)->cleanup());
  EXPECT_FALSE(std::filesystem::exists(runtime_artifact_path));
  EXPECT_FALSE(std::filesystem::exists(staging_path));
}

TEST(FilesystemStorageTest,
     FailedOperationInitializationRemovesNewStagingBestEffort) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();

  auto initial = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(initial);
  const auto staging_parent = (*initial)->staging_directory().parent_path();
  ASSERT_TRUE((*initial)->cleanup());
  initial->reset();

  injected->arm(FaultPoint::create_file, EIO);
  const auto failed = storage->begin_operation(
      container, operation_id(2), StorageOperationKind::create_checkpoint);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, StableErrorCode::storage_error);
  EXPECT_TRUE(std::filesystem::is_empty(staging_parent));

  const auto retry = storage->begin_operation(
      container, operation_id(2), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(retry);
}

TEST(FilesystemStorageTest, EnforcesExclusiveContainerLockAcrossInstances) {
  TemporaryDirectory root;
  auto first = open_storage(root.path());
  auto second = open_storage(root.path());
  const auto container = container_id();

  auto first_operation = first->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(first_operation);
  const auto contended = second->begin_operation(container, operation_id(2),
                                                 StorageOperationKind::restore);
  ASSERT_FALSE(contended);
  EXPECT_EQ(contended.error().code, StableErrorCode::conflict);
  EXPECT_TRUE(contended.error().retryable);

  ASSERT_TRUE((*first_operation)->cleanup());
  const auto contended_after_cleanup = second->begin_operation(
      container, operation_id(2), StorageOperationKind::restore);
  ASSERT_FALSE(contended_after_cleanup);
  EXPECT_EQ(contended_after_cleanup.error().code, StableErrorCode::conflict);
  EXPECT_TRUE(contended_after_cleanup.error().retryable);

  first_operation->reset();
  const auto acquired = second->begin_operation(container, operation_id(2),
                                                StorageOperationKind::restore);
  EXPECT_TRUE(acquired);
}

TEST(FilesystemStorageTest,
     RecoveryRemovesOnlyStaleCanonicalStagingAndPublicationTemps) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();

  auto stale = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(stale);
  const auto stale_path = (*stale)->staging_directory();
  injected->arm(FaultPoint::list_directory, EIO);
  stale->reset();
  ASSERT_TRUE(std::filesystem::exists(stale_path));

  auto fresh = storage->begin_operation(
      container, operation_id(2), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(fresh);
  const auto fresh_path = (*fresh)->staging_directory();
  const auto manifests = fresh_path.parent_path().parent_path() / "manifests";
  injected->arm(FaultPoint::list_directory, EIO);
  fresh->reset();
  ASSERT_TRUE(std::filesystem::exists(fresh_path));

  const auto stale_temporary =
      manifests / ".tmp-00000000000000000000000000000000";
  const auto fresh_temporary =
      manifests / ".tmp-11111111111111111111111111111111";
  {
    std::ofstream stale_file{stale_temporary};
    std::ofstream fresh_file{fresh_temporary};
    ASSERT_TRUE(stale_file.good());
    ASSERT_TRUE(fresh_file.good());
  }
  set_file_timestamp(stale_path, 800);
  set_file_timestamp(fresh_path, 950);
  set_file_timestamp(stale_temporary, 800);
  set_file_timestamp(fresh_temporary, 950);

  const StartupRecoveryOptions options{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{100},
      .maximum_chunk_deletions_per_container = 8U,
  };
  const auto recovered = storage->recover(options);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->containers_scanned, 1U);
  EXPECT_EQ(recovered->staging_entries_deleted, 1U);
  EXPECT_EQ(recovered->temporary_entries_deleted, 1U);
  EXPECT_FALSE(std::filesystem::exists(stale_path));
  EXPECT_TRUE(std::filesystem::exists(fresh_path));
  EXPECT_FALSE(std::filesystem::exists(stale_temporary));
  EXPECT_TRUE(std::filesystem::exists(fresh_temporary));

  const auto repeated = storage->recover(options);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->staging_entries_deleted, 0U);
  EXPECT_EQ(repeated->temporary_entries_deleted, 0U);
  EXPECT_TRUE(std::filesystem::exists(fresh_path));
  EXPECT_TRUE(std::filesystem::exists(fresh_temporary));
}

TEST(FilesystemStorageTest, MapsTraversalIdentifiersToOpaqueNamespaces) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto unsafe_identifier = container_id("../../outside");
  auto operation = storage->begin_operation(
      unsafe_identifier, OperationId::create("../operation").value(),
      StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  EXPECT_TRUE(std::filesystem::is_directory((*operation)->staging_directory()));
  EXPECT_EQ((*operation)->staging_directory().string().find(".."),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root.path().parent_path() / "outside"));
}

TEST(FilesystemStorageTest, RefusesSymlinkedContainerNamespace) {
  TemporaryDirectory root;
  TemporaryDirectory outside;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();

  const auto saved_path = container_path.string() + "-saved";
  std::filesystem::rename(container_path, saved_path);
  std::filesystem::create_directory_symlink(outside.path(), container_path);
  const auto listed = storage->list_manifests(container);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().code, StableErrorCode::storage_error);
  EXPECT_TRUE(std::filesystem::is_empty(outside.path()));
}

TEST(FilesystemStorageTest, ImmediateRetentionKeepsOneTwoOrThreeOfFour) {
  for (std::size_t keep_last = 1U; keep_last <= 3U; ++keep_last) {
    TemporaryDirectory root;
    auto storage = open_storage(root.path());
    const auto container = container_id();
    for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
      publish_manifest(*storage, container, generation, generation);
    }

    auto operation = storage->begin_operation(
        container, operation_id(4), StorageOperationKind::create_checkpoint);
    ASSERT_TRUE(operation);
    const auto premature = (*operation)->apply_retention(keep_last);
    ASSERT_FALSE(premature);
    EXPECT_EQ(premature.error().code, StableErrorCode::conflict);

    const auto manifest = encoded_manifest(container, 4U);
    ASSERT_TRUE((*operation)->publish_manifest(checkpoint_id(4U), manifest));
    const auto retained = (*operation)->apply_retention(keep_last);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->manifests_deleted, 4U - keep_last);
    EXPECT_EQ(retained->manifests_deferred, 0U);
    EXPECT_TRUE(retained->complete);

    const auto listed = storage->list_manifests(container);
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), keep_last);
    for (std::size_t index = 0U; index < keep_last; ++index) {
      EXPECT_EQ((*listed)[index].generation, 4U - index);
    }
    ASSERT_TRUE((*operation)->cleanup());
  }
}

TEST(FilesystemStorageTest,
     MaintenanceUsesContainerLockAndRestorePinDefersDeletion) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  publish_manifest(*storage, container, 1U, 1U);

  auto restore = storage->begin_operation(container, operation_id(2),
                                          StorageOperationKind::restore);
  ASSERT_TRUE(restore);
  const auto staging = (*restore)->staging_directory().parent_path();
  auto pin = (*restore)->pin_manifest(1U);
  ASSERT_TRUE(pin);
  restore->reset();

  const auto blocked =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error().code, StableErrorCode::conflict);

  ASSERT_TRUE((*pin)->release());
  auto maintenance =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(maintenance);
  EXPECT_TRUE(std::filesystem::is_empty(staging));
  const auto wrong_kind =
      (*maintenance)
          ->collect_garbage(GarbageCollectionOptions{
              .now = std::chrono::system_clock::time_point{},
              .grace_period = std::chrono::seconds{0},
              .maximum_deletions = 1U,
          });
  ASSERT_FALSE(wrong_kind);
  EXPECT_EQ(wrong_kind.error().code, StableErrorCode::conflict);
}

TEST(FilesystemStorageTest, StrictRetentionCorruptionCausesZeroDeletion) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    publish_manifest(*storage, container, generation, generation);
  }

  auto locator = storage->begin_operation(container, operation_id(5),
                                          StorageOperationKind::restore);
  ASSERT_TRUE(locator);
  const auto manifests =
      (*locator)->staging_directory().parent_path().parent_path() / "manifests";
  ASSERT_TRUE((*locator)->cleanup());
  locator->reset();
  {
    std::ofstream corrupted{manifests / "00000000000000000002.json",
                            std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(corrupted.good());
    corrupted << "{}";
  }

  auto maintenance =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(maintenance);
  const auto retained = (*maintenance)->apply_retention(1U);
  ASSERT_FALSE(retained);
  EXPECT_EQ(retained.error().code, StableErrorCode::corrupted_snapshot);
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    EXPECT_TRUE(std::filesystem::exists(
        manifests / (generation == 1U   ? "00000000000000000001.json"
                     : generation == 2U ? "00000000000000000002.json"
                     : generation == 3U ? "00000000000000000003.json"
                                        : "00000000000000000004.json")));
  }
}

TEST(FilesystemStorageTest, ListsByGenerationAndRejectsNonMonotonicCommit) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  publish_manifest(*storage, container, 2, 1);
  publish_manifest(*storage, container, 4, 2);

  auto operation = storage->begin_operation(
      container, operation_id(3), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto stale = encoded_manifest(container, 3);
  const auto result = (*operation)->publish_manifest(checkpoint_id(3), stale);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::conflict);

  const auto listed = storage->list_manifests(container);
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2U);
  EXPECT_EQ((*listed)[0].generation, 4U);
  EXPECT_EQ((*listed)[1].generation, 2U);
}

TEST(FilesystemStorageTest, ValidatesManifestBeforeFilesystemPublication) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);

  const std::vector<std::byte> malformed{std::byte{'{'}};
  const auto malformed_result =
      (*operation)->publish_manifest(checkpoint_id(1), malformed);
  ASSERT_FALSE(malformed_result);
  EXPECT_EQ(malformed_result.error().code, StableErrorCode::invalid_argument);

  const auto other = container_id("other-container");
  const auto wrong_identity = encoded_manifest(other, 1);
  const auto mismatch =
      (*operation)->publish_manifest(checkpoint_id(1), wrong_identity);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().code, StableErrorCode::invalid_argument);

  const auto listed = storage->list_manifests(container);
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->empty());
}

TEST(FilesystemStorageTest, KeepsChunksImmutableAndContainerScoped) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto first_container = container_id();
  const auto second_container = container_id("container-2");
  const std::vector<std::byte> original{std::byte{0x01}, std::byte{0x02}};
  const std::vector<std::byte> different{std::byte{0x03}};
  const auto hash = test_hash();

  auto first =
      storage->begin_operation(first_container, operation_id(1),
                               StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(first);
  ASSERT_TRUE((*first)->publish_chunk(hash, original));
  ASSERT_TRUE((*first)->cleanup());
  first->reset();

  auto retry =
      storage->begin_operation(first_container, operation_id(2),
                               StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(retry);
  EXPECT_TRUE((*retry)->publish_chunk(hash, original));
  const auto conflict = (*retry)->publish_chunk(hash, different);
  ASSERT_FALSE(conflict);
  EXPECT_EQ(conflict.error().code, StableErrorCode::corrupted_snapshot);

  auto other =
      storage->begin_operation(second_container, operation_id(3),
                               StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(other);
  EXPECT_TRUE((*other)->publish_chunk(hash, different));
}

TEST(FilesystemStorageTest, GarbageCollectionPreservesSharedLiveChunk) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x10}, std::byte{0x20}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  publish_manifest_with_chunk(*storage, container, 1U, 2U, hash, bytes.size());
  publish_manifest_with_chunk(*storage, container, 2U, 3U, hash, bytes.size());

  auto retention =
      storage->begin_maintenance(container, StorageOperationKind::retention);
  ASSERT_TRUE(retention);
  const auto retained = (*retention)->apply_retention(1U);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->manifests_deleted, 1U);
  retention->reset();

  const auto report = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(report.chunks_marked, 0U);
  EXPECT_EQ(report.chunks_deleted, 0U);
  EXPECT_TRUE(std::filesystem::exists(container_path / "chunks" /
                                      encoded_hash(hash).substr(0, 2) /
                                      encoded_hash(hash)));
}

TEST(FilesystemStorageTest, GarbageCollectionMarksThenHonorsGrace) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  const auto chunk_path = container_path / "chunks" /
                          encoded_hash(hash).substr(0, 2) / encoded_hash(hash);
  set_file_timestamp(chunk_path, 1);
  const auto candidate = container_path / "gc-candidates" / encoded_hash(hash);

  const auto first = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(first.chunks_marked, 1U);
  EXPECT_EQ(first.chunks_deleted, 0U);
  EXPECT_TRUE(std::filesystem::is_regular_file(candidate));
  EXPECT_TRUE(storage->contains_chunk(container, hash).value());

  const auto before_grace = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1099}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(before_grace.chunks_marked, 0U);
  EXPECT_EQ(before_grace.chunks_deleted, 0U);
  EXPECT_TRUE(std::filesystem::exists(candidate));

  const auto after_grace = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1100}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(after_grace.chunks_deleted, 1U);
  EXPECT_EQ(after_grace.candidate_records_deleted, 1U);
  EXPECT_FALSE(after_grace.more_work);
  EXPECT_FALSE(std::filesystem::exists(candidate));
  const auto present = storage->contains_chunk(container, hash);
  ASSERT_TRUE(present);
  EXPECT_FALSE(*present);
}

TEST(FilesystemStorageTest, GarbageCollectionBoundsDeletionAndReportsMoreWork) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto first_hash = test_hash(std::byte{0x42});
  const auto second_hash = test_hash(std::byte{0x43});
  const std::vector<std::byte> bytes{std::byte{0x01}};
  static_cast<void>(
      publish_orphan_chunk(*storage, container, first_hash, bytes, 1U));
  static_cast<void>(
      publish_orphan_chunk(*storage, container, second_hash, bytes, 2U));
  const GarbageCollectionOptions initial{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{10},
      .maximum_deletions = 1U,
  };
  EXPECT_EQ(collect_garbage(*storage, container, initial).chunks_marked, 2U);

  const auto bounded = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1010}},
          .grace_period = std::chrono::seconds{10},
          .maximum_deletions = 1U,
      });
  EXPECT_EQ(bounded.chunks_deleted, 1U);
  EXPECT_TRUE(bounded.more_work);
  const auto completed = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1010}},
          .grace_period = std::chrono::seconds{10},
          .maximum_deletions = 1U,
      });
  EXPECT_EQ(completed.chunks_deleted, 1U);
  EXPECT_FALSE(completed.more_work);
}

TEST(FilesystemStorageTest, GarbageCollectionClearsReReferencedCandidate) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  const auto options = GarbageCollectionOptions{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{100},
      .maximum_deletions = 8U,
  };
  EXPECT_EQ(collect_garbage(*storage, container, options).chunks_marked, 1U);
  const auto candidate = container_path / "gc-candidates" / encoded_hash(hash);
  ASSERT_TRUE(std::filesystem::exists(candidate));

  publish_manifest_with_chunk(*storage, container, 1U, 2U, hash, bytes.size());
  const auto referenced = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1200}},
          .grace_period = std::chrono::seconds{100},
          .maximum_deletions = 8U,
      });
  EXPECT_EQ(referenced.chunks_deleted, 0U);
  EXPECT_EQ(referenced.candidate_records_deleted, 1U);
  EXPECT_FALSE(std::filesystem::exists(candidate));
  EXPECT_TRUE(storage->contains_chunk(container, hash).value());
}

TEST(FilesystemStorageTest, CorruptCandidatePreventsEveryChunkDeletion) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto first_hash = test_hash(std::byte{0x42});
  const auto second_hash = test_hash(std::byte{0x43});
  const std::vector<std::byte> bytes{std::byte{0x01}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, first_hash, bytes, 1U);
  static_cast<void>(
      publish_orphan_chunk(*storage, container, second_hash, bytes, 2U));
  const auto first_pass = collect_garbage(
      *storage, container,
      GarbageCollectionOptions{
          .now =
              std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
          .grace_period = std::chrono::seconds{10},
          .maximum_deletions = 8U,
      });
  ASSERT_EQ(first_pass.chunks_marked, 2U);

  {
    std::ofstream corrupted{container_path / "gc-candidates" /
                                encoded_hash(first_hash),
                            std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(corrupted.good());
    corrupted << "corrupt";
  }
  auto maintenance = storage->begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  ASSERT_TRUE(maintenance);
  const auto collected =
      (*maintenance)
          ->collect_garbage(GarbageCollectionOptions{
              .now = std::chrono::system_clock::time_point{std::chrono::seconds{
                  1010}},
              .grace_period = std::chrono::seconds{10},
              .maximum_deletions = 8U,
          });
  ASSERT_FALSE(collected);
  EXPECT_EQ(collected.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_TRUE(storage->contains_chunk(container, first_hash).value());
  EXPECT_TRUE(storage->contains_chunk(container, second_hash).value());
}

TEST(FilesystemStorageTest, CorruptManifestPreventsGarbageDeletion) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  EXPECT_EQ(collect_garbage(*storage, container,
                            GarbageCollectionOptions{
                                .now =
                                    std::chrono::system_clock::time_point{
                                        std::chrono::seconds{1000}},
                                .grace_period = std::chrono::seconds{10},
                                .maximum_deletions = 8U,
                            })
                .chunks_marked,
            1U);
  publish_manifest(*storage, container, 1U, 2U);
  {
    std::ofstream corrupted{container_path / "manifests" /
                                "00000000000000000001.json",
                            std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(corrupted.good());
    corrupted << "{}";
  }

  auto maintenance = storage->begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  ASSERT_TRUE(maintenance);
  const auto collected =
      (*maintenance)
          ->collect_garbage(GarbageCollectionOptions{
              .now = std::chrono::system_clock::time_point{std::chrono::seconds{
                  1010}},
              .grace_period = std::chrono::seconds{10},
              .maximum_deletions = 8U,
          });
  ASSERT_FALSE(collected);
  EXPECT_TRUE(storage->contains_chunk(container, hash).value());
}

TEST(FilesystemStorageTest,
     UnknownManifestRootEntryPreventsMatureChunkDeletion) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}};
  const auto container_path =
      publish_orphan_chunk(*storage, container, hash, bytes, 1U);
  EXPECT_EQ(collect_garbage(*storage, container,
                            GarbageCollectionOptions{
                                .now =
                                    std::chrono::system_clock::time_point{
                                        std::chrono::seconds{100}},
                                .grace_period = std::chrono::seconds{10},
                                .maximum_deletions = 8U,
                            })
                .chunks_marked,
            1U);
  {
    std::ofstream unknown{container_path / "manifests" / "unknown-root"};
    ASSERT_TRUE(unknown.good());
    unknown << "uncertain";
  }

  auto maintenance = storage->begin_maintenance(
      container, StorageOperationKind::garbage_collection);
  ASSERT_TRUE(maintenance);
  const auto collected =
      (*maintenance)
          ->collect_garbage(GarbageCollectionOptions{
              .now = std::chrono::system_clock::time_point{std::chrono::seconds{
                  110}},
              .grace_period = std::chrono::seconds{10},
              .maximum_deletions = 8U,
          });
  ASSERT_FALSE(collected);
  EXPECT_EQ(collected.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_TRUE(storage->contains_chunk(container, hash).value());
}

TEST(FilesystemStorageTest,
     InvalidReferencedChunkPreventsOtherMatureChunkDeletion) {
  for (const bool remove_referenced_chunk : {false, true}) {
    TemporaryDirectory root;
    auto storage = open_storage(root.path());
    const auto container = container_id();
    const auto orphan_hash = test_hash(std::byte{0x42});
    const auto referenced_hash = test_hash(std::byte{0x43});
    const std::vector<std::byte> orphan_bytes{std::byte{0x01}};
    const std::vector<std::byte> referenced_bytes{std::byte{0x02},
                                                  std::byte{0x03}};
    const auto container_path = publish_orphan_chunk(
        *storage, container, orphan_hash, orphan_bytes, 1U);
    EXPECT_EQ(collect_garbage(*storage, container,
                              GarbageCollectionOptions{
                                  .now =
                                      std::chrono::system_clock::time_point{
                                          std::chrono::seconds{100}},
                                  .grace_period = std::chrono::seconds{10},
                                  .maximum_deletions = 8U,
                              })
                  .chunks_marked,
              1U);
    static_cast<void>(publish_orphan_chunk(*storage, container, referenced_hash,
                                           referenced_bytes, 2U));
    publish_manifest_with_chunk(*storage, container, 1U, 3U, referenced_hash,
                                referenced_bytes.size());

    const auto referenced_path = container_path / "chunks" /
                                 encoded_hash(referenced_hash).substr(0U, 2U) /
                                 encoded_hash(referenced_hash);
    if (remove_referenced_chunk) {
      ASSERT_TRUE(std::filesystem::remove(referenced_path));
    } else {
      std::ofstream truncated{referenced_path,
                              std::ios::binary | std::ios::trunc};
      ASSERT_TRUE(truncated.good());
      truncated.put('x');
    }

    auto maintenance = storage->begin_maintenance(
        container, StorageOperationKind::garbage_collection);
    ASSERT_TRUE(maintenance);
    const auto collected = (*maintenance)
                               ->collect_garbage(GarbageCollectionOptions{
                                   .now =
                                       std::chrono::system_clock::time_point{
                                           std::chrono::seconds{110}},
                                   .grace_period = std::chrono::seconds{10},
                                   .maximum_deletions = 8U,
                               });
    ASSERT_FALSE(collected);
    EXPECT_EQ(collected.error().code, StableErrorCode::corrupted_snapshot);
    EXPECT_TRUE(storage->contains_chunk(container, orphan_hash).value());
  }
}

TEST(FilesystemStorageTest, RecoveryDefersBusyContainerAndRunsIdempotentGc) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  const auto hash = test_hash();
  const std::vector<std::byte> bytes{std::byte{0x01}};
  static_cast<void>(publish_orphan_chunk(*storage, container, hash, bytes, 1U));

  auto active = storage->begin_operation(container, operation_id(2),
                                         StorageOperationKind::restore);
  ASSERT_TRUE(active);
  const StartupRecoveryOptions first_options{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{10},
      .maximum_chunk_deletions_per_container = 8U,
  };
  const auto deferred = storage->recover(first_options);
  ASSERT_TRUE(deferred);
  EXPECT_EQ(deferred->containers_deferred, 1U);
  EXPECT_EQ(deferred->containers_scanned, 0U);
  EXPECT_TRUE(deferred->more_work);
  ASSERT_TRUE((*active)->cleanup());
  active->reset();

  const auto marked = storage->recover(first_options);
  ASSERT_TRUE(marked);
  EXPECT_EQ(marked->chunks_marked, 1U);
  EXPECT_EQ(marked->chunks_deleted, 0U);
  const auto deleted = storage->recover(StartupRecoveryOptions{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1010}},
      .grace_period = std::chrono::seconds{10},
      .maximum_chunk_deletions_per_container = 8U,
  });
  ASSERT_TRUE(deleted);
  EXPECT_EQ(deleted->chunks_deleted, 1U);
  const auto repeated = storage->recover(StartupRecoveryOptions{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1010}},
      .grace_period = std::chrono::seconds{10},
      .maximum_chunk_deletions_per_container = 8U,
  });
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->chunks_marked, 0U);
  EXPECT_EQ(repeated->chunks_deleted, 0U);
}

TEST(FilesystemStorageTest, PinMetadataBlocksCleanupUntilReleased) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  publish_manifest(*storage, container, 1, 1);
  auto operation = storage->begin_operation(container, operation_id(2),
                                            StorageOperationKind::restore);
  ASSERT_TRUE(operation);
  const auto staging_path = (*operation)->staging_directory();

  auto pin = (*operation)->pin_manifest(1);
  ASSERT_TRUE(pin);
  EXPECT_FALSE((*operation)->cleanup());
  EXPECT_TRUE(std::filesystem::exists(staging_path));
  ASSERT_TRUE((*pin)->release());
  ASSERT_TRUE((*operation)->cleanup());
  EXPECT_FALSE(std::filesystem::exists(staging_path));
}

TEST(FilesystemStorageTest,
     VisibleChunkRequiresSuccessfulDirectorySyncBeforeReuse) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const std::vector<std::byte> bytes{std::byte{0x01}, std::byte{0x02}};

  injected->arm(FaultPoint::sync_directory,
                {FaultAction{}, FaultAction{.error = EIO}});
  const auto first = (*operation)->publish_chunk(test_hash(), bytes);
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);

  injected->arm(FaultPoint::sync_directory,
                {FaultAction{}, FaultAction{.error = EIO}});
  const auto retry = (*operation)->publish_chunk(test_hash(), bytes);
  ASSERT_FALSE(retry);
  EXPECT_EQ(retry.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  EXPECT_TRUE((*operation)->publish_chunk(test_hash(), bytes));
}

TEST(FilesystemStorageTest,
     VisibleManifestRequiresSuccessfulDirectorySyncBeforeIdempotentSuccess) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  const auto manifest = encoded_manifest(container, 1);
  auto first = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(first);

  injected->arm(FaultPoint::sync_directory, EIO);
  const auto published = (*first)->publish_manifest(checkpoint_id(1), manifest);
  ASSERT_FALSE(published);
  EXPECT_EQ(published.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  ASSERT_TRUE((*first)->cleanup());
  first->reset();

  auto retry = storage->begin_operation(
      container, operation_id(2), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(retry);
  injected->arm(FaultPoint::sync_directory, EIO);
  const auto unconfirmed =
      (*retry)->publish_manifest(checkpoint_id(1), manifest);
  ASSERT_FALSE(unconfirmed);
  EXPECT_EQ(unconfirmed.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  EXPECT_TRUE((*retry)->publish_manifest(checkpoint_id(1), manifest));
}

TEST(FilesystemStorageTest, ExistingDirectoriesAreResyncedBeforeUse) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  auto storage = open_storage(root.path(), injected);
  storage.reset();

  injected->arm(FaultPoint::sync_directory, EIO);
  const auto reopened = FilesystemStorage::open(root.path(), injected);
  ASSERT_FALSE(reopened);
  EXPECT_EQ(reopened.error().code, StableErrorCode::storage_error);
}

TEST(FilesystemStorageTest, StaleSequentialTemporaryNamesCannotBlockPublish) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  const auto manifests_path = container_path / "manifests";
  for (std::size_t index = 0; index < 128U; ++index) {
    std::ofstream stale{manifests_path / (".tmp-" + std::to_string(index))};
    ASSERT_TRUE(stale.good());
  }

  const auto manifest = encoded_manifest(container, 1);
  EXPECT_TRUE((*operation)->publish_manifest(checkpoint_id(1), manifest));
}

TEST(FilesystemStorageTest,
     PublishesIdentityOnlyAfterEveryRequiredDirectoryIsDurable) {
  struct InitializationFailure {
    FaultPoint point;
    std::size_t successful_actions;
  };
  const std::array failures{
      InitializationFailure{FaultPoint::create_directory, 1U},
      InitializationFailure{FaultPoint::create_directory, 2U},
      InitializationFailure{FaultPoint::create_directory, 3U},
      InitializationFailure{FaultPoint::sync_directory, 2U},
      InitializationFailure{FaultPoint::sync_directory, 3U},
      InitializationFailure{FaultPoint::sync_directory, 4U},
  };

  for (std::size_t index = 0U; index < failures.size(); ++index) {
    TemporaryDirectory root;
    auto injected = std::make_shared<FaultInjectingFileOperations>(
        make_posix_file_operations());
    auto storage = open_storage(root.path(), injected);
    const auto container =
        container_id("identity-order-" + std::to_string(index));

    std::vector<FaultAction> actions(failures[index].successful_actions);
    actions.push_back(FaultAction{.error = EIO});
    injected->arm(failures[index].point, std::move(actions));
    const auto failed = storage->begin_operation(
        container, operation_id(1U), StorageOperationKind::create_checkpoint);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, StableErrorCode::storage_error);

    const auto containers = root.path() / "containers";
    std::vector<std::filesystem::path> namespaces;
    for (const auto &entry : std::filesystem::directory_iterator{containers}) {
      namespaces.push_back(entry.path());
    }
    ASSERT_EQ(namespaces.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(namespaces.front() / "identity"));

    auto retry = storage->begin_operation(
        container, operation_id(2U), StorageOperationKind::create_checkpoint);
    ASSERT_TRUE(retry);
    const auto container_path =
        (*retry)->staging_directory().parent_path().parent_path();
    EXPECT_TRUE(std::filesystem::is_regular_file(container_path / "identity"));
    EXPECT_TRUE(std::filesystem::is_directory(container_path / "manifests"));
    EXPECT_TRUE(std::filesystem::is_directory(container_path / "chunks"));
    EXPECT_TRUE(std::filesystem::is_directory(container_path / "staging"));
  }
}

TEST(FilesystemStorageTest,
     CompletesCrashLikePartialContainerBeforePublishingIdentity) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto initial = storage->begin_operation(
      container, operation_id(1U), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(initial);
  const auto container_path =
      (*initial)->staging_directory().parent_path().parent_path();
  ASSERT_TRUE((*initial)->cleanup());
  initial->reset();

  ASSERT_GT(std::filesystem::remove_all(container_path), 0U);
  ASSERT_TRUE(std::filesystem::create_directory(container_path));
  ASSERT_TRUE(std::filesystem::create_directory(container_path / "manifests"));
  {
    std::ofstream marker{container_path / "partial-marker"};
    ASSERT_TRUE(marker.good());
    marker << "preserve";
  }
  ASSERT_FALSE(std::filesystem::exists(container_path / "identity"));
  ASSERT_FALSE(std::filesystem::exists(container_path / "chunks"));
  ASSERT_FALSE(std::filesystem::exists(container_path / "staging"));

  const auto recovered = storage->recover(StartupRecoveryOptions{
      .now = std::chrono::system_clock::time_point{std::chrono::seconds{1000}},
      .grace_period = std::chrono::seconds{100},
      .maximum_chunk_deletions_per_container = 8U,
  });
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->containers_scanned, 0U);
  EXPECT_TRUE(std::filesystem::exists(container_path / "partial-marker"));

  auto completed = storage->begin_operation(
      container, operation_id(2U), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(completed);
  EXPECT_TRUE(std::filesystem::is_regular_file(container_path / "identity"));
  EXPECT_TRUE(std::filesystem::is_directory(container_path / "manifests"));
  EXPECT_TRUE(std::filesystem::is_directory(container_path / "chunks"));
  EXPECT_TRUE(std::filesystem::is_directory(container_path / "staging"));
  EXPECT_TRUE(std::filesystem::exists(container_path / "partial-marker"));
}

TEST(FilesystemStorageTest, MissingRequiredNamespaceIsCorruptionNotAbsence) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();
  ASSERT_TRUE(std::filesystem::remove(container_path / "manifests"));

  const auto listed = storage->list_manifests(container);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().code, StableErrorCode::corrupted_snapshot);

  const auto operation_after_corruption = storage->begin_operation(
      container, operation_id(2U), StorageOperationKind::create_checkpoint);
  ASSERT_FALSE(operation_after_corruption);
  EXPECT_EQ(operation_after_corruption.error().code,
            StableErrorCode::corrupted_snapshot);
  EXPECT_FALSE(std::filesystem::exists(container_path / "manifests"));
}

TEST(FilesystemStorageTest, CorruptedOldManifestDoesNotBlockNewGeneration) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  publish_manifest(*storage, container, 1, 1);
  auto operation = storage->begin_operation(
      container, operation_id(2), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();

  const auto first_manifest =
      container_path / "manifests" / "00000000000000000001.json";
  {
    std::ofstream corrupted{first_manifest, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(corrupted.good());
    corrupted << "{}";
  }
  publish_manifest(*storage, container, 2, 3);

  const auto listed = storage->list_manifests(container);
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2U);
  EXPECT_EQ((*listed)[0].generation, 2U);
  const auto corrupted = storage->read_manifest(container, 1);
  ASSERT_FALSE(corrupted);
  EXPECT_EQ(corrupted.error().code, StableErrorCode::corrupted_snapshot);
}

TEST(FilesystemStorageTest, ManifestBodyMustMatchStorageGeneration) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  publish_manifest(*storage, container, 1, 1);
  auto operation = storage->begin_operation(container, operation_id(2),
                                            StorageOperationKind::restore);
  ASSERT_TRUE(operation);
  const auto manifests =
      (*operation)->staging_directory().parent_path().parent_path() /
      "manifests";
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();
  std::filesystem::rename(manifests / "00000000000000000001.json",
                          manifests / "00000000000000000002.json");

  const auto read = storage->read_manifest(container, 2);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().code, StableErrorCode::corrupted_snapshot);
}

TEST(FilesystemStorageTest, RejectsFifoBeforeOpeningItForReading) {
  TemporaryDirectory root;
  auto storage = open_storage(root.path());
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  const auto container_path =
      (*operation)->staging_directory().parent_path().parent_path();
  const std::vector<std::byte> bytes{std::byte{0x01}};
  ASSERT_TRUE((*operation)->publish_chunk(test_hash(), bytes));
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();

  std::string chunk_name;
  for (std::size_t index = 0; index < ChunkHash::digest_size; ++index) {
    chunk_name.append("42");
  }
  const auto chunk_path = container_path / "chunks" / "42" / chunk_name;
  ASSERT_TRUE(std::filesystem::remove(chunk_path));
  ASSERT_EQ(::mkfifo(chunk_path.c_str(), 0600), 0);
  const auto read = storage->read_chunk(container, test_hash());
  ASSERT_FALSE(read);
  EXPECT_EQ(read.error().code, StableErrorCode::storage_error);
}

TEST(FilesystemStorageTest, InjectsFailuresAndEintrAtOpenBoundaries) {
  TemporaryDirectory root;
  auto injected = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  injected->arm(FaultPoint::open_root, EINTR);
  auto storage = open_storage(root.path(), injected);
  const auto container = container_id();
  auto operation = storage->begin_operation(
      container, operation_id(1), StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(operation);
  ASSERT_TRUE((*operation)->cleanup());
  operation->reset();

  injected->arm(FaultPoint::open_directory, EACCES);
  auto listed = storage->list_manifests(container);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().code, StableErrorCode::permission_denied);

  injected->arm(FaultPoint::open_regular, EACCES);
  listed = storage->list_manifests(container);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().code, StableErrorCode::permission_denied);

  injected->arm(FaultPoint::open_read_write, EACCES);
  const auto begin = storage->begin_operation(container, operation_id(2),
                                              StorageOperationKind::restore);
  ASSERT_FALSE(begin);
  EXPECT_EQ(begin.error().code, StableErrorCode::permission_denied);

  auto denied = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  denied->arm(FaultPoint::open_root, EACCES);
  const auto unopened = FilesystemStorage::open(root.path(), denied);
  ASSERT_FALSE(unopened);
  EXPECT_EQ(unopened.error().code, StableErrorCode::permission_denied);
}

} // namespace
} // namespace gravesentinel::ccm
