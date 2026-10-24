#include "src/core/checkpoint_creator.hxx"

#include "src/core/blake3_hasher.hxx"
#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"
#include "src/storage/filesystem_storage.hxx"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

constexpr std::uint64_t kCreatedAtNanoseconds = 1'730'000'000'123'456'789ULL;
constexpr std::uint32_t kRegularFileMode =
    ManifestArtifactV1::posix_regular_file | 0644U;

[[nodiscard]] CheckpointErrorDetail
make_test_error(StableErrorCode code, std::string diagnostic,
                StorageSideEffect side_effect = StorageSideEffect::none,
                bool retryable = false) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = std::nullopt,
      .storage_side_effect = side_effect,
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

template <typename Identifier>
[[nodiscard]] Identifier make_identifier(std::string value) {
  auto created = Identifier::create(std::move(value));
  if (!created) {
    throw std::runtime_error(created.error().diagnostic);
  }
  return std::move(*created);
}

[[nodiscard]] RuntimeFingerprint valid_runtime_fingerprint() {
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

[[nodiscard]] std::vector<std::byte> make_bytes(std::size_t size,
                                                std::uint8_t seed) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < size; ++index) {
    const auto value = (static_cast<std::uint64_t>(seed) +
                        static_cast<std::uint64_t>(index) * 31U) &
                       0xFFU;
    result[index] = static_cast<std::byte>(static_cast<std::uint8_t>(value));
  }
  return result;
}

void write_bytes(const std::filesystem::path &file,
                 std::span<const std::byte> contents) {
  std::ofstream output{file, std::ios::binary | std::ios::trunc};
  if (!output.good()) {
    throw std::runtime_error("could not open a test artifact for writing");
  }
  output.write(reinterpret_cast<const char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
  if (!output.good()) {
    throw std::runtime_error("could not write a test artifact");
  }
}

[[nodiscard]] std::vector<std::byte>
read_bytes(const std::filesystem::path &file) {
  std::ifstream input{file, std::ios::binary | std::ios::ate};
  if (!input.good()) {
    throw std::runtime_error("could not open a test artifact for reading");
  }
  const auto end = input.tellg();
  if (end < 0) {
    throw std::runtime_error("could not determine a test artifact size");
  }
  std::vector<std::byte> result(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(result.data()),
             static_cast<std::streamsize>(result.size()));
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("could not read a test artifact");
  }
  return result;
}

