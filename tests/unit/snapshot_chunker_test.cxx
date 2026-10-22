#include "src/chunking/snapshot_chunker.hxx"

#include "src/core/blake3_hasher.hxx"
#include "src/storage/filesystem_storage.hxx"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] CheckpointErrorDetail test_error(StableErrorCode code,
                                               std::string diagnostic) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
  };
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("gravesentinel-chunking-" + std::to_string(::getpid()) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("could not create temporary test directory");
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

[[nodiscard]] ContainerId container_id(std::string value = "container-1") {
  return ContainerId::create(std::move(value)).value();
}

[[nodiscard]] OperationId operation_id(std::string value) {
  return OperationId::create(std::move(value)).value();
}

[[nodiscard]] std::vector<std::byte> bytes(std::size_t size,
                                           std::uint8_t seed) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result[index] = static_cast<std::byte>(
        (static_cast<std::uint64_t>(seed) + index * 31U) & 0xFFU);
  }
  return result;
}

void write_bytes(const std::filesystem::path &path,
                 std::span<const std::byte> contents) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  ASSERT_TRUE(output.good());
  output.write(reinterpret_cast<const char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
  ASSERT_TRUE(output.good());
}

void append_bytes(const std::filesystem::path &path,
                  std::span<const std::byte> contents) {
  std::ofstream output{path, std::ios::binary | std::ios::app};
  ASSERT_TRUE(output.good());
  output.write(reinterpret_cast<const char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
  ASSERT_TRUE(output.good());
}

[[nodiscard]] std::vector<std::byte>
read_bytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input.good()) {
    throw std::runtime_error("could not open test artifact");
  }
  const auto end = input.tellg();
  if (end < 0) {
    throw std::runtime_error("could not determine test artifact size");
  }
  std::vector<std::byte> result(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(result.data()),
             static_cast<std::streamsize>(result.size()));
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("could not read test artifact");
  }
  return result;
}

[[nodiscard]] std::uint32_t
file_permissions(const std::filesystem::path &path) {
  struct stat attributes{};
  if (::stat(path.c_str(), &attributes) != 0) {
    throw std::runtime_error("could not stat test artifact");
  }
  return static_cast<std::uint32_t>(attributes.st_mode) &
         ManifestArtifactV1::posix_permission_bits;
}

class FakeStorage;

class FakeStorageOperation final : public IStorageOperation {
public:
  FakeStorageOperation(FakeStorage &storage, ContainerId container,
                       StorageOperationKind kind,
                       std::filesystem::path staging_directory)
      : storage_(storage), container_(std::move(container)), kind_(kind),
        staging_directory_(std::move(staging_directory)) {}

  [[nodiscard]] const ContainerId &container_id() const noexcept override {
    return container_;
  }

  [[nodiscard]] StorageOperationKind kind() const noexcept override {
    return kind_;
  }

  [[nodiscard]] const std::filesystem::path &
  staging_directory() const noexcept override {
    return staging_directory_;
  }

  [[nodiscard]] StorageDirectoryIdentity
  staging_directory_identity() const noexcept override {
    struct stat attributes{};
    if (::stat(staging_directory_.c_str(), &attributes) != 0) {
      return {};
    }
    return StorageDirectoryIdentity{
        .device = static_cast<std::uint64_t>(attributes.st_dev),
        .inode = static_cast<std::uint64_t>(attributes.st_ino),
    };
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ChunkHash &hash) override;

  [[nodiscard]] CheckpointResult<void>
  publish_chunk(const ChunkHash &hash,
                std::span<const std::byte> chunk_bytes) override;

  [[nodiscard]] CheckpointResult<void>
  publish_manifest(const CheckpointId &, std::span<const std::byte>) override {
    return std::unexpected(test_error(StableErrorCode::internal,
                                      "manifest publication is not expected"));
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStoragePin>>
  pin_manifest(std::uint64_t) override {
    return std::unexpected(
        test_error(StableErrorCode::not_found, "manifest is not present"));
  }

  [[nodiscard]] CheckpointResult<void> cleanup() override {
    cleaned_ = true;
    return {};
  }

private:
  FakeStorage &storage_;
  ContainerId container_;
  StorageOperationKind kind_;
  std::filesystem::path staging_directory_;
  bool cleaned_{false};
};

class FakeStorage final : public IStorage {
public:
  using Namespace = std::map<ChunkHash, std::vector<std::byte>>;

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &, const OperationId &,
                  StorageOperationKind) override {
    return std::unexpected(test_error(
        StableErrorCode::internal, "begin_operation is not used by this fake"));
  }