[[nodiscard]] StorageDirectoryIdentity
storage_identity(const std::filesystem::path &directory) noexcept {
  struct stat attributes{};
  if (::stat(directory.c_str(), &attributes) != 0) {
    return {};
  }
  return StorageDirectoryIdentity{
      .device = static_cast<std::uint64_t>(attributes.st_dev),
      .inode = static_cast<std::uint64_t>(attributes.st_ino),
  };
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    static std::atomic<std::uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("gravesentinel-checkpoint-creator-" + std::to_string(::getpid()) +
             "-" + std::to_string(sequence.fetch_add(1U)));
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

struct OperationRecord {
  ContainerId container;
  OperationId operation;
  StorageOperationKind kind;
  std::filesystem::path staging_directory;
  std::filesystem::path runtime_artifact_directory;
  std::size_t cleanup_calls{0};
  bool cleanup_complete{false};
  bool lease_released{false};
};

struct PublicationEvent {
  std::string kind;
  std::string container;
  std::string operation;

  bool operator==(const PublicationEvent &) const = default;
};

class FakeStorage;

class FakeStorageOperation final : public IStorageOperation {
public:
  FakeStorageOperation(FakeStorage &storage,
                       std::shared_ptr<OperationRecord> record)
      : storage_(storage), record_(std::move(record)) {}

  ~FakeStorageOperation() override;

  [[nodiscard]] const ContainerId &container_id() const noexcept override {
    return record_->container;
  }

  [[nodiscard]] StorageOperationKind kind() const noexcept override {
    return record_->kind;
  }

  [[nodiscard]] const std::filesystem::path &
  staging_directory() const noexcept override {
    return record_->staging_directory;
  }

  [[nodiscard]] StorageDirectoryIdentity
  staging_directory_identity() const noexcept override {
    return storage_identity(record_->staging_directory);
  }

  [[nodiscard]] const std::filesystem::path &
  runtime_artifact_directory() const noexcept override {
    return record_->runtime_artifact_directory;
  }

  [[nodiscard]] StorageDirectoryIdentity
  runtime_artifact_directory_identity() const noexcept override {
    return storage_identity(record_->runtime_artifact_directory);
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ChunkHash &hash) override;

  [[nodiscard]] CheckpointResult<void>
  publish_chunk(const ChunkHash &hash,
                std::span<const std::byte> contents) override;

  [[nodiscard]] CheckpointResult<void>
  publish_manifest(const CheckpointId &checkpoint,
                   std::span<const std::byte> manifest) override;

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStoragePin>>
  pin_manifest(std::uint64_t) override {
    return std::unexpected(
        make_test_error(StableErrorCode::not_found, "manifest is not pinned"));
  }

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(std::size_t keep_last) override;

  [[nodiscard]] CheckpointResult<void> cleanup() override;

private:
  FakeStorage &storage_;
  std::shared_ptr<OperationRecord> record_;
};

class FakeStorage final : public IStorage {
public:
  explicit FakeStorage(std::filesystem::path root) : root_(std::move(root)) {}

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &container, const OperationId &operation,
                  StorageOperationKind kind) override {
    if (active_containers_.contains(container.value())) {
      return std::unexpected(make_test_error(
          StableErrorCode::conflict,
          "another operation already holds the per-container lease", {}, true));
    }

    active_containers_.insert(container.value());
    maximum_active_operations_ =
        std::max(maximum_active_operations_, active_containers_.size());

    const auto directory =
        root_ / ("operation-" + std::to_string(operation_sequence_++));
    const auto runtime_directory = directory / "runtime-artifact";
    std::error_code create_error;
    std::filesystem::create_directories(runtime_directory, create_error);
    if (create_error) {
      active_containers_.erase(container.value());
      return std::unexpected(make_test_error(
          StableErrorCode::storage_error,
          "could not create fake operation staging directories"));
    }

    auto record = std::make_shared<OperationRecord>(OperationRecord{
        .container = container,
        .operation = operation,
        .kind = kind,
        .staging_directory = directory,
        .runtime_artifact_directory = runtime_directory,
    });
    records_.push_back(record);
    return std::make_unique<FakeStorageOperation>(*this, std::move(record));
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageMaintenanceOperation>>
  begin_maintenance(const ContainerId &, StorageOperationKind) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "maintenance is not used by this fake"));
  }

  [[nodiscard]] CheckpointResult<bool>
  contains_chunk(const ContainerId &container,
                 const ChunkHash &hash) const override {
    const auto namespace_it = chunks_.find(container.value());
    return namespace_it != chunks_.end() && namespace_it->second.contains(hash);
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container,
             const ChunkHash &hash) const override {
    const auto namespace_it = chunks_.find(container.value());
    if (namespace_it == chunks_.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    const auto chunk_it = namespace_it->second.find(hash);
    if (chunk_it == namespace_it->second.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    return chunk_it->second;
  }

  [[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &container) const override {
    list_observed_active_ =
        list_observed_active_ || active_containers_.contains(container.value());
    std::vector<StoredManifest> result;
    const auto namespace_it = manifests_.find(container.value());
    if (namespace_it == manifests_.end()) {
      return result;
    }
    result.reserve(namespace_it->second.size());
    for (const auto &[generation, encoded] : namespace_it->second) {
      static_cast<void>(encoded);
      result.push_back(StoredManifest{.generation = generation});
    }
    return result;
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &container,
                std::uint64_t generation) const override {
    const auto namespace_it = manifests_.find(container.value());
    if (namespace_it == manifests_.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "manifest is absent"));
    }
    const auto manifest_it = namespace_it->second.find(generation);
    if (manifest_it == namespace_it->second.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "manifest is absent"));
    }
    return manifest_it->second;
  }

  void seed_checkpoint(const ContainerId &container,
                       const CheckpointId &checkpoint, std::uint64_t generation,
                       std::span<const std::byte> contents,
                       const Blake3Hasher &hasher) {
    auto hash = hasher.hash(contents);
    if (!hash) {
      throw std::runtime_error(hash.error().diagnostic);
    }
    chunks_[container.value()][*hash] =
        std::vector<std::byte>{contents.begin(), contents.end()};

    auto relative = RelativeSnapshotPath::create(
        "previous-" + std::to_string(generation) + ".bin");
    if (!relative) {
      throw std::runtime_error(relative.error().diagnostic);
    }
    const ManifestV1 manifest{
        .container_id = container,
        .checkpoint_id = checkpoint,
        .generation = generation,
        .created_at_unix_nanoseconds = kCreatedAtNanoseconds - generation,
        .source_image_identity = "registry.example/previous@sha256:abcdef",
        .runtime_fingerprint = valid_runtime_fingerprint(),
        .artifacts = {ManifestArtifactV1{
            .path = std::move(*relative),
            .size = static_cast<std::uint64_t>(contents.size()),
            .mode = kRegularFileMode,
            .chunks = {ManifestChunkV1{
                .hash = *hash,
                .size = static_cast<std::uint64_t>(contents.size()),
            }},
        }},
    };
    auto encoded = encode_manifest_v1(manifest);
    if (!encoded) {
      throw std::runtime_error(encoded.error().diagnostic);
    }
    manifests_[container.value()][generation] = std::move(*encoded);
  }

  [[nodiscard]] CheckpointResult<void>
  publish_chunk(const std::shared_ptr<OperationRecord> &record,
                const ChunkHash &hash, std::span<const std::byte> contents) {
    if (next_chunk_failure_) {
      auto injected = std::move(*next_chunk_failure_);
      next_chunk_failure_.reset();
      if (injected.storage_side_effect ==
          StorageSideEffect::publication_may_be_visible) {
        chunks_[record->container.value()][hash] =
            std::vector<std::byte>{contents.begin(), contents.end()};
        ++unique_chunk_publications_;
        publication_events_.push_back(PublicationEvent{
            .kind = "chunk-visible-unconfirmed",
            .container = record->container.value(),
            .operation = record->operation.value(),
        });
      }
      return std::unexpected(std::move(injected));
    }

    auto &container_chunks = chunks_[record->container.value()];
    const auto existing = container_chunks.find(hash);
    if (existing != container_chunks.end()) {
      if (std::ranges::equal(existing->second, contents)) {
        return {};
      }
      return std::unexpected(
          make_test_error(StableErrorCode::corrupted_snapshot,
                          "published chunk conflicts with existing content"));
    }

    container_chunks.emplace(
        hash, std::vector<std::byte>{contents.begin(), contents.end()});
    ++unique_chunk_publications_;
    publication_events_.push_back(PublicationEvent{
        .kind = "chunk",
        .container = record->container.value(),
        .operation = record->operation.value(),
    });
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  publish_manifest(const std::shared_ptr<OperationRecord> &record,
                   const CheckpointId &checkpoint,
                   std::span<const std::byte> encoded) {
    auto decoded = decode_manifest_v1(encoded);
    if (!decoded) {
      return std::unexpected(
          make_test_error(StableErrorCode::storage_error,
                          "fake storage rejected a non-canonical manifest"));
    }
    if (decoded->container_id != record->container ||
        decoded->checkpoint_id != checkpoint) {
      return std::unexpected(make_test_error(
          StableErrorCode::storage_error,
          "fake storage rejected a manifest identity mismatch"));
    }

    if (next_manifest_failure_) {
      auto injected = std::move(*next_manifest_failure_);
      next_manifest_failure_.reset();
      if (injected.storage_side_effect ==
          StorageSideEffect::publication_may_be_visible) {
        manifests_[record->container.value()][decoded->generation] =
            std::vector<std::byte>{encoded.begin(), encoded.end()};
        publication_events_.push_back(PublicationEvent{
            .kind = "manifest-visible-unconfirmed",
            .container = record->container.value(),
            .operation = record->operation.value(),
        });
      }
      return std::unexpected(std::move(injected));
    }

    manifests_[record->container.value()][decoded->generation] =
        std::vector<std::byte>{encoded.begin(), encoded.end()};
    publication_events_.push_back(PublicationEvent{
        .kind = "manifest",
        .container = record->container.value(),
        .operation = record->operation.value(),
    });
    return {};
  }

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(const std::shared_ptr<OperationRecord> &record,
                  std::size_t keep_last) {
    retention_calls_.push_back(keep_last);
    publication_events_.push_back(PublicationEvent{
        .kind = "retention",
        .container = record->container.value(),
        .operation = record->operation.value(),
    });
    if (std::exchange(throw_next_retention_, false)) {
      throw std::runtime_error("injected retention exception");
    }
    if (next_retention_failure_) {
      auto injected = std::move(*next_retention_failure_);
      next_retention_failure_.reset();
      return std::unexpected(std::move(injected));
    }
    return retention_report_;
  }

  [[nodiscard]] std::optional<CheckpointErrorDetail> take_cleanup_failure() {
    if (std::exchange(throw_next_cleanup_, false)) {
      throw std::runtime_error("injected cleanup exception");
    }
    if (!next_cleanup_failure_) {
      return std::nullopt;
    }
    auto injected = std::move(next_cleanup_failure_);
    next_cleanup_failure_.reset();
    return injected;
  }

  void release(const std::shared_ptr<OperationRecord> &record) noexcept {
    if (!record->lease_released) {
      active_containers_.erase(record->container.value());
      record->lease_released = true;
    }
  }

  void fail_next_chunk(CheckpointErrorDetail injected) {
    next_chunk_failure_ = std::move(injected);
  }

  void fail_next_manifest(CheckpointErrorDetail injected) {
    next_manifest_failure_ = std::move(injected);
  }

  void fail_next_retention(CheckpointErrorDetail injected) {
    next_retention_failure_ = std::move(injected);
  }

  void throw_next_retention() { throw_next_retention_ = true; }

  void throw_next_cleanup() { throw_next_cleanup_ = true; }

  void set_retention_report(RetentionReport report) {
    retention_report_ = report;
  }

  void fail_next_cleanup(CheckpointErrorDetail injected) {
    next_cleanup_failure_ = std::move(injected);
  }

  void clear_observations() {
    publication_events_.clear();
    retention_calls_.clear();
    unique_chunk_publications_ = 0;
    list_observed_active_ = false;
  }

  [[nodiscard]] bool has_active_operation(const ContainerId &container) const {
    return active_containers_.contains(container.value());
  }

  [[nodiscard]] bool list_observed_active() const noexcept {
    return list_observed_active_;
  }

  [[nodiscard]] std::size_t maximum_active_operations() const noexcept {
    return maximum_active_operations_;
  }

  [[nodiscard]] std::size_t unique_chunk_publications() const noexcept {
    return unique_chunk_publications_;
  }

  [[nodiscard]] const std::vector<PublicationEvent> &
  publication_events() const noexcept {
    return publication_events_;
  }

  [[nodiscard]] const std::vector<std::size_t> &
  retention_calls() const noexcept {
    return retention_calls_;
  }

  [[nodiscard]] const std::vector<std::shared_ptr<OperationRecord>> &
  records() const noexcept {
    return records_;
  }

private:
  using ChunkNamespace = std::map<ChunkHash, std::vector<std::byte>>;
  using ManifestNamespace = std::map<std::uint64_t, std::vector<std::byte>>;

  std::filesystem::path root_;
  std::map<std::string, ChunkNamespace> chunks_;
  std::map<std::string, ManifestNamespace> manifests_;
  std::set<std::string> active_containers_;
  std::vector<std::shared_ptr<OperationRecord>> records_;
  std::vector<PublicationEvent> publication_events_;
  std::vector<std::size_t> retention_calls_;
  RetentionReport retention_report_{
      .manifests_deleted = 0,
      .manifests_deferred = 0,
      .complete = true,
  };
  std::optional<CheckpointErrorDetail> next_chunk_failure_;
  std::optional<CheckpointErrorDetail> next_manifest_failure_;
  std::optional<CheckpointErrorDetail> next_retention_failure_;
  std::optional<CheckpointErrorDetail> next_cleanup_failure_;
  bool throw_next_retention_{false};
  bool throw_next_cleanup_{false};
  std::size_t operation_sequence_{0};
  std::size_t maximum_active_operations_{0};
  std::size_t unique_chunk_publications_{0};
  mutable bool list_observed_active_{false};
};

FakeStorageOperation::~FakeStorageOperation() { storage_.release(record_); }

CheckpointResult<std::vector<std::byte>>
FakeStorageOperation::read_chunk(const ChunkHash &hash) {
  return storage_.read_chunk(record_->container, hash);
}

CheckpointResult<void>
FakeStorageOperation::publish_chunk(const ChunkHash &hash,
                                    std::span<const std::byte> contents) {
  return storage_.publish_chunk(record_, hash, contents);
}

CheckpointResult<void>
FakeStorageOperation::publish_manifest(const CheckpointId &checkpoint,
                                       std::span<const std::byte> manifest) {
  return storage_.publish_manifest(record_, checkpoint, manifest);
}

CheckpointResult<RetentionReport>
FakeStorageOperation::apply_retention(std::size_t keep_last) {
  return storage_.apply_retention(record_, keep_last);
}

CheckpointResult<void> FakeStorageOperation::cleanup() {
  ++record_->cleanup_calls;
  if (auto injected = storage_.take_cleanup_failure()) {
    storage_.release(record_);
    return std::unexpected(std::move(*injected));
  }

  std::error_code remove_error;
  std::filesystem::remove_all(record_->staging_directory, remove_error);
  storage_.release(record_);
  if (remove_error) {
    return std::unexpected(
        make_test_error(StableErrorCode::storage_error,
                        "could not remove fake operation staging directory"));
  }
  record_->cleanup_complete = true;
  return {};
}

class FaultInjectingHasher final : public IHasher {
public:
  explicit FaultInjectingHasher(const IHasher &delegate)
      : delegate_(delegate) {}

  [[nodiscard]] CheckpointResult<ChunkHash>
  hash(std::span<const std::byte> contents) const override {
    if (next_failure_) {
      auto injected = std::move(*next_failure_);
      next_failure_.reset();
      return std::unexpected(std::move(injected));
    }
    return delegate_.hash(contents);
  }

  void fail_next(CheckpointErrorDetail injected) {
    next_failure_ = std::move(injected);
  }

private:
  const IHasher &delegate_;
  mutable std::optional<CheckpointErrorDetail> next_failure_;
};

class TestSnapshotChunker final : public ISnapshotChunker {
public:
  explicit TestSnapshotChunker(const IHasher &hasher) : hasher_(hasher) {}

  [[nodiscard]] CheckpointResult<std::vector<ManifestArtifactV1>>
  ingest(IStorageOperation &operation,
         const std::filesystem::path &snapshot_root) override {
    ++ingest_calls_;
    roots_.push_back(snapshot_root);
    if (next_source_read_failure_) {
      auto injected = std::move(*next_source_read_failure_);
      next_source_read_failure_.reset();
      return std::unexpected(std::move(injected));
    }

    std::optional<std::filesystem::path> artifact;
    try {
      for (const auto &entry :
           std::filesystem::directory_iterator(snapshot_root)) {
        if (!entry.is_regular_file() || artifact) {
          return std::unexpected(make_test_error(
              StableErrorCode::storage_error,
              "fake chunker expected exactly one regular runtime artifact"));
        }
        artifact = entry.path();
      }
    } catch (const std::filesystem::filesystem_error &) {
      return std::unexpected(make_test_error(
          StableErrorCode::storage_error,
          "fake chunker could not enumerate the runtime artifact directory"));
    }
    if (!artifact) {
      return std::unexpected(
          make_test_error(StableErrorCode::storage_error,
                          "fake chunker found no runtime artifact"));
    }

    std::vector<std::byte> contents;
    try {
      contents = read_bytes(*artifact);
    } catch (const std::runtime_error &) {
      return std::unexpected(
          make_test_error(StableErrorCode::storage_error,
                          "fake chunker could not read the runtime artifact"));
    }

    std::vector<ManifestChunkV1> chunks;
    for (std::size_t offset = 0; offset < contents.size();) {
      const auto remaining = contents.size() - offset;
      const auto chunk_size = std::min<std::size_t>(
          remaining, static_cast<std::size_t>(ManifestLimits::chunk_bytes));
      const std::span<const std::byte> chunk{contents.data() + offset,
                                             chunk_size};
      auto hash = hasher_.hash(chunk);
      if (!hash) {
        return std::unexpected(hash.error());
      }

      auto existing = operation.read_chunk(*hash);
      if (existing) {
        if (!std::ranges::equal(*existing, chunk)) {
          return std::unexpected(make_test_error(
              StableErrorCode::corrupted_snapshot,
              "existing fake chunk failed content verification"));
        }
        ++reused_chunks_;
      } else if (existing.error().code == StableErrorCode::not_found) {
        if (auto published = operation.publish_chunk(*hash, chunk);
            !published) {
          return std::unexpected(published.error());
        }
      } else {
        return std::unexpected(existing.error());
      }

      chunks.push_back(ManifestChunkV1{
          .hash = *hash,
          .size = static_cast<std::uint64_t>(chunk_size),
      });
      offset += chunk_size;
    }

    auto relative = RelativeSnapshotPath::create(artifact->filename().string());
    if (!relative) {
      return std::unexpected(make_test_error(
          StableErrorCode::internal,
          "fake chunker produced an invalid relative artifact path"));
    }
    return std::vector<ManifestArtifactV1>{ManifestArtifactV1{
        .path = std::move(*relative),
        .size = static_cast<std::uint64_t>(contents.size()),
        .mode = kRegularFileMode,
        .chunks = std::move(chunks),
    }};
  }