  [[nodiscard]] CheckpointResult<bool>
  contains_chunk(const ContainerId &container,
                 const ChunkHash &hash) const override {
    const auto found_namespace = chunks_.find(container.value());
    return found_namespace != chunks_.end() &&
           found_namespace->second.contains(hash);
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container,
             const ChunkHash &hash) const override {
    const auto found_namespace = chunks_.find(container.value());
    if (found_namespace == chunks_.end()) {
      return std::unexpected(
          test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    const auto found_chunk = found_namespace->second.find(hash);
    if (found_chunk == found_namespace->second.end()) {
      return std::unexpected(
          test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    return found_chunk->second;
  }

  [[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &) const override {
    return std::vector<StoredManifest>{};
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &, std::uint64_t) const override {
    return std::unexpected(
        test_error(StableErrorCode::not_found, "manifest is absent"));
  }

  [[nodiscard]] CheckpointResult<void>
  publish(const ContainerId &container, const ChunkHash &hash,
          std::span<const std::byte> chunk_bytes) {
    auto &container_chunks = chunks_[container.value()];
    const auto found = container_chunks.find(hash);
    if (found != container_chunks.end()) {
      if (std::ranges::equal(found->second, chunk_bytes)) {
        return {};
      }
      return std::unexpected(
          test_error(StableErrorCode::corrupted_snapshot,
                     "existing fake chunk differs from publication"));
    }
    container_chunks.emplace(
        hash, std::vector<std::byte>{chunk_bytes.begin(), chunk_bytes.end()});
    ++publication_count_;
    maximum_publication_size_ =
        std::max(maximum_publication_size_, chunk_bytes.size());
    return {};
  }

  [[nodiscard]] std::size_t publication_count() const noexcept {
    return publication_count_;
  }

  [[nodiscard]] std::size_t maximum_publication_size() const noexcept {
    return maximum_publication_size_;
  }

  [[nodiscard]] std::size_t namespace_size(const ContainerId &container) const {
    const auto found = chunks_.find(container.value());
    return found == chunks_.end() ? 0U : found->second.size();
  }

  void corrupt(const ContainerId &container, const ChunkHash &hash,
               std::vector<std::byte> replacement) {
    chunks_.at(container.value()).at(hash) = std::move(replacement);
  }

private:
  std::map<std::string, Namespace> chunks_;
  std::size_t publication_count_{0};
  std::size_t maximum_publication_size_{0};
};

CheckpointResult<std::vector<std::byte>>
FakeStorageOperation::read_chunk(const ChunkHash &hash) {
  return storage_.read_chunk(container_, hash);
}

CheckpointResult<void>
FakeStorageOperation::publish_chunk(const ChunkHash &hash,
                                    std::span<const std::byte> chunk_bytes) {
  return storage_.publish(container_, hash, chunk_bytes);
}

struct IoAction {
  int error{0};
  std::size_t maximum_transfer{0};
};

class FaultInjectingFileOperations final : public IFileOperations {
public:
  explicit FaultInjectingFileOperations(
      std::shared_ptr<IFileOperations> delegate)
      : delegate_(std::move(delegate)) {}

  void arm_reads(std::initializer_list<IoAction> actions) {
    read_actions_ = std::deque<IoAction>{actions};
  }

  void arm_writes(std::initializer_list<IoAction> actions) {
    write_actions_ = std::deque<IoAction>{actions};
  }

  void after_successful_reads(std::size_t count,
                              std::function<void()> callback) {
    reads_before_callback_ = count;
    after_read_ = std::move(callback);
  }

  void after_next_successful_read(std::function<void()> callback) {
    after_successful_reads(1U, std::move(callback));
  }

  [[nodiscard]] FileOperationResult<int>
  open_root_directory(const std::filesystem::path &path) override {
    return delegate_->open_root_directory(path);
  }

  [[nodiscard]] FileOperationResult<int>
  open_directory_at(int parent_fd, std::string_view name) override {
    return delegate_->open_directory_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  open_regular_file_at(int parent_fd, std::string_view name) override {
    return delegate_->open_regular_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  open_read_write_file_at(int parent_fd, std::string_view name) override {
    return delegate_->open_read_write_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<int>
  create_exclusive_file_at(int parent_fd, std::string_view name,
                           std::uint32_t mode) override {
    return delegate_->create_exclusive_file_at(parent_fd, name, mode);
  }

  [[nodiscard]] FileOperationResult<bool>
  create_directory_at(int parent_fd, std::string_view name,
                      std::uint32_t mode) override {
    return delegate_->create_directory_at(parent_fd, name, mode);
  }

  [[nodiscard]] FileOperationResult<FileStatus> status(int fd) override {
    return delegate_->status(fd);
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  read(int fd, std::span<std::byte> buffer) override {
    if (!read_actions_.empty()) {
      const auto action = read_actions_.front();
      read_actions_.pop_front();
      if (action.error != 0) {
        return std::unexpected(action.error);
      }
      if (action.maximum_transfer != 0U) {
        buffer = buffer.first(std::min(buffer.size(), action.maximum_transfer));
      }
    }
    auto result = delegate_->read(fd, buffer);
    if (result && *result != 0U && after_read_) {
      if (reads_before_callback_ > 1U) {
        --reads_before_callback_;
      } else {
        auto callback = std::move(after_read_);
        after_read_ = {};
        reads_before_callback_ = 0U;
        callback();
      }
    }
    return result;
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  write(int fd, std::span<const std::byte> buffer) override {
    if (!write_actions_.empty()) {
      const auto action = write_actions_.front();
      write_actions_.pop_front();
      if (action.error != 0) {
        return std::unexpected(action.error);
      }
      if (action.maximum_transfer != 0U) {
        buffer = buffer.first(std::min(buffer.size(), action.maximum_transfer));
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
    return delegate_->sync_file(fd);
  }

  [[nodiscard]] FileOperationResult<void> sync_directory(int fd) override {
    return delegate_->sync_directory(fd);
  }

  [[nodiscard]] FileOperationResult<void>
  rename_no_replace_at(int source_directory_fd, std::string_view source_name,
                       int destination_directory_fd,
                       std::string_view destination_name) override {
    return delegate_->rename_no_replace_at(source_directory_fd, source_name,
                                           destination_directory_fd,
                                           destination_name);
  }

  [[nodiscard]] FileOperationResult<void>
  remove_file_at(int parent_fd, std::string_view name) override {
    return delegate_->remove_file_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<void>
  remove_directory_at(int parent_fd, std::string_view name) override {
    return delegate_->remove_directory_at(parent_fd, name);
  }

  [[nodiscard]] FileOperationResult<std::vector<std::string>>
  list_directory(int directory_fd, std::size_t maximum_entries) override {
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
  std::shared_ptr<IFileOperations> delegate_;
  std::deque<IoAction> read_actions_;
  std::deque<IoAction> write_actions_;
  std::size_t reads_before_callback_{0};
  std::function<void()> after_read_;
};

TEST(SnapshotChunkerTest, IngestsBoundariesAndReassemblesByteIdenticalModes) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto restore_staging = temporary.path() / "restore-staging";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  ASSERT_TRUE(std::filesystem::create_directory(source / "nested"));
  ASSERT_TRUE(std::filesystem::create_directory(restore_staging));

  const auto small = bytes(73U, 0x11U);
  const auto exact =
      bytes(static_cast<std::size_t>(ManifestLimits::chunk_bytes), 0x22U);
  const auto large =
      bytes(static_cast<std::size_t>(ManifestLimits::chunk_bytes) + 29U, 0x33U);
  write_bytes(source / "b-small", small);
  write_bytes(source / "nested" / "a-exact", exact);
  write_bytes(source / "nested" / "z-large", large);
  write_bytes(source / "z-empty", {});
  ASSERT_EQ(::chmod((source / "b-small").c_str(), 0640), 0);
  ASSERT_EQ(::chmod((source / "nested" / "a-exact").c_str(), 0601), 0);
  ASSERT_EQ(::chmod((source / "nested" / "z-large").c_str(), 0750), 0);
  ASSERT_EQ(::chmod((source / "z-empty").c_str(), 0400), 0);

  FakeStorage storage;
  FakeStorageOperation create_operation{
      storage, container_id(), StorageOperationKind::create_checkpoint,
      temporary.path() / "unused-create-staging"};
  Blake3Hasher hasher;
  SnapshotChunker chunker{create_operation, hasher};
  auto artifacts = chunker.ingest(source);
  ASSERT_TRUE(artifacts) << artifacts.error().diagnostic;
  ASSERT_EQ(artifacts->size(), 4U);

  EXPECT_EQ((*artifacts)[0].path.value(), "b-small");
  EXPECT_EQ((*artifacts)[0].chunks.size(), 1U);
  EXPECT_EQ((*artifacts)[0].chunks[0].size, small.size());
  EXPECT_EQ((*artifacts)[1].path.value(), "nested/a-exact");
  EXPECT_EQ((*artifacts)[1].chunks.size(), 1U);
  EXPECT_EQ((*artifacts)[1].chunks[0].size, ManifestLimits::chunk_bytes);
  EXPECT_EQ((*artifacts)[2].path.value(), "nested/z-large");
  ASSERT_EQ((*artifacts)[2].chunks.size(), 2U);
  EXPECT_EQ((*artifacts)[2].chunks[0].size, ManifestLimits::chunk_bytes);
  EXPECT_EQ((*artifacts)[2].chunks[1].size, 29U);
  EXPECT_EQ((*artifacts)[3].path.value(), "z-empty");
  EXPECT_TRUE((*artifacts)[3].chunks.empty());
  EXPECT_LE(storage.maximum_publication_size(), ManifestLimits::chunk_bytes);

  FakeStorageOperation restore_operation{
      storage, container_id(), StorageOperationKind::restore, restore_staging};
  SnapshotReassembler reassembler{restore_operation, hasher};
  auto output = reassembler.reassemble(*artifacts);
  ASSERT_TRUE(output) << output.error().diagnostic;
  EXPECT_EQ(*output, restore_staging / "snapshot");

  EXPECT_EQ(read_bytes(*output / "b-small"), small);
  EXPECT_EQ(read_bytes(*output / "nested" / "a-exact"), exact);
  EXPECT_EQ(read_bytes(*output / "nested" / "z-large"), large);
  EXPECT_TRUE(read_bytes(*output / "z-empty").empty());
  EXPECT_EQ(file_permissions(*output / "b-small"), 0640U);
  EXPECT_EQ(file_permissions(*output / "nested" / "a-exact"), 0601U);
  EXPECT_EQ(file_permissions(*output / "nested" / "z-large"), 0750U);
  EXPECT_EQ(file_permissions(*output / "z-empty"), 0400U);
}

TEST(SnapshotChunkerTest, DeduplicatesOnlyWithinAContainerNamespace) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  const auto repeated =
      bytes(static_cast<std::size_t>(ManifestLimits::chunk_bytes), 0x51U);
  {
    std::ofstream output{source / "repeated", std::ios::binary};
    ASSERT_TRUE(output.good());
    for (std::size_t index = 0; index < 2U; ++index) {
      output.write(reinterpret_cast<const char *>(repeated.data()),
                   static_cast<std::streamsize>(repeated.size()));
    }
    ASSERT_TRUE(output.good());
  }

  FakeStorage storage;
  Blake3Hasher hasher;
  const auto first_container = container_id();
  FakeStorageOperation first_operation{storage, first_container,
                                       StorageOperationKind::create_checkpoint,
                                       temporary.path() / "unused-1"};
  SnapshotChunker first_chunker{first_operation, hasher};
  auto first = first_chunker.ingest(source);
  ASSERT_TRUE(first);
  ASSERT_EQ(first->front().chunks.size(), 2U);
  EXPECT_EQ(first->front().chunks[0].hash, first->front().chunks[1].hash);
  EXPECT_EQ(storage.publication_count(), 1U);
  EXPECT_EQ(storage.namespace_size(first_container), 1U);

  auto repeated_ingest = first_chunker.ingest(source);
  ASSERT_TRUE(repeated_ingest);
  EXPECT_EQ(storage.publication_count(), 1U);

  const auto second_container = container_id("container-2");
  FakeStorageOperation second_operation{storage, second_container,
                                        StorageOperationKind::create_checkpoint,
                                        temporary.path() / "unused-2"};
  SnapshotChunker second_chunker{second_operation, hasher};
  auto second = second_chunker.ingest(source);
  ASSERT_TRUE(second);
  EXPECT_EQ(storage.publication_count(), 2U);
  EXPECT_EQ(storage.namespace_size(first_container), 1U);
  EXPECT_EQ(storage.namespace_size(second_container), 1U);
}

TEST(SnapshotChunkerTest, RejectsCorruptedExistingChunkInsteadOfReusingIt) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  const auto contents = bytes(128U, 0x61U);
  write_bytes(source / "artifact", contents);

  FakeStorage storage;
  Blake3Hasher hasher;
  const auto container = container_id();
  FakeStorageOperation operation{storage, container,
                                 StorageOperationKind::create_checkpoint,
                                 temporary.path() / "unused"};
  SnapshotChunker chunker{operation, hasher};
  auto first = chunker.ingest(source);
  ASSERT_TRUE(first);
  ASSERT_EQ(first->front().chunks.size(), 1U);
  const auto hash = first->front().chunks.front().hash;
  storage.corrupt(container, hash, bytes(contents.size(), 0x7FU));

  const auto second = chunker.ingest(source);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_EQ(storage.publication_count(), 1U);
}

TEST(SnapshotChunkerTest, DetectsCorruptionBeforeReturningReassembledPath) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto staging = temporary.path() / "restore-staging";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  ASSERT_TRUE(std::filesystem::create_directory(staging));
  const auto contents = bytes(4096U, 0x71U);
  write_bytes(source / "artifact", contents);

  FakeStorage storage;
  Blake3Hasher hasher;
  const auto container = container_id();
  FakeStorageOperation create_operation{storage, container,
                                        StorageOperationKind::create_checkpoint,
                                        temporary.path() / "unused"};
  SnapshotChunker chunker{create_operation, hasher};
  auto artifacts = chunker.ingest(source);
  ASSERT_TRUE(artifacts);
  const auto hash = artifacts->front().chunks.front().hash;
  storage.corrupt(container, hash, bytes(contents.size(), 0x72U));

  FakeStorageOperation restore_operation{
      storage, container, StorageOperationKind::restore, staging};
  SnapshotReassembler reassembler{restore_operation, hasher};
  const auto result = reassembler.reassemble(*artifacts);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_FALSE(std::filesystem::exists(staging / "snapshot" / "artifact"));
}

TEST(SnapshotChunkerTest, RetriesInterruptedAndShortReadsAndWrites) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto staging = temporary.path() / "restore-staging";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  ASSERT_TRUE(std::filesystem::create_directory(staging));
  const auto contents = bytes(257U, 0x81U);
  write_bytes(source / "artifact", contents);

  FakeStorage storage;
  Blake3Hasher hasher;
  const auto container = container_id();
  FakeStorageOperation create_operation{storage, container,
                                        StorageOperationKind::create_checkpoint,
                                        temporary.path() / "unused"};
  auto ingest_io = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  ingest_io->arm_reads(
      {IoAction{.error = EINTR}, IoAction{.maximum_transfer = 3U}});
  SnapshotChunker chunker{create_operation, hasher, ingest_io};
  auto artifacts = chunker.ingest(source);
  ASSERT_TRUE(artifacts) << artifacts.error().diagnostic;

  FakeStorageOperation restore_operation{
      storage, container, StorageOperationKind::restore, staging};
  auto restore_io = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  restore_io->arm_writes(
      {IoAction{.error = EINTR}, IoAction{.maximum_transfer = 5U}});
  SnapshotReassembler reassembler{restore_operation, hasher, restore_io};
  auto output = reassembler.reassemble(*artifacts);
  ASSERT_TRUE(output) << output.error().diagnostic;
  EXPECT_EQ(read_bytes(*output / "artifact"), contents);
}

TEST(SnapshotChunkerTest, RejectsArtifactChangedWhileReading) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  const auto contents = bytes(256U, 0x91U);
  const auto artifact = source / "artifact";
  write_bytes(artifact, contents);

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::create_checkpoint,
                                 temporary.path() / "unused"};
  auto file_operations = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  file_operations->after_next_successful_read([artifact] {
    const std::vector<std::byte> suffix{std::byte{0xFF}};
    append_bytes(artifact, suffix);
  });
  SnapshotChunker chunker{operation, hasher, file_operations};

  const auto result = chunker.ingest(source);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
}

TEST(SnapshotChunkerTest, FinalPassDetectsChangeToAnEarlierArtifact) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  const auto early = source / "a-early";
  write_bytes(early, bytes(32U, 0xA0U));
  write_bytes(source / "z-late", bytes(32U, 0xA1U));
  ASSERT_EQ(::chmod(early.c_str(), 0600), 0);

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::create_checkpoint,
                                 temporary.path() / "unused"};
  auto file_operations = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  file_operations->after_successful_reads(
      2U, [early] { ASSERT_EQ(::chmod(early.c_str(), 0601), 0); });
  SnapshotChunker chunker{operation, hasher, file_operations};

  const auto result = chunker.ingest(source);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
}

TEST(SnapshotChunkerTest, FinalPassDetectsLateExternalHardlink) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  const auto early = source / "a-early";
  const auto external_link = temporary.path() / "external-link";
  write_bytes(early, bytes(32U, 0xA2U));
  write_bytes(source / "z-late", bytes(32U, 0xA3U));

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::create_checkpoint,
                                 temporary.path() / "unused"};
  auto file_operations = std::make_shared<FaultInjectingFileOperations>(
      make_posix_file_operations());
  file_operations->after_successful_reads(2U, [early, external_link] {
    ASSERT_EQ(::link(early.c_str(), external_link.c_str()), 0);
  });
  SnapshotChunker chunker{operation, hasher, file_operations};

  const auto result = chunker.ingest(source);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
}

TEST(SnapshotChunkerTest, RejectsWrongStorageOperationKinds) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto staging = temporary.path() / "staging";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  ASSERT_TRUE(std::filesystem::create_directory(staging));
  write_bytes(source / "artifact", bytes(8U, 0xA4U));

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation restore_operation{
      storage, container_id(), StorageOperationKind::restore, staging};
  SnapshotChunker chunker{restore_operation, hasher};
  const auto ingest = chunker.ingest(source);
  ASSERT_FALSE(ingest);
  EXPECT_EQ(ingest.error().code, StableErrorCode::conflict);

  FakeStorageOperation create_operation{storage, container_id(),
                                        StorageOperationKind::create_checkpoint,
                                        temporary.path() / "unused"};
  SnapshotReassembler reassembler{create_operation, hasher};
  const std::vector<ManifestArtifactV1> no_artifacts;
  const auto reassembled = reassembler.reassemble(no_artifacts);
  ASSERT_FALSE(reassembled);
  EXPECT_EQ(reassembled.error().code, StableErrorCode::conflict);
}

TEST(SnapshotChunkerTest, RoundTripsThroughFilesystemStorageOperation) {
  TemporaryDirectory temporary;
  const auto source = temporary.path() / "source";
  const auto storage_root = temporary.path() / "storage";
  ASSERT_TRUE(std::filesystem::create_directory(source));
  ASSERT_TRUE(std::filesystem::create_directory(storage_root));
  const auto contents = bytes(8193U, 0xA5U);
  write_bytes(source / "artifact", contents);
  ASSERT_EQ(::chmod((source / "artifact").c_str(), 0640), 0);

  auto storage = FilesystemStorage::open(storage_root);
  ASSERT_TRUE(storage) << storage.error().diagnostic;
  Blake3Hasher hasher;
  const auto container = container_id();
  auto create_operation =
      (*storage)->begin_operation(container, operation_id("create"),
                                  StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(create_operation) << create_operation.error().diagnostic;
  SnapshotChunker chunker{**create_operation, hasher};
  auto artifacts = chunker.ingest(source);
  ASSERT_TRUE(artifacts) << artifacts.error().diagnostic;
  ASSERT_TRUE((*create_operation)->cleanup());
  create_operation->reset();

  auto restore_operation = (*storage)->begin_operation(
      container, operation_id("restore"), StorageOperationKind::restore);
  ASSERT_TRUE(restore_operation) << restore_operation.error().diagnostic;
  SnapshotReassembler reassembler{**restore_operation, hasher};
  auto output = reassembler.reassemble(*artifacts);
  ASSERT_TRUE(output) << output.error().diagnostic;
  EXPECT_EQ(read_bytes(*output / "artifact"), contents);
  EXPECT_EQ(file_permissions(*output / "artifact"), 0640U);
}

TEST(SnapshotChunkerTest, PosixDirectoryListingEnforcesCallerBound) {
  TemporaryDirectory temporary;
  write_bytes(temporary.path() / "one", {});
  write_bytes(temporary.path() / "two", {});
  auto operations = make_posix_file_operations();
  auto directory = operations->open_root_directory(temporary.path());
  ASSERT_TRUE(directory);

  const auto listed = operations->list_directory(*directory, 1U);
  operations->close(*directory);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error(), EOVERFLOW);
}

TEST(SnapshotChunkerTest, RejectsSymlinkInSnapshotRootPath) {
  TemporaryDirectory temporary;
  const auto actual_parent = temporary.path() / "actual";
  const auto actual_source = actual_parent / "source";
  ASSERT_TRUE(std::filesystem::create_directory(actual_parent));
  ASSERT_TRUE(std::filesystem::create_directory(actual_source));
  write_bytes(actual_source / "artifact", bytes(8U, 0xA6U));
  std::error_code error;
  std::filesystem::create_directory_symlink(actual_parent,
                                            temporary.path() / "linked", error);
  ASSERT_FALSE(error);

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::create_checkpoint,
                                 temporary.path() / "unused"};
  SnapshotChunker chunker{operation, hasher};
  const auto result = chunker.ingest(temporary.path() / "linked" / "source");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
}

TEST(SnapshotChunkerTest, RejectsSymlinkHardlinkAndFifoEntries) {
  const auto expect_rejected =
      [](const std::function<void(const std::filesystem::path &)> &prepare) {
        TemporaryDirectory temporary;
        const auto source = temporary.path() / "source";
        ASSERT_TRUE(std::filesystem::create_directory(source));
        prepare(source);

        FakeStorage storage;
        Blake3Hasher hasher;
        FakeStorageOperation operation{storage, container_id(),
                                       StorageOperationKind::create_checkpoint,
                                       temporary.path() / "unused"};
        SnapshotChunker chunker{operation, hasher};
        const auto result = chunker.ingest(source);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
      };

  expect_rejected([](const std::filesystem::path &source) {
    const auto target = source.parent_path() / "outside";
    write_bytes(target, bytes(8U, 0xA1U));
    std::error_code error;
    std::filesystem::create_symlink(target, source / "link", error);
    ASSERT_FALSE(error);
  });

  expect_rejected([](const std::filesystem::path &source) {
    write_bytes(source / "original", bytes(8U, 0xA2U));
    ASSERT_EQ(::link((source / "original").c_str(), (source / "alias").c_str()),
              0);
  });

  expect_rejected([](const std::filesystem::path &source) {
    ASSERT_EQ(::mkfifo((source / "pipe").c_str(), 0600), 0);
  });
}

TEST(SnapshotChunkerTest, RejectsExcessivePathDepthBeforeCreatingOutput) {
  TemporaryDirectory temporary;
  const auto staging = temporary.path() / "restore-staging";
  ASSERT_TRUE(std::filesystem::create_directory(staging));
  std::string path;
  for (std::size_t index = 0; index < ManifestLimits::maximum_path_components;
       ++index) {
    path.append("d/");
  }
  path.append("artifact");
  auto relative_path = RelativeSnapshotPath::create(std::move(path));
  ASSERT_TRUE(relative_path);
  const std::vector<ManifestArtifactV1> artifacts{
      ManifestArtifactV1{
          .path = std::move(*relative_path),
          .size = 0,
          .mode = ManifestArtifactV1::posix_regular_file | 0600U,
          .chunks = {},
      },
  };

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::restore, staging};
  SnapshotReassembler reassembler{operation, hasher};
  const auto result = reassembler.reassemble(artifacts);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_FALSE(std::filesystem::exists(staging / "snapshot"));
}