  void fail_next_source_read(CheckpointErrorDetail injected) {
    next_source_read_failure_ = std::move(injected);
  }

  [[nodiscard]] std::size_t ingest_calls() const noexcept {
    return ingest_calls_;
  }

  [[nodiscard]] std::size_t reused_chunks() const noexcept {
    return reused_chunks_;
  }

  [[nodiscard]] const std::vector<std::filesystem::path> &
  roots() const noexcept {
    return roots_;
  }

private:
  const IHasher &hasher_;
  std::optional<CheckpointErrorDetail> next_source_read_failure_;
  std::vector<std::filesystem::path> roots_;
  std::size_t ingest_calls_{0};
  std::size_t reused_chunks_{0};
};

struct CaptureCall {
  ContainerId container;
  RuntimeContainerRef runtime_container;
  std::filesystem::path artifact_directory;
  RuntimeCpuAssignment cpu_assignment;
};

class FakePodmanAdapter final : public IPodmanAdapter {
public:
  FakePodmanAdapter()
      : payload_(make_bytes(
            static_cast<std::size_t>(ManifestLimits::chunk_bytes) + 17U,
            0x31U)) {}

  [[nodiscard]] CheckpointResult<void>
  validate_restore_compatibility(const RuntimeFingerprint &, std::string_view,
                                 RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "restore compatibility is not under test"));
  }

  [[nodiscard]] CheckpointResult<CapturedSnapshot>
  capture(const ContainerId &container,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &artifact_directory,
          RuntimeCpuAssignment cpu_assignment) override {
    calls_.push_back(CaptureCall{
        .container = container,
        .runtime_container = runtime_container,
        .artifact_directory = artifact_directory,
        .cpu_assignment = cpu_assignment,
    });
    if (next_capture_failure_) {
      auto injected = std::move(*next_capture_failure_);
      next_capture_failure_.reset();
      return std::unexpected(std::move(injected));
    }

    const auto artifact =
        artifact_directory /
        ("snapshot-" + std::to_string(calls_.size()) + ".bin");
    try {
      write_bytes(artifact, payload_);
    } catch (const std::runtime_error &) {
      return std::unexpected(
          make_test_error(StableErrorCode::capture_failed,
                          "fake adapter could not write its runtime artifact"));
    }
    artifacts_.push_back(artifact);

    if (next_capture_hook_) {
      auto hook = std::move(next_capture_hook_);
      next_capture_hook_ = {};
      hook();
    }

    return CapturedSnapshot{
        .artifact_path = artifact,
        .source_image_identity =
            invalid_manifest_metadata_
                ? std::string{}
                : "registry.example/application@sha256:0123456789abcdef",
        .runtime_fingerprint = valid_runtime_fingerprint(),
        .source_incarnation =
            RuntimeIncarnation{
                .reference = runtime_container,
                .runtime_id = "runtime-id-" + std::to_string(calls_.size()),
            },
    };
  }

  [[nodiscard]] CheckpointResult<void>
  stop_and_remove(const ContainerId &, const RuntimeIncarnation &,
                  RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "stop-and-remove is not under test"));
  }

  [[nodiscard]] CheckpointResult<void>
  confirm_absence(const ContainerId &, const RuntimeContainerRef &,
                  RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(StableErrorCode::internal,
                                           "absence check is not under test"));
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  restore(const ContainerId &, const RuntimeContainerRef &,
          const std::filesystem::path &, const RuntimeFingerprint &,
          std::string_view, RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(StableErrorCode::internal,
                                           "restore is not under test"));
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  cold_start(const ContainerId &, const RuntimeContainerRef &,
             const ColdStartSpec &, RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(StableErrorCode::internal,
                                           "cold start is not under test"));
  }

  [[nodiscard]] CheckpointResult<void>
  cleanup_failed_restore(const ContainerId &, const RuntimeContainerRef &,
                         RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "restore cleanup is not under test"));
  }

  void fail_next_capture(CheckpointErrorDetail injected) {
    next_capture_failure_ = std::move(injected);
  }

  void invalidate_manifest_metadata() noexcept {
    invalid_manifest_metadata_ = true;
  }

  void on_next_capture(std::function<void()> hook) {
    next_capture_hook_ = std::move(hook);
  }

  [[nodiscard]] const std::vector<CaptureCall> &calls() const noexcept {
    return calls_;
  }

  [[nodiscard]] const std::vector<std::filesystem::path> &
  artifacts() const noexcept {
    return artifacts_;
  }

private:
  std::vector<std::byte> payload_;
  std::optional<CheckpointErrorDetail> next_capture_failure_;
  std::function<void()> next_capture_hook_;
  std::vector<CaptureCall> calls_;
  std::vector<std::filesystem::path> artifacts_;
  bool invalid_manifest_metadata_{false};
};

class FakeClock final : public IClock {
public:
  FakeClock()
      : wall_time_(WallTime{std::chrono::duration_cast<WallTime::duration>(
            std::chrono::nanoseconds{kCreatedAtNanoseconds})}) {}

  [[nodiscard]] WallTime wall_now() const noexcept override {
    return wall_time_;
  }

  [[nodiscard]] MonotonicTime monotonic_now() const noexcept override {
    return MonotonicTime{std::chrono::seconds{42}};
  }

private:
  WallTime wall_time_;
};

class FakeIdGenerator final : public IIdGenerator {
public:
  explicit FakeIdGenerator(std::vector<std::string> checkpoint_values) {
    for (auto &value : checkpoint_values) {
      checkpoint_ids_.push_back(
          make_identifier<CheckpointId>(std::move(value)));
    }
  }

  [[nodiscard]] CheckpointResult<CheckpointId> new_checkpoint_id() override {
    if (checkpoint_ids_.empty()) {
      return std::unexpected(
          make_test_error(StableErrorCode::resource_exhausted,
                          "fake checkpoint ID sequence is exhausted"));
    }
    auto generated = std::move(checkpoint_ids_.front());
    checkpoint_ids_.pop_front();
    issued_checkpoint_ids_.push_back(generated);
    return generated;
  }

  [[nodiscard]] CheckpointResult<OperationId> new_operation_id() override {
    return make_identifier<OperationId>(
        "generated-operation-" + std::to_string(generated_operation_count_++));
  }

  [[nodiscard]] const std::vector<CheckpointId> &
  issued_checkpoint_ids() const noexcept {
    return issued_checkpoint_ids_;
  }

private:
  std::deque<CheckpointId> checkpoint_ids_;
  std::vector<CheckpointId> issued_checkpoint_ids_;
  std::size_t generated_operation_count_{0};
};

struct CapturedLogEntry {
  LogLevel level;
  std::string message;
  std::map<std::string, std::string> fields;
};

class FakeLogger final : public ILogger {
public:
  void log(LogLevel level, std::string_view message,
           std::span<const LogField> fields) noexcept override {
    CapturedLogEntry entry{
        .level = level,
        .message = std::string{message},
        .fields = {},
    };
    for (const auto &field : fields) {
      entry.fields.emplace(field.key, field.value);
    }
    entries_.push_back(std::move(entry));
  }

  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  transitions() const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto &entry : entries_) {
      if (entry.message == "checkpoint create state transition") {
        result.emplace_back(entry.fields.at("from_state"),
                            entry.fields.at("to_state"));
      }
    }
    return result;
  }

  [[nodiscard]] const std::vector<CapturedLogEntry> &entries() const noexcept {
    return entries_;
  }