TEST(SnapshotChunkerTest, RejectsPathPrefixConflictBeforeCreatingOutput) {
  TemporaryDirectory temporary;
  const auto staging = temporary.path() / "restore-staging";
  ASSERT_TRUE(std::filesystem::create_directory(staging));
  auto parent = RelativeSnapshotPath::create("artifact").value();
  auto child = RelativeSnapshotPath::create("artifact/child").value();
  const std::vector<ManifestArtifactV1> artifacts{
      ManifestArtifactV1{
          .path = std::move(parent),
          .size = 0,
          .mode = ManifestArtifactV1::posix_regular_file | 0600U,
          .chunks = {},
      },
      ManifestArtifactV1{
          .path = std::move(child),
          .size = 0,
          .mode = ManifestArtifactV1::posix_regular_file | 0600U,
          .chunks = {},
      },
  };

  FakeStorage storage;
  Blake3Hasher hasher;
  FakeStorageOperation operation{storage, container_id(),
                                 StorageOperationKind::restore, staging};
  SnapshotReassembler reassembler{operation, hasher};
  const auto result = reassembler.reassemble(artifacts);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, StableErrorCode::corrupted_snapshot);
  EXPECT_FALSE(std::filesystem::exists(staging / "snapshot"));
}

} // namespace
} // namespace gravesentinel::ccm