private:
  std::vector<CapturedLogEntry> entries_;
};

class TestRig final {
public:
  explicit TestRig(std::vector<std::string> checkpoint_ids = {"checkpoint-new"})
      : storage(temporary.path()), faulting_hasher(real_hasher),
        chunker(faulting_hasher), ids(std::move(checkpoint_ids)),
        creator(storage, adapter, chunker, clock, ids, logger) {
    const std::vector<std::pair<std::uint64_t, std::uint8_t>> seeds{
        {1U, 0x11U},
        {4U, 0x44U},
        {7U, 0x77U},
    };
    for (const auto &[generation, seed] : seeds) {
      auto contents =
          make_bytes(83U + static_cast<std::size_t>(generation), seed);
      previous_contents.emplace(generation, contents);
      storage.seed_checkpoint(
          default_container(),
          make_identifier<CheckpointId>("checkpoint-old-" +
                                        std::to_string(generation)),
          generation, contents, real_hasher);
    }
    storage.clear_observations();
  }

  [[nodiscard]] static ContainerId default_container() {
    return make_identifier<ContainerId>("container-primary");
  }

  TemporaryDirectory temporary;
  Blake3Hasher real_hasher;
  FakeStorage storage;
  FakePodmanAdapter adapter;
  FaultInjectingHasher faulting_hasher;
  TestSnapshotChunker chunker;
  FakeClock clock;
  FakeIdGenerator ids;
  FakeLogger logger;
  CheckpointCreator creator;
  std::map<std::uint64_t, std::vector<std::byte>> previous_contents;
};

[[nodiscard]] CheckpointCreateRequest
make_request(std::string operation_value = "operation-create-1",
             ContainerId container = TestRig::default_container()) {
  return CheckpointCreateRequest{
      .request_id = make_identifier<RequestId>("request-correlation-1"),
      .operation_id = make_identifier<OperationId>(std::move(operation_value)),
      .container_id = std::move(container),
      .runtime_container =
          make_identifier<RuntimeContainerRef>("runtime-container-1"),
      .cpu_assignment =
          RuntimeCpuAssignment{
              .container_cpu = 2U,
              .checkpoint_cpu = 3U,
          },
  };
}

void expect_correlation(const CheckpointErrorDetail &failure,
                        const RequestId &request) {
  ASSERT_TRUE(failure.correlation_id.has_value());
  EXPECT_EQ(*failure.correlation_id, request);
}

void expect_all_logs_correlated(const FakeLogger &logger,
                                const CheckpointCreateRequest &request) {
  ASSERT_FALSE(logger.entries().empty());
  for (const auto &entry : logger.entries()) {
    EXPECT_EQ(entry.fields.at("request_id"), request.request_id.value());
    EXPECT_EQ(entry.fields.at("operation_id"), request.operation_id.value());
    EXPECT_EQ(entry.fields.at("container_id"), request.container_id.value());
  }
}

void expect_previous_checkpoints_intact(TestRig &rig) {
  const auto container = TestRig::default_container();
  auto listed = rig.storage.list_manifests(container);
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;

  std::vector<std::uint64_t> generations;
  for (const auto &stored : *listed) {
    if (stored.generation <= 7U) {
      generations.push_back(stored.generation);
    }
  }
  EXPECT_EQ(generations, (std::vector<std::uint64_t>{1U, 4U, 7U}));

  for (const auto &[generation, expected_contents] : rig.previous_contents) {
    auto encoded = rig.storage.read_manifest(container, generation);
    ASSERT_TRUE(encoded.has_value()) << encoded.error().diagnostic;
    auto decoded = decode_manifest_v1(*encoded);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().diagnostic;
    auto canonical = encode_manifest_v1(*decoded);
    ASSERT_TRUE(canonical.has_value()) << canonical.error().diagnostic;
    EXPECT_EQ(*canonical, *encoded);

    auto restore_operation = rig.storage.begin_operation(
        container,
        make_identifier<OperationId>("verify-old-" +
                                     std::to_string(generation)),
        StorageOperationKind::restore);
    ASSERT_TRUE(restore_operation.has_value())
        << restore_operation.error().diagnostic;
    SnapshotReassembler reassembler{**restore_operation, rig.real_hasher};
    auto restored = reassembler.reassemble(decoded->artifacts);
    ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
    ASSERT_EQ(decoded->artifacts.size(), 1U);
    EXPECT_EQ(read_bytes(*restored / decoded->artifacts.front().path.value()),
              expected_contents);
    auto cleaned = (*restore_operation)->cleanup();
    ASSERT_TRUE(cleaned.has_value()) << cleaned.error().diagnostic;
  }
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>>
expected_failure_transitions(std::string failure_state,
                             std::string terminal_state = "FAILED") {
  std::vector<std::pair<std::string, std::string>> result{
      {"IDLE", "CAPTURING"},
  };
  if (failure_state == "CHUNKING" || failure_state == "COMMITTING") {
    result.emplace_back("CAPTURING", "CHUNKING");
  }
  if (failure_state == "COMMITTING") {
    result.emplace_back("CHUNKING", "COMMITTING");
  }
  result.emplace_back(std::move(failure_state), std::move(terminal_state));
  return result;
}

TEST(CheckpointCreatorTest,
     HappyPathPublishesCanonicalManifestLastAndReleasesLease) {
  TestRig rig;
  const auto request = make_request();

  auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_EQ(result->operation_id, request.operation_id);
  EXPECT_EQ(result->checkpoint_id.value(), "checkpoint-new");
  EXPECT_EQ(result->generation, 8U);
  EXPECT_EQ(result->source_incarnation.reference, request.runtime_container);
  EXPECT_TRUE(result->retention_complete);
  EXPECT_TRUE(result->staging_cleanup_complete);
  ASSERT_EQ(rig.ids.issued_checkpoint_ids().size(), 1U);
  EXPECT_NE(rig.ids.issued_checkpoint_ids().front().value(),
            "checkpoint-old-7");
  EXPECT_TRUE(rig.storage.list_observed_active());

  ASSERT_EQ(rig.adapter.calls().size(), 1U);
  ASSERT_EQ(rig.storage.records().size(), 1U);
  const auto &record = *rig.storage.records().front();
  EXPECT_EQ(rig.adapter.calls().front().artifact_directory,
            record.runtime_artifact_directory);
  EXPECT_NE(record.runtime_artifact_directory, record.staging_directory);
  EXPECT_EQ(record.runtime_artifact_directory.parent_path(),
            record.staging_directory);
  EXPECT_EQ(rig.chunker.roots(), (std::vector<std::filesystem::path>{
                                     record.runtime_artifact_directory}));
  EXPECT_EQ(record.cleanup_calls, 1U);
  EXPECT_TRUE(record.cleanup_complete);
  EXPECT_TRUE(record.lease_released);
  EXPECT_FALSE(std::filesystem::exists(record.staging_directory));
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));

  auto encoded = rig.storage.read_manifest(request.container_id, 8U);
  ASSERT_TRUE(encoded.has_value()) << encoded.error().diagnostic;
  auto manifest = decode_manifest_v1(*encoded);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().diagnostic;
  EXPECT_EQ(manifest->container_id, request.container_id);
  EXPECT_EQ(manifest->checkpoint_id, result->checkpoint_id);
  EXPECT_EQ(manifest->generation, 8U);
  EXPECT_EQ(manifest->created_at_unix_nanoseconds, kCreatedAtNanoseconds);
  EXPECT_EQ(manifest->source_image_identity,
            "registry.example/application@sha256:0123456789abcdef");
  EXPECT_EQ(manifest->runtime_fingerprint, valid_runtime_fingerprint());
  ASSERT_EQ(manifest->artifacts.size(), 1U);
  ASSERT_EQ(manifest->artifacts.front().chunks.size(), 2U);

  const std::vector<PublicationEvent> expected_publications{
      PublicationEvent{
          .kind = "chunk",
          .container = request.container_id.value(),
          .operation = request.operation_id.value(),
      },
      PublicationEvent{
          .kind = "chunk",
          .container = request.container_id.value(),
          .operation = request.operation_id.value(),
      },
      PublicationEvent{
          .kind = "manifest",
          .container = request.container_id.value(),
          .operation = request.operation_id.value(),
      },
      PublicationEvent{
          .kind = "retention",
          .container = request.container_id.value(),
          .operation = request.operation_id.value(),
      },
  };
  EXPECT_EQ(rig.storage.publication_events(), expected_publications);

  const std::vector<std::pair<std::string, std::string>> expected_transitions{
      {"IDLE", "CAPTURING"},
      {"CAPTURING", "CHUNKING"},
      {"CHUNKING", "COMMITTING"},
      {"COMMITTING", "COMMITTED"},
  };
  EXPECT_EQ(rig.logger.transitions(), expected_transitions);
  expect_all_logs_correlated(rig.logger, request);
  expect_previous_checkpoints_intact(rig);
}

TEST(CheckpointCreatorTest, KeepLastIsForwardedExactlyToRetention) {
  TestRig rig;
  auto request = make_request();
  request.keep_last = 11U;

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_EQ(rig.storage.retention_calls(), (std::vector<std::size_t>{11U}));
}

TEST(CheckpointCreatorTest,
     RetentionRunsAfterConfirmedManifestPublicationAndNotAfterFailedCommit) {
  TestRig successful_rig;
  const auto successful_request = make_request();

  const auto successful = successful_rig.creator.create(successful_request);

  ASSERT_TRUE(successful.has_value()) << successful.error().diagnostic;
  const auto &successful_events = successful_rig.storage.publication_events();
  const auto manifest = std::ranges::find_if(
      successful_events,
      [](const PublicationEvent &event) { return event.kind == "manifest"; });
  const auto retention = std::ranges::find_if(
      successful_events,
      [](const PublicationEvent &event) { return event.kind == "retention"; });
  ASSERT_NE(manifest, successful_events.end());
  ASSERT_NE(retention, successful_events.end());
  EXPECT_LT(manifest, retention);

  TestRig failed_rig;
  const auto failed_request = make_request();
  failed_rig.storage.fail_next_manifest(make_test_error(
      StableErrorCode::storage_error, "injected unconfirmed manifest failure",
      StorageSideEffect::publication_may_be_visible, true));

  const auto failed = failed_rig.creator.create(failed_request);

  ASSERT_FALSE(failed.has_value());
  EXPECT_TRUE(failed_rig.storage.retention_calls().empty());
  EXPECT_TRUE(std::ranges::none_of(
      failed_rig.storage.publication_events(),
      [](const PublicationEvent &event) { return event.kind == "retention"; }));
}

TEST(CheckpointCreatorTest,
     RetentionFailureKeepsCreatedCheckpointAndReportsIncompleteRetention) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.fail_next_retention(make_test_error(
      StableErrorCode::storage_error, "injected retention failure", {}, true));

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_EQ(result->generation, 8U);
  EXPECT_FALSE(result->retention_complete);
}

TEST(CheckpointCreatorTest, CleanupStillRunsAfterRetentionFailure) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.fail_next_retention(make_test_error(
      StableErrorCode::storage_error, "injected retention failure"));

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  ASSERT_EQ(rig.storage.records().size(), 1U);
  EXPECT_EQ(rig.storage.records().front()->cleanup_calls, 1U);
  EXPECT_TRUE(rig.storage.records().front()->cleanup_complete);
  EXPECT_TRUE(result->staging_cleanup_complete);
}

TEST(CheckpointCreatorTest,
     PostCommitRetentionExceptionKeepsSuccessAndStillCleansStaging) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.throw_next_retention();

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_FALSE(result->retention_complete);
  EXPECT_TRUE(result->staging_cleanup_complete);
  ASSERT_EQ(rig.storage.records().size(), 1U);
  EXPECT_EQ(rig.storage.records().front()->cleanup_calls, 1U);
}

TEST(CheckpointCreatorTest, PostCommitCleanupExceptionKeepsCommittedSuccess) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.throw_next_cleanup();

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_TRUE(result->retention_complete);
  EXPECT_FALSE(result->staging_cleanup_complete);
}

TEST(CheckpointCreatorTest,
     CompleteRetentionReportMarksCreatedCheckpointRetentionComplete) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.set_retention_report(RetentionReport{
      .manifests_deleted = 2U,
      .manifests_deferred = 0U,
      .complete = true,
  });

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_TRUE(result->retention_complete);
}

TEST(CheckpointCreatorTest,
     ProductionStorageAndChunkerCommitAndReassembleCapturedArtifact) {
  TemporaryDirectory temporary;
  const auto storage_root = temporary.path() / "production-storage";
  ASSERT_TRUE(std::filesystem::create_directory(storage_root));
  auto storage = FilesystemStorage::open(storage_root);
  ASSERT_TRUE(storage.has_value()) << storage.error().diagnostic;

  Blake3Hasher hasher;
  SnapshotChunker chunker{hasher};
  FakePodmanAdapter adapter;
  FakeClock clock;
  FakeIdGenerator ids{{"production-checkpoint"}};
  FakeLogger logger;
  CheckpointCreator creator{**storage, adapter, chunker, clock, ids, logger};
  const auto request = make_request("production-operation");

  const auto created = creator.create(request);

  ASSERT_TRUE(created.has_value()) << created.error().diagnostic;
  EXPECT_EQ(created->generation, 1U);
  EXPECT_TRUE(created->staging_cleanup_complete);
  auto encoded = (*storage)->read_manifest(request.container_id, 1U);
  ASSERT_TRUE(encoded.has_value()) << encoded.error().diagnostic;
  auto manifest = decode_manifest_v1(*encoded);
  ASSERT_TRUE(manifest.has_value()) << manifest.error().diagnostic;
  ASSERT_EQ(manifest->artifacts.size(), 1U);
  EXPECT_EQ(manifest->artifacts.front().path.value(), "snapshot-1.bin");
  ASSERT_EQ(adapter.calls().size(), 1U);
  EXPECT_FALSE(std::filesystem::exists(
      adapter.calls().front().artifact_directory.parent_path()));

  auto restore_operation = (*storage)->begin_operation(
      request.container_id, make_identifier<OperationId>("production-verify"),
      StorageOperationKind::restore);
  ASSERT_TRUE(restore_operation.has_value())
      << restore_operation.error().diagnostic;
  SnapshotReassembler reassembler{**restore_operation, hasher};
  auto restored = reassembler.reassemble(manifest->artifacts);
  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(
      read_bytes(*restored / manifest->artifacts.front().path.value()),
      make_bytes(static_cast<std::size_t>(ManifestLimits::chunk_bytes) + 17U,
                 0x31U));
}

TEST(CheckpointCreatorTest,
     LockConflictFailsBeforeCaptureAndCarriesCorrelationId) {
  TestRig rig;
  const auto request = make_request();
  auto held = rig.storage.begin_operation(
      request.container_id,
      make_identifier<OperationId>("operation-already-active"),
      StorageOperationKind::create_checkpoint);
  ASSERT_TRUE(held.has_value()) << held.error().diagnostic;

  const auto result = rig.creator.create(request);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, StableErrorCode::conflict);
  EXPECT_TRUE(result.error().retryable);
  expect_correlation(result.error(), request.request_id);
  EXPECT_TRUE(rig.adapter.calls().empty());
  EXPECT_EQ(
      rig.logger.transitions(),
      (std::vector<std::pair<std::string, std::string>>{{"IDLE", "FAILED"}}));
  expect_all_logs_correlated(rig.logger, request);

  auto released = (*held)->cleanup();
  ASSERT_TRUE(released.has_value()) << released.error().diagnostic;
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));
}

enum class FailurePoint : std::uint8_t {
  capture,
  source_read,
  hash,
  chunk_publish,
  manifest_serialization,
  manifest_commit,
};

struct FailureCase {
  FailurePoint point;
  std::string_view name;
  std::string_view failure_state;
  StableErrorCode expected_code;
};

class CheckpointCreatorFailureTest
    : public ::testing::TestWithParam<FailureCase> {};

TEST_P(CheckpointCreatorFailureTest,
       FailurePreservesCanonicalAndReassemblablePreviousCheckpoints) {
  TestRig rig;
  const auto request = make_request();
  const auto test_case = GetParam();

  switch (test_case.point) {
  case FailurePoint::capture:
    rig.adapter.fail_next_capture(make_test_error(
        StableErrorCode::capture_failed, "injected capture failure"));
    break;
  case FailurePoint::source_read:
    rig.chunker.fail_next_source_read(make_test_error(
        StableErrorCode::storage_error, "injected source read failure"));
    break;
  case FailurePoint::hash:
    rig.faulting_hasher.fail_next(
        make_test_error(StableErrorCode::internal, "injected hash failure"));
    break;
  case FailurePoint::chunk_publish:
    rig.storage.fail_next_chunk(make_test_error(
        StableErrorCode::storage_error, "injected chunk publication failure"));
    break;
  case FailurePoint::manifest_serialization:
    rig.adapter.invalidate_manifest_metadata();
    break;
  case FailurePoint::manifest_commit:
    rig.storage.fail_next_manifest(make_test_error(
        StableErrorCode::storage_error, "injected manifest commit failure"));
    break;
  }

  const auto result = rig.creator.create(request);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, test_case.expected_code);
  EXPECT_EQ(result.error().storage_side_effect, StorageSideEffect::none);
  expect_correlation(result.error(), request.request_id);
  ASSERT_EQ(rig.storage.records().size(), 1U);
  const auto &record = *rig.storage.records().front();
  EXPECT_EQ(record.cleanup_calls, 1U);
  EXPECT_TRUE(record.cleanup_complete);
  EXPECT_TRUE(record.lease_released);
  EXPECT_FALSE(std::filesystem::exists(record.staging_directory));
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));
  EXPECT_EQ(rig.logger.transitions(),
            expected_failure_transitions(std::string{test_case.failure_state}));
  expect_all_logs_correlated(rig.logger, request);

  auto listed = rig.storage.list_manifests(request.container_id);
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  EXPECT_EQ(listed->size(), 3U);
  expect_previous_checkpoints_intact(rig);
}

INSTANTIATE_TEST_SUITE_P(
    EveryPreCommitAndCommitSeam, CheckpointCreatorFailureTest,
    ::testing::Values(FailureCase{FailurePoint::capture, "Capture", "CAPTURING",
                                  StableErrorCode::capture_failed},
                      FailureCase{FailurePoint::source_read, "SourceRead",
                                  "CHUNKING", StableErrorCode::storage_error},
                      FailureCase{FailurePoint::hash, "Hash", "CHUNKING",
                                  StableErrorCode::internal},
                      FailureCase{FailurePoint::chunk_publish, "ChunkPublish",
                                  "CHUNKING", StableErrorCode::storage_error},
                      FailureCase{FailurePoint::manifest_serialization,
                                  "ManifestSerialization", "COMMITTING",
                                  StableErrorCode::internal},
                      FailureCase{FailurePoint::manifest_commit,
                                  "ManifestCommit", "COMMITTING",
                                  StableErrorCode::storage_error}),
    [](const ::testing::TestParamInfo<FailureCase> &information) {
      return std::string{information.param.name};
    });

TEST(CheckpointCreatorTest,
     VisibleChunkPublicationFailureIsDefiniteWithoutManifestCommit) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.fail_next_chunk(
      make_test_error(StableErrorCode::storage_error,
                      "injected post-rename-like chunk publication failure",
                      StorageSideEffect::publication_may_be_visible, true));

  const auto result = rig.creator.create(request);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  EXPECT_EQ(rig.logger.transitions(),
            expected_failure_transitions("CHUNKING", "FAILED"));
  auto listed = rig.storage.list_manifests(request.container_id);
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  EXPECT_EQ(listed->size(), 3U);
  expect_previous_checkpoints_intact(rig);
}

TEST(CheckpointCreatorTest,
     PublicationMayBeVisibleManifestFailureBecomesIncomplete) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.fail_next_manifest(
      make_test_error(StableErrorCode::storage_error,
                      "injected post-rename-like manifest commit failure",
                      StorageSideEffect::publication_may_be_visible, true));

  const auto result = rig.creator.create(request);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().storage_side_effect,
            StorageSideEffect::publication_may_be_visible);
  EXPECT_TRUE(result.error().retryable);
  expect_correlation(result.error(), request.request_id);
  EXPECT_EQ(rig.logger.transitions(),
            expected_failure_transitions("COMMITTING", "INCOMPLETE"));
  ASSERT_EQ(rig.storage.records().size(), 1U);
  EXPECT_TRUE(rig.storage.records().front()->cleanup_complete);
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));
  auto listed = rig.storage.list_manifests(request.container_id);
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  EXPECT_EQ(listed->size(), 4U);
  auto visible = rig.storage.read_manifest(request.container_id, 8U);
  ASSERT_TRUE(visible.has_value()) << visible.error().diagnostic;
  EXPECT_TRUE(decode_manifest_v1(*visible));
  ASSERT_FALSE(rig.storage.publication_events().empty());
  EXPECT_EQ(rig.storage.publication_events().back().kind,
            "manifest-visible-unconfirmed");
  expect_previous_checkpoints_intact(rig);
}

TEST(CheckpointCreatorTest,
     CleanupFailureOnErrorMarksIncompleteAndStillReleasesLease) {
  TestRig rig;
  const auto request = make_request();
  rig.adapter.fail_next_capture(make_test_error(StableErrorCode::capture_failed,
                                                "injected capture failure"));
  rig.storage.fail_next_cleanup(make_test_error(
      StableErrorCode::storage_error, "injected cleanup failure", {}, true));

  const auto result = rig.creator.create(request);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, StableErrorCode::capture_failed);
  EXPECT_TRUE(result.error().retryable);
  EXPECT_NE(result.error().diagnostic.find("staging cleanup also failed"),
            std::string::npos);
  expect_correlation(result.error(), request.request_id);
  EXPECT_EQ(rig.logger.transitions(),
            expected_failure_transitions("CAPTURING", "INCOMPLETE"));
  ASSERT_EQ(rig.storage.records().size(), 1U);
  const auto &record = *rig.storage.records().front();
  EXPECT_EQ(record.cleanup_calls, 1U);
  EXPECT_FALSE(record.cleanup_complete);
  EXPECT_TRUE(record.lease_released);
  EXPECT_TRUE(std::filesystem::exists(record.staging_directory));
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));
  expect_previous_checkpoints_intact(rig);
}

TEST(CheckpointCreatorTest,
     RetryUsesFreshIdsArtifactAndCaptureWhileReusingPublishedChunks) {
  TestRig rig{{"checkpoint-attempt-1", "checkpoint-attempt-2"}};
  const auto first_request = make_request("operation-attempt-1");
  const auto retry_request = make_request("operation-attempt-2");
  rig.storage.fail_next_manifest(
      make_test_error(StableErrorCode::storage_error,
                      "injected first manifest commit failure"));

  const auto first = rig.creator.create(first_request);
  ASSERT_FALSE(first.has_value());
  expect_correlation(first.error(), first_request.request_id);
  EXPECT_EQ(rig.storage.unique_chunk_publications(), 2U);

  const auto retry = rig.creator.create(retry_request);

  ASSERT_TRUE(retry.has_value()) << retry.error().diagnostic;
  EXPECT_EQ(retry->operation_id, retry_request.operation_id);
  EXPECT_EQ(retry->checkpoint_id.value(), "checkpoint-attempt-2");
  EXPECT_EQ(retry->generation, 8U);
  ASSERT_EQ(rig.ids.issued_checkpoint_ids().size(), 2U);
  EXPECT_NE(rig.ids.issued_checkpoint_ids().front(),
            rig.ids.issued_checkpoint_ids().back());
  ASSERT_EQ(rig.adapter.calls().size(), 2U);
  ASSERT_EQ(rig.adapter.artifacts().size(), 2U);
  EXPECT_NE(rig.adapter.calls().front().artifact_directory,
            rig.adapter.calls().back().artifact_directory);
  EXPECT_NE(rig.adapter.artifacts().front(), rig.adapter.artifacts().back());
  EXPECT_EQ(rig.chunker.ingest_calls(), 2U);
  EXPECT_EQ(rig.chunker.reused_chunks(), 2U);
  EXPECT_EQ(rig.storage.unique_chunk_publications(), 2U);
  ASSERT_GE(rig.storage.records().size(), 2U);
  EXPECT_EQ(rig.storage.records()[0]->operation, first_request.operation_id);
  EXPECT_EQ(rig.storage.records()[1]->operation, retry_request.operation_id);
  EXPECT_TRUE(rig.storage.records()[0]->cleanup_complete);
  EXPECT_TRUE(rig.storage.records()[1]->cleanup_complete);
  EXPECT_FALSE(rig.storage.has_active_operation(retry_request.container_id));

  auto listed = rig.storage.list_manifests(retry_request.container_id);
  ASSERT_TRUE(listed.has_value()) << listed.error().diagnostic;
  EXPECT_EQ(listed->size(), 4U);
  EXPECT_TRUE(rig.storage.read_manifest(retry_request.container_id, 8U));
  expect_previous_checkpoints_intact(rig);
}

TEST(
    CheckpointCreatorTest,
    CleanupFailureAfterConfirmedCommitKeepsSuccessAndReportsIncompleteCleanup) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.fail_next_cleanup(make_test_error(
      StableErrorCode::storage_error, "injected post-commit cleanup failure"));

  const auto result = rig.creator.create(request);

  ASSERT_TRUE(result.has_value()) << result.error().diagnostic;
  EXPECT_FALSE(result->staging_cleanup_complete);
  EXPECT_EQ(result->generation, 8U);
  auto manifest = rig.storage.read_manifest(request.container_id, 8U);
  EXPECT_TRUE(manifest.has_value());
  EXPECT_EQ(rig.logger.transitions(),
            (std::vector<std::pair<std::string, std::string>>{
                {"IDLE", "CAPTURING"},
                {"CAPTURING", "CHUNKING"},
                {"CHUNKING", "COMMITTING"},
                {"COMMITTING", "COMMITTED"},
            }));
  ASSERT_EQ(rig.storage.records().size(), 1U);
  EXPECT_EQ(rig.storage.records().front()->cleanup_calls, 1U);
  EXPECT_FALSE(rig.storage.records().front()->cleanup_complete);
  EXPECT_TRUE(rig.storage.records().front()->lease_released);
  EXPECT_TRUE(std::filesystem::exists(
      rig.storage.records().front()->staging_directory));
  EXPECT_FALSE(rig.storage.has_active_operation(request.container_id));
  ASSERT_FALSE(rig.logger.entries().empty());
  const auto cleanup_log = std::ranges::find_if(
      rig.logger.entries(), [](const CapturedLogEntry &entry) {
        return entry.message == "checkpoint create staging cleanup failed";
      });
  ASSERT_NE(cleanup_log, rig.logger.entries().end());
  EXPECT_EQ(cleanup_log->fields.at("request_id"), request.request_id.value());
  expect_previous_checkpoints_intact(rig);
}

TEST(CheckpointCreatorTest,
     DifferentContainersCanHaveIndependentOverlappingActiveOperations) {
  TestRig rig{{"checkpoint-primary", "checkpoint-secondary"}};
  const auto primary_request = make_request("operation-primary");
  const auto secondary_container =
      make_identifier<ContainerId>("container-secondary");
  const auto secondary_request =
      make_request("operation-secondary", secondary_container);
  std::optional<CheckpointResult<CreatedCheckpoint>> secondary_result;
  rig.adapter.on_next_capture(
      [&] { secondary_result.emplace(rig.creator.create(secondary_request)); });

  const auto primary_result = rig.creator.create(primary_request);

  ASSERT_TRUE(primary_result.has_value()) << primary_result.error().diagnostic;
  ASSERT_TRUE(secondary_result.has_value());
  ASSERT_TRUE(secondary_result->has_value())
      << secondary_result->error().diagnostic;
  EXPECT_EQ(primary_result->generation, 8U);
  EXPECT_EQ((*secondary_result)->generation, 1U);
  EXPECT_EQ(rig.storage.maximum_active_operations(), 2U);
  EXPECT_FALSE(rig.storage.has_active_operation(primary_request.container_id));
  EXPECT_FALSE(
      rig.storage.has_active_operation(secondary_request.container_id));
  auto primary_manifests =
      rig.storage.list_manifests(primary_request.container_id);
  auto secondary_manifests =
      rig.storage.list_manifests(secondary_request.container_id);
  ASSERT_TRUE(primary_manifests.has_value())
      << primary_manifests.error().diagnostic;
  ASSERT_TRUE(secondary_manifests.has_value())
      << secondary_manifests.error().diagnostic;
  EXPECT_EQ(primary_manifests->size(), 4U);
  EXPECT_EQ(secondary_manifests->size(), 1U);
}

} // namespace
} // namespace gravesentinel::ccm
