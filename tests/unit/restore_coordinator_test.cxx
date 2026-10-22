#include "src/core/restore_coordinator.hxx"

#include "src/core/blake3_hasher.hxx"
#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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

constexpr std::uint32_t kRegularFileMode =
    ManifestArtifactV1::posix_regular_file | 0600U;
constexpr std::uint64_t kCreatedAtNanoseconds = 1'730'000'000'123'456'789ULL;

template <typename Identifier>
[[nodiscard]] Identifier make_identifier(std::string value) {
  auto identifier = Identifier::create(std::move(value));
  if (!identifier) {
    throw std::runtime_error(identifier.error().diagnostic);
  }
  return std::move(*identifier);
}

[[nodiscard]] CheckpointErrorDetail
make_test_error(StableErrorCode code, std::string diagnostic,
                RuntimeSideEffect runtime_side_effect = RuntimeSideEffect::none,
                bool retryable = false) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
      .runtime_side_effect = runtime_side_effect,
  };
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

[[nodiscard]] ColdStartSpec cold_start_spec() {
  return ColdStartSpec{
      .image_reference = "registry.example/application@sha256:cold",
      .argv = {"/usr/bin/application", "serve"},
      .environment = {{.name = "MODE", .value = "test"}},
      .mounts = {},
      .runtime_options = {},
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

[[nodiscard]] std::vector<std::byte>
read_bytes(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input.good()) {
    throw std::runtime_error("could not open a reassembled test artifact");
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
    throw std::runtime_error("could not read a reassembled test artifact");
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
            ("gravesentinel-restore-coordinator-" + std::to_string(::getpid()) +
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

class EventRecorder final {
public:
  void add(std::string event) { events_.push_back(std::move(event)); }

  [[nodiscard]] std::size_t index_of(std::string_view event) const {
    const auto found = std::ranges::find(events_, event);
    return static_cast<std::size_t>(std::distance(events_.begin(), found));
  }

  [[nodiscard]] const std::vector<std::string> &events() const noexcept {
    return events_;
  }

private:
  std::vector<std::string> events_;
};

struct PinRecord {
  std::uint64_t generation;
  std::size_t release_calls{0};
  bool active{true};
  bool release_complete{false};
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

struct SnapshotRecord {
  ChunkHash hash;
  std::vector<std::byte> payload;
  std::string source_image;
};

class FakeStorage;

class FakeStoragePin final : public IStoragePin {
public:
  FakeStoragePin(FakeStorage &storage, std::shared_ptr<PinRecord> record)
      : storage_(storage), record_(std::move(record)) {}

  ~FakeStoragePin() override;

  [[nodiscard]] CheckpointResult<void> release() override;

private:
  FakeStorage &storage_;
  std::shared_ptr<PinRecord> record_;
  bool release_attempted_{false};
};

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
  publish_chunk(const ChunkHash &, std::span<const std::byte>) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "chunk publication is not under test"));
  }

  [[nodiscard]] CheckpointResult<void>
  publish_manifest(const CheckpointId &, std::span<const std::byte>) override {
    return std::unexpected(make_test_error(
        StableErrorCode::internal, "manifest publication is not under test"));
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStoragePin>>
  pin_manifest(std::uint64_t generation) override;

  [[nodiscard]] CheckpointResult<void> cleanup() override;

private:
  FakeStorage &storage_;
  std::shared_ptr<OperationRecord> record_;
};

class FakeStorage final : public IStorage {
public:
  FakeStorage(std::filesystem::path root, EventRecorder &events)
      : root_(std::move(root)), events_(events) {}

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &container, const OperationId &operation,
                  StorageOperationKind kind) override {
    events_.add("storage.begin:" + container.value());
    if (active_containers_.contains(container.value())) {
      return std::unexpected(make_test_error(
          StableErrorCode::conflict,
          "another operation already holds the per-container lease",
          RuntimeSideEffect::none, true));
    }

    active_containers_.insert(container.value());
    maximum_active_operations_ =
        std::max(maximum_active_operations_, active_containers_.size());
    const auto staging =
        root_ / ("operation-" + std::to_string(operation_sequence_++));
    const auto runtime_artifact = staging / "runtime-artifact";
    std::error_code create_error;
    std::filesystem::create_directories(runtime_artifact, create_error);
    if (create_error) {
      active_containers_.erase(container.value());
      return std::unexpected(
          make_test_error(StableErrorCode::storage_error,
                          "could not create fake restore staging directories"));
    }

    auto record = std::make_shared<OperationRecord>(OperationRecord{
        .container = container,
        .operation = operation,
        .kind = kind,
        .staging_directory = staging,
        .runtime_artifact_directory = runtime_artifact,
    });
    operation_records_.push_back(record);
    return std::make_unique<FakeStorageOperation>(*this, std::move(record));
  }

  [[nodiscard]] CheckpointResult<bool>
  contains_chunk(const ContainerId &container,
                 const ChunkHash &hash) const override {
    const auto container_it = chunks_.find(container.value());
    return container_it != chunks_.end() && container_it->second.contains(hash);
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container,
             const ChunkHash &hash) const override {
    ++chunk_read_counts_[hash];
    events_.add("storage.read_chunk");
    const auto container_it = chunks_.find(container.value());
    if (container_it == chunks_.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    const auto chunk_it = container_it->second.find(hash);
    if (chunk_it == container_it->second.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "chunk is absent"));
    }
    return chunk_it->second;
  }

  [[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &container) const override {
    events_.add("storage.list_manifests");
    if (!listed_generations_.empty()) {
      std::vector<StoredManifest> listed;
      listed.reserve(listed_generations_.size());
      for (const auto generation : listed_generations_) {
        listed.push_back(StoredManifest{.generation = generation});
      }
      return listed;
    }

    std::vector<StoredManifest> listed;
    const auto container_it = manifests_.find(container.value());
    if (container_it == manifests_.end()) {
      return listed;
    }
    for (const auto &[generation, encoded] : container_it->second) {
      static_cast<void>(encoded);
      listed.push_back(StoredManifest{.generation = generation});
    }
    return listed;
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &container,
                std::uint64_t generation) const override {
    manifest_reads_.push_back(generation);
    events_.add("storage.read_manifest:" + std::to_string(generation));
    const auto container_it = manifests_.find(container.value());
    if (container_it == manifests_.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "manifest is absent"));
    }
    const auto manifest_it = container_it->second.find(generation);
    if (manifest_it == container_it->second.end()) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "manifest is absent"));
    }
    return manifest_it->second;
  }

  void seed_snapshot(const ContainerId &container, std::uint64_t generation,
                     std::uint8_t seed) {
    auto payload = make_bytes(91U + static_cast<std::size_t>(generation), seed);
    auto hash = hasher_.hash(payload);
    if (!hash) {
      throw std::runtime_error(hash.error().diagnostic);
    }
    chunks_[container.value()][*hash] = payload;

    auto path = RelativeSnapshotPath::create("checkpoint.tar");
    if (!path) {
      throw std::runtime_error(path.error().diagnostic);
    }
    const auto source_image = "image-generation-" + std::to_string(generation);
    const ManifestV1 manifest{
        .container_id = container,
        .checkpoint_id = make_identifier<CheckpointId>(
            "checkpoint-" + std::to_string(generation)),
        .generation = generation,
        .created_at_unix_nanoseconds = kCreatedAtNanoseconds + generation,
        .source_image_identity = source_image,
        .runtime_fingerprint = valid_runtime_fingerprint(),
        .artifacts = {ManifestArtifactV1{
            .path = std::move(*path),
            .size = static_cast<std::uint64_t>(payload.size()),
            .mode = kRegularFileMode,
            .chunks = {ManifestChunkV1{
                .hash = *hash,
                .size = static_cast<std::uint64_t>(payload.size()),
            }},
        }},
    };
    auto encoded = encode_manifest_v1(manifest);
    if (!encoded) {
      throw std::runtime_error(encoded.error().diagnostic);
    }
    manifests_[container.value()][generation] = std::move(*encoded);
    snapshots_.emplace(generation,
                       SnapshotRecord{.hash = *hash,
                                      .payload = std::move(payload),
                                      .source_image = source_image});
  }

  void set_list_order(std::vector<std::uint64_t> generations) {
    listed_generations_ = std::move(generations);
  }

  void corrupt_chunk(const ContainerId &container, std::uint64_t generation) {
    auto &chunk =
        chunks_.at(container.value()).at(snapshots_.at(generation).hash);
    if (chunk.empty()) {
      throw std::logic_error("test snapshot chunk unexpectedly has no bytes");
    }
    const auto value = std::to_integer<std::uint8_t>(chunk.front());
    chunk.front() = static_cast<std::byte>(value ^ 0xFFU);
  }

  void make_manifest_malformed(const ContainerId &container,
                               std::uint64_t generation) {
    manifests_.at(container.value()).at(generation) = {
        std::byte{'{'}, std::byte{'n'}, std::byte{'o'}, std::byte{'t'}};
  }

  void corrupt_manifest_checksum(const ContainerId &container,
                                 std::uint64_t generation) {
    auto &encoded = manifests_.at(container.value()).at(generation);
    constexpr std::string_view prefix = "\"checksum\":\"";
    const std::string text{reinterpret_cast<const char *>(encoded.data()),
                           encoded.size()};
    const auto checksum = text.find(prefix);
    if (checksum == std::string::npos) {
      throw std::logic_error("encoded test manifest has no checksum");
    }
    const auto digit = checksum + prefix.size();
    const auto current =
        static_cast<char>(std::to_integer<unsigned char>(encoded.at(digit)));
    encoded.at(digit) = static_cast<std::byte>(current == '0' ? '1' : '0');
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStoragePin>>
  pin(const std::shared_ptr<OperationRecord> &operation,
      std::uint64_t generation) {
    events_.add("storage.pin:" + std::to_string(generation));
    const auto container_it = manifests_.find(operation->container.value());
    if (container_it == manifests_.end() ||
        !container_it->second.contains(generation)) {
      return std::unexpected(
          make_test_error(StableErrorCode::not_found, "manifest is absent"));
    }
    auto record = std::make_shared<PinRecord>(PinRecord{
        .generation = generation,
    });
    pin_records_.push_back(record);
    return std::make_unique<FakeStoragePin>(*this, std::move(record));
  }

  [[nodiscard]] CheckpointResult<void>
  release_pin(const std::shared_ptr<PinRecord> &record) {
    ++record->release_calls;
    record->active = false;
    events_.add("storage.release_pin:" + std::to_string(record->generation));
    if (pin_release_failures_.contains(record->generation)) {
      return std::unexpected(make_test_error(StableErrorCode::storage_error,
                                             "injected pin release failure"));
    }
    record->release_complete = true;
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  cleanup(const std::shared_ptr<OperationRecord> &record) {
    ++record->cleanup_calls;
    events_.add("storage.cleanup");
    if (std::ranges::any_of(pin_records_,
                            [](const auto &pin) { return pin->active; })) {
      release_lease(record);
      return std::unexpected(
          make_test_error(StableErrorCode::conflict,
                          "cleanup attempted while a pin was active"));
    }
    if (next_cleanup_failure_) {
      auto failure = std::move(*next_cleanup_failure_);
      next_cleanup_failure_.reset();
      release_lease(record);
      return std::unexpected(std::move(failure));
    }

    std::error_code remove_error;
    std::filesystem::remove_all(record->staging_directory, remove_error);
    release_lease(record);
    if (remove_error) {
      return std::unexpected(
          make_test_error(StableErrorCode::storage_error,
                          "could not remove fake restore staging directory"));
    }
    record->cleanup_complete = true;
    return {};
  }

  void release_lease(const std::shared_ptr<OperationRecord> &record) noexcept {
    if (!record->lease_released) {
      active_containers_.erase(record->container.value());
      record->lease_released = true;
    }
  }

  void fail_pin_release(std::uint64_t generation) {
    pin_release_failures_.insert(generation);
  }

  void fail_next_cleanup(CheckpointErrorDetail failure) {
    next_cleanup_failure_ = std::move(failure);
  }

  [[nodiscard]] bool active_pin(std::uint64_t generation) const {
    return std::ranges::any_of(pin_records_, [generation](const auto &record) {
      return record->generation == generation && record->active;
    });
  }

  [[nodiscard]] std::size_t chunk_reads(std::uint64_t generation) const {
    const auto found = chunk_read_counts_.find(snapshots_.at(generation).hash);
    return found == chunk_read_counts_.end() ? 0U : found->second;
  }

  [[nodiscard]] const SnapshotRecord &snapshot(std::uint64_t generation) const {
    return snapshots_.at(generation);
  }

  [[nodiscard]] const std::vector<std::uint64_t> &manifest_reads() const {
    return manifest_reads_;
  }

  [[nodiscard]] const std::vector<std::shared_ptr<PinRecord>> &
  pin_records() const noexcept {
    return pin_records_;
  }

  [[nodiscard]] const std::vector<std::shared_ptr<OperationRecord>> &
  operation_records() const noexcept {
    return operation_records_;
  }

  [[nodiscard]] bool has_active_operation(const ContainerId &container) const {
    return active_containers_.contains(container.value());
  }

  [[nodiscard]] std::size_t maximum_active_operations() const noexcept {
    return maximum_active_operations_;
  }

private:
  using ChunkNamespace = std::map<ChunkHash, std::vector<std::byte>>;
  using ManifestNamespace = std::map<std::uint64_t, std::vector<std::byte>>;

  std::filesystem::path root_;
  EventRecorder &events_;
  Blake3Hasher hasher_;
  std::map<std::string, ChunkNamespace> chunks_;
  std::map<std::string, ManifestNamespace> manifests_;
  std::map<std::uint64_t, SnapshotRecord> snapshots_;
  std::vector<std::uint64_t> listed_generations_;
  mutable std::vector<std::uint64_t> manifest_reads_;
  mutable std::map<ChunkHash, std::size_t> chunk_read_counts_;
  std::set<std::string> active_containers_;
  std::vector<std::shared_ptr<PinRecord>> pin_records_;
  std::vector<std::shared_ptr<OperationRecord>> operation_records_;
  std::set<std::uint64_t> pin_release_failures_;
  std::optional<CheckpointErrorDetail> next_cleanup_failure_;
  std::size_t operation_sequence_{0};
  std::size_t maximum_active_operations_{0};
};

FakeStoragePin::~FakeStoragePin() { static_cast<void>(release()); }

CheckpointResult<void> FakeStoragePin::release() {
  if (release_attempted_) {
    return {};
  }
  release_attempted_ = true;
  return storage_.release_pin(record_);
}

FakeStorageOperation::~FakeStorageOperation() {
  storage_.release_lease(record_);
}

CheckpointResult<std::vector<std::byte>>
FakeStorageOperation::read_chunk(const ChunkHash &hash) {
  return storage_.read_chunk(record_->container, hash);
}

CheckpointResult<std::unique_ptr<IStoragePin>>
FakeStorageOperation::pin_manifest(std::uint64_t generation) {
  return storage_.pin(record_, generation);
}

CheckpointResult<void> FakeStorageOperation::cleanup() {
  return storage_.cleanup(record_);
}

struct RestoreCall {
  std::filesystem::path snapshot_artifact;
  std::string source_image;
  std::vector<std::byte> artifact_contents;
};

class FakePodmanAdapter final : public IPodmanAdapter {
public:
  explicit FakePodmanAdapter(EventRecorder &events) : events_(events) {}

  [[nodiscard]] CheckpointResult<void>
  validate_restore_compatibility(const RuntimeFingerprint &,
                                 std::string_view source_image,
                                 RuntimeCpuAssignment) override {
    const std::string image{source_image};
    validated_images_.push_back(image);
    events_.add("adapter.validate:" + image);
    if (next_validation_hook_) {
      auto hook = std::move(next_validation_hook_);
      next_validation_hook_ = {};
      hook();
    }
    const auto failure = incompatible_images_.find(image);
    if (failure != incompatible_images_.end()) {
      return std::unexpected(failure->second);
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<CapturedSnapshot>
  capture(const ContainerId &, const RuntimeContainerRef &,
          const std::filesystem::path &, RuntimeCpuAssignment) override {
    return std::unexpected(make_test_error(StableErrorCode::internal,
                                           "capture is not under test"));
  }

  [[nodiscard]] CheckpointResult<void>
  stop_and_remove(const ContainerId &,
                  const RuntimeIncarnation &expected_incarnation,
                  RuntimeCpuAssignment) override {
    stopped_incarnations_.push_back(expected_incarnation);
    events_.add("adapter.stop_and_remove");
    if (stop_failure_) {
      auto failure = std::move(*stop_failure_);
      stop_failure_.reset();
      return std::unexpected(std::move(failure));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  restore(const ContainerId &, const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &snapshot_artifact,
          const RuntimeFingerprint &, std::string_view source_image,
          RuntimeCpuAssignment) override {
    const std::string image{source_image};
    events_.add("adapter.restore:" + image);
    std::vector<std::byte> contents;
    try {
      contents = read_bytes(snapshot_artifact);
    } catch (const std::runtime_error &) {
      return std::unexpected(make_test_error(
          StableErrorCode::restore_failed,
          "fake adapter could not read the reassembled artifact"));
    }
    restore_calls_.push_back(RestoreCall{
        .snapshot_artifact = snapshot_artifact,
        .source_image = image,
        .artifact_contents = std::move(contents),
    });
    if (next_restore_hook_) {
      auto hook = std::move(next_restore_hook_);
      next_restore_hook_ = {};
      hook();
    }
    if (!restore_failures_.empty()) {
      auto failure = std::move(restore_failures_.front());
      restore_failures_.pop_front();
      return std::unexpected(std::move(failure));
    }
    return RuntimeIncarnation{
        .reference = runtime_container,
        .runtime_id = "restored-" + image,
    };
  }

  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  cold_start(const ContainerId &, const RuntimeContainerRef &runtime_container,
             const ColdStartSpec &, RuntimeCpuAssignment) override {
    ++cold_start_calls_;
    events_.add("adapter.cold_start");
    if (cold_start_failure_) {
      auto failure = std::move(*cold_start_failure_);
      cold_start_failure_.reset();
      return std::unexpected(std::move(failure));
    }
    return RuntimeIncarnation{
        .reference = runtime_container,
        .runtime_id = "cold-started-runtime",
    };
  }

  [[nodiscard]] CheckpointResult<void>
  cleanup_failed_restore(const ContainerId &, const RuntimeContainerRef &,
                         RuntimeCpuAssignment) override {
    ++cleanup_failed_restore_calls_;
    events_.add("adapter.cleanup_failed_restore");
    if (next_cleanup_hook_) {
      auto hook = std::move(next_cleanup_hook_);
      next_cleanup_hook_ = {};
      hook();
    }
    if (!cleanup_failures_.empty()) {
      auto failure = std::move(cleanup_failures_.front());
      cleanup_failures_.pop_front();
      return std::unexpected(std::move(failure));
    }
    return {};
  }

  [[nodiscard]] CheckpointResult<void>
  confirm_absence(const ContainerId &, const RuntimeContainerRef &,
                  RuntimeCpuAssignment) override {
    ++confirm_absence_calls_;
    events_.add("adapter.confirm_absence");
    if (next_confirm_hook_) {
      auto hook = std::move(next_confirm_hook_);
      next_confirm_hook_ = {};
      hook();
    }
    if (!absence_failures_.empty()) {
      auto failure = std::move(absence_failures_.front());
      absence_failures_.pop_front();
      return std::unexpected(std::move(failure));
    }
    return {};
  }

  void mark_incompatible(std::string image) {
    incompatible_images_.emplace(
        std::move(image), make_test_error(StableErrorCode::incompatible_runtime,
                                          "injected runtime incompatibility"));
  }

  void fail_next_restore(CheckpointErrorDetail failure) {
    restore_failures_.push_back(std::move(failure));
  }

  void fail_next_cleanup(CheckpointErrorDetail failure) {
    cleanup_failures_.push_back(std::move(failure));
  }

  void fail_next_cold_start(CheckpointErrorDetail failure) {
    cold_start_failure_ = std::move(failure);
  }

  void fail_next_confirm_absence(CheckpointErrorDetail failure) {
    absence_failures_.push_back(std::move(failure));
  }

  void fail_next_stop_and_remove(CheckpointErrorDetail failure) {
    stop_failure_ = std::move(failure);
  }

  void on_next_validation(std::function<void()> hook) {
    next_validation_hook_ = std::move(hook);
  }

  void on_next_restore(std::function<void()> hook) {
    next_restore_hook_ = std::move(hook);
  }

  void on_next_cleanup(std::function<void()> hook) {
    next_cleanup_hook_ = std::move(hook);
  }

  void on_next_confirm_absence(std::function<void()> hook) {
    next_confirm_hook_ = std::move(hook);
  }

  [[nodiscard]] const std::vector<std::string> &validated_images() const {
    return validated_images_;
  }

  [[nodiscard]] const std::vector<RestoreCall> &restore_calls() const {
    return restore_calls_;
  }

  [[nodiscard]] const std::vector<RuntimeIncarnation> &
  stopped_incarnations() const {
    return stopped_incarnations_;
  }

  [[nodiscard]] std::size_t cleanup_failed_restore_calls() const noexcept {
    return cleanup_failed_restore_calls_;
  }

  [[nodiscard]] std::size_t confirm_absence_calls() const noexcept {
    return confirm_absence_calls_;
  }

  [[nodiscard]] std::size_t cold_start_calls() const noexcept {
    return cold_start_calls_;
  }

private:
  EventRecorder &events_;
  std::map<std::string, CheckpointErrorDetail> incompatible_images_;
  std::deque<CheckpointErrorDetail> restore_failures_;
  std::deque<CheckpointErrorDetail> cleanup_failures_;
  std::deque<CheckpointErrorDetail> absence_failures_;
  std::optional<CheckpointErrorDetail> stop_failure_;
  std::optional<CheckpointErrorDetail> cold_start_failure_;
  std::function<void()> next_validation_hook_;
  std::function<void()> next_restore_hook_;
  std::function<void()> next_cleanup_hook_;
  std::function<void()> next_confirm_hook_;
  std::vector<std::string> validated_images_;
  std::vector<RestoreCall> restore_calls_;
  std::vector<RuntimeIncarnation> stopped_incarnations_;
  std::size_t cleanup_failed_restore_calls_{0};
  std::size_t confirm_absence_calls_{0};
  std::size_t cold_start_calls_{0};
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
      const auto from = entry.fields.find("from_state");
      const auto to = entry.fields.find("to_state");
      if (from != entry.fields.end() && to != entry.fields.end()) {
        result.emplace_back(from->second, to->second);
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
  TestRig()
      : storage(temporary.path(), events), adapter(events),
        coordinator(storage, adapter, hasher, logger) {}

  [[nodiscard]] static ContainerId default_container() {
    return make_identifier<ContainerId>("container-primary");
  }

  TemporaryDirectory temporary;
  EventRecorder events;
  FakeStorage storage;
  FakePodmanAdapter adapter;
  Blake3Hasher hasher;
  FakeLogger logger;
  RestoreCoordinator coordinator;
};

[[nodiscard]] RestoreRequest
make_request(std::optional<ColdStartSpec> cold_start = cold_start_spec(),
             std::optional<RuntimeIncarnation> source = std::nullopt,
             std::string operation = "operation-restore-1") {
  return RestoreRequest{
      .request_id = make_identifier<RequestId>("request-restore-1"),
      .operation_id = make_identifier<OperationId>(std::move(operation)),
      .container_id = TestRig::default_container(),
      .runtime_container =
          make_identifier<RuntimeContainerRef>("runtime-container-1"),
      .cpu_assignment =
          RuntimeCpuAssignment{
              .container_cpu = 2U,
              .checkpoint_cpu = 3U,
          },
      .expected_source_incarnation = std::move(source),
      .cold_start = std::move(cold_start),
  };
}

void expect_correlation(const CheckpointErrorDetail &failure,
                        const RestoreRequest &request) {
  ASSERT_TRUE(failure.correlation_id.has_value());
  EXPECT_EQ(*failure.correlation_id, request.request_id);
}

void expect_all_logs_correlated(const FakeLogger &logger,
                                const RestoreRequest &request) {
  ASSERT_FALSE(logger.entries().empty());
  for (const auto &entry : logger.entries()) {
    EXPECT_EQ(entry.fields.at("request_id"), request.request_id.value());
    EXPECT_EQ(entry.fields.at("operation_id"), request.operation_id.value());
    EXPECT_EQ(entry.fields.at("container_id"), request.container_id.value());
  }
}

void expect_before(const EventRecorder &events, std::string_view first,
                   std::string_view second) {
  const auto first_index = events.index_of(first);
  const auto second_index = events.index_of(second);
  ASSERT_LT(first_index, events.events().size()) << first;
  ASSERT_LT(second_index, events.events().size()) << second;
  EXPECT_LT(first_index, second_index);
}

void expect_between(const EventRecorder &events, std::string_view first,
                    std::string_view middle, std::string_view last) {
  const auto first_index = events.index_of(first);
  const auto last_index = events.index_of(last);
  ASSERT_LT(first_index, events.events().size()) << first;
  ASSERT_LT(last_index, events.events().size()) << last;
  ASSERT_LT(first_index, last_index);
  const auto begin =
      events.events().begin() + static_cast<std::ptrdiff_t>(first_index + 1U);
  const auto end =
      events.events().begin() + static_cast<std::ptrdiff_t>(last_index);
  EXPECT_NE(std::ranges::find(begin, end, middle), end) << middle;
}

void expect_no_event_after(const EventRecorder &events,
                           std::string_view boundary,
                           std::string_view forbidden) {
  const auto boundary_index = events.index_of(boundary);
  ASSERT_LT(boundary_index, events.events().size()) << boundary;
  const auto begin = events.events().begin() +
                     static_cast<std::ptrdiff_t>(boundary_index + 1U);
  EXPECT_EQ(std::ranges::find(begin, events.events().end(), forbidden),
            events.events().end())
      << forbidden;
}

void expect_operation_cleaned(const TestRig &rig, bool complete = true) {
  ASSERT_EQ(rig.storage.operation_records().size(), 1U);
  const auto &operation = *rig.storage.operation_records().front();
  EXPECT_EQ(operation.cleanup_calls, 1U);
  EXPECT_EQ(operation.cleanup_complete, complete);
  EXPECT_TRUE(operation.lease_released);
  EXPECT_FALSE(rig.storage.has_active_operation(operation.container));
  if (complete) {
    EXPECT_FALSE(std::filesystem::exists(operation.staging_directory));
  }
}

void expect_all_pins_released(const TestRig &rig) {
  for (const auto &pin : rig.storage.pin_records()) {
    EXPECT_EQ(pin->release_calls, 1U);
    EXPECT_FALSE(pin->active);
  }
}

void expect_failed_transition(const FakeLogger &logger) {
  const auto transitions = logger.transitions();
  ASSERT_FALSE(transitions.empty());
  EXPECT_EQ(transitions.back().second, "FAILED");
}

TEST(RestoreCoordinatorTest,
     NewestSnapshotWinsFromUnsortedListingAndLogsSuccessfulStateOrder) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 2U, 0x22U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.seed_snapshot(request.container_id, 5U, 0x55U);
  rig.storage.set_list_order({5U, 2U, 9U});

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->operation_id, request.operation_id);
  EXPECT_EQ(restored->outcome, RestoreOutcome::checkpoint);
  ASSERT_TRUE(restored->checkpoint_id.has_value());
  EXPECT_EQ(restored->checkpoint_id->value(), "checkpoint-9");
  EXPECT_EQ(restored->generation, 9U);
  EXPECT_EQ(restored->incarnation.reference, request.runtime_container);
  EXPECT_TRUE(restored->pin_release_complete);
  EXPECT_TRUE(restored->staging_cleanup_complete);
  EXPECT_EQ(rig.storage.manifest_reads(), (std::vector<std::uint64_t>{9U}));
  ASSERT_EQ(rig.adapter.restore_calls().size(), 1U);
  EXPECT_EQ(rig.adapter.restore_calls().front().source_image,
            "image-generation-9");
  EXPECT_EQ(rig.adapter.restore_calls().front().artifact_contents,
            rig.storage.snapshot(9U).payload);
  EXPECT_EQ(rig.logger.transitions(),
            (std::vector<std::pair<std::string, std::string>>{
                {"VALIDATING", "REASSEMBLING"},
                {"REASSEMBLING", "RESTORING"},
                {"RESTORING", "RESTORED"},
            }));
  expect_all_logs_correlated(rig.logger, request);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     CorruptedNewestChunkFallsBackWithoutCallingRuntimeForThatCandidate) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 4U, 0x44U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.seed_snapshot(request.container_id, 7U, 0x77U);
  rig.storage.set_list_order({4U, 7U, 9U});
  rig.storage.corrupt_chunk(request.container_id, 9U);

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->outcome, RestoreOutcome::checkpoint);
  EXPECT_EQ(restored->generation, 7U);
  EXPECT_EQ(rig.storage.manifest_reads(), (std::vector<std::uint64_t>{9U, 7U}));
  ASSERT_EQ(rig.adapter.restore_calls().size(), 1U);
  EXPECT_EQ(rig.adapter.restore_calls().front().source_image,
            "image-generation-7");
  EXPECT_EQ(rig.adapter.restore_calls().front().artifact_contents,
            rig.storage.snapshot(7U).payload);
  EXPECT_EQ(rig.storage.chunk_reads(9U), 1U);
  EXPECT_EQ(rig.storage.chunk_reads(7U), 1U);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     MalformedAndChecksumCorruptManifestsFallBackInDescendingOrder) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 10U, 0x10U);
  rig.storage.seed_snapshot(request.container_id, 12U, 0x12U);
  rig.storage.seed_snapshot(request.container_id, 13U, 0x13U);
  rig.storage.set_list_order({10U, 13U, 12U});
  rig.storage.make_manifest_malformed(request.container_id, 13U);
  rig.storage.corrupt_manifest_checksum(request.container_id, 12U);

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->generation, 10U);
  EXPECT_EQ(rig.storage.manifest_reads(),
            (std::vector<std::uint64_t>{13U, 12U, 10U}));
  EXPECT_EQ(rig.storage.chunk_reads(13U), 0U);
  EXPECT_EQ(rig.storage.chunk_reads(12U), 0U);
  ASSERT_EQ(rig.adapter.restore_calls().size(), 1U);
  EXPECT_EQ(rig.adapter.restore_calls().front().source_image,
            "image-generation-10");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     IncompatibleNewestFallsBackBeforeReassemblyOrRuntimeMutation) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 8U, 0x88U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.set_list_order({8U, 9U});
  rig.adapter.mark_incompatible("image-generation-9");

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->generation, 8U);
  EXPECT_EQ(
      rig.adapter.validated_images(),
      (std::vector<std::string>{"image-generation-9", "image-generation-8"}));
  EXPECT_EQ(rig.storage.chunk_reads(9U), 0U);
  ASSERT_EQ(rig.adapter.restore_calls().size(), 1U);
  EXPECT_EQ(rig.adapter.restore_calls().front().source_image,
            "image-generation-8");
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 0U);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     PossibleRestoreSideEffectIsCleanedWhilePinnedBeforePreviousGeneration) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 7U, 0x77U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.set_list_order({7U, 9U});
  rig.adapter.fail_next_restore(make_test_error(
      StableErrorCode::restore_failed, "injected restore failure",
      RuntimeSideEffect::incarnation_may_exist));
  rig.adapter.on_next_restore([&] { EXPECT_TRUE(rig.storage.active_pin(9U)); });
  rig.adapter.on_next_cleanup([&] { EXPECT_TRUE(rig.storage.active_pin(9U)); });

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->generation, 7U);
  ASSERT_EQ(rig.adapter.restore_calls().size(), 2U);
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 1U);
  const auto &newest_artifact =
      rig.adapter.restore_calls()[0].snapshot_artifact;
  const auto &previous_artifact =
      rig.adapter.restore_calls()[1].snapshot_artifact;
  EXPECT_NE(newest_artifact, previous_artifact);
  EXPECT_NE(newest_artifact.parent_path(), previous_artifact.parent_path());
  ASSERT_EQ(rig.storage.operation_records().size(), 1U);
  const auto &operation_staging =
      rig.storage.operation_records().front()->staging_directory;
  EXPECT_EQ(newest_artifact.parent_path().parent_path(), operation_staging);
  EXPECT_EQ(previous_artifact.parent_path().parent_path(), operation_staging);
  expect_before(rig.events, "adapter.restore:image-generation-9",
                "adapter.cleanup_failed_restore");
  expect_before(rig.events, "adapter.cleanup_failed_restore",
                "storage.release_pin:9");
  expect_before(rig.events, "storage.release_pin:9",
                "adapter.validate:image-generation-7");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     UnconfirmedFailedRestoreCleanupStopsFallbackAndColdStartImmediately) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 7U, 0x77U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.set_list_order({7U, 9U});
  rig.adapter.fail_next_restore(make_test_error(
      StableErrorCode::restore_failed, "injected restore failure",
      RuntimeSideEffect::incarnation_may_exist));
  rig.adapter.fail_next_cleanup(make_test_error(
      StableErrorCode::restore_failed, "cleanup could not confirm absence",
      RuntimeSideEffect::incarnation_may_exist));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  expect_correlation(restored.error(), request);
  EXPECT_EQ(rig.adapter.restore_calls().size(), 1U);
  EXPECT_EQ(rig.adapter.validated_images(),
            (std::vector<std::string>{"image-generation-9"}));
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 1U);
  EXPECT_EQ(rig.adapter.cold_start_calls(), 0U);
  expect_no_event_after(rig.events, "adapter.cleanup_failed_restore",
                        "adapter.confirm_absence");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     SideEffectFreeRestoreFailureConfirmsAbsenceBeforeFallback) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 7U, 0x77U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.set_list_order({9U, 7U});
  rig.adapter.fail_next_restore(make_test_error(
      StableErrorCode::restore_failed, "side-effect-free restore failure"));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->generation, 7U);
  EXPECT_EQ(rig.adapter.restore_calls().size(), 2U);
  EXPECT_GE(rig.adapter.confirm_absence_calls(), 1U);
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 0U);
  expect_between(rig.events, "adapter.restore:image-generation-9",
                 "adapter.confirm_absence",
                 "adapter.validate:image-generation-7");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     AbsenceConfirmationFailurePreventsColdStartWithoutSnapshots) {
  TestRig rig;
  const auto request = make_request();
  rig.adapter.fail_next_confirm_absence(make_test_error(
      StableErrorCode::unavailable, "injected absence confirmation failure",
      RuntimeSideEffect::incarnation_may_exist, true));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  EXPECT_EQ(restored.error().code, StableErrorCode::unavailable);
  EXPECT_EQ(restored.error().runtime_side_effect,
            RuntimeSideEffect::incarnation_may_exist);
  expect_correlation(restored.error(), request);
  EXPECT_EQ(rig.adapter.confirm_absence_calls(), 1U);
  EXPECT_TRUE(rig.adapter.restore_calls().empty());
  EXPECT_EQ(rig.adapter.cold_start_calls(), 0U);
  EXPECT_TRUE(rig.storage.pin_records().empty());
  expect_operation_cleaned(rig);
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     AllSnapshotsUnusableColdStartsOnlyAfterConfirmedAbsence) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 7U, 0x77U);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.set_list_order({7U, 9U});
  rig.adapter.mark_incompatible("image-generation-9");
  rig.adapter.mark_incompatible("image-generation-7");

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->outcome, RestoreOutcome::cold_start);
  EXPECT_FALSE(restored->checkpoint_id.has_value());
  EXPECT_FALSE(restored->generation.has_value());
  EXPECT_EQ(rig.adapter.restore_calls().size(), 0U);
  EXPECT_EQ(rig.adapter.confirm_absence_calls(), 1U);
  EXPECT_EQ(rig.adapter.cold_start_calls(), 1U);
  EXPECT_EQ(rig.storage.chunk_reads(9U), 0U);
  EXPECT_EQ(rig.storage.chunk_reads(7U), 0U);
  EXPECT_EQ(
      rig.adapter.validated_images(),
      (std::vector<std::string>{"image-generation-9", "image-generation-7"}));
  expect_before(rig.events, "adapter.confirm_absence", "adapter.cold_start");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     MissingColdStartSpecFailsAfterSnapshotsWithoutStartingRuntime) {
  TestRig rig;
  const auto request = make_request(std::nullopt);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.adapter.mark_incompatible("image-generation-9");

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  expect_correlation(restored.error(), request);
  EXPECT_EQ(rig.adapter.restore_calls().size(), 0U);
  EXPECT_EQ(rig.adapter.cold_start_calls(), 0U);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     ColdStartFailureWithPossibleSideEffectCleansUpAndNeverStartsAgain) {
  TestRig rig;
  const auto request = make_request();
  rig.adapter.fail_next_cold_start(make_test_error(
      StableErrorCode::cold_start_failed, "injected cold-start failure",
      RuntimeSideEffect::incarnation_may_exist));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  expect_correlation(restored.error(), request);
  EXPECT_EQ(rig.adapter.confirm_absence_calls(), 1U);
  EXPECT_EQ(rig.adapter.cold_start_calls(), 1U);
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 1U);
  expect_before(rig.events, "adapter.confirm_absence", "adapter.cold_start");
  expect_before(rig.events, "adapter.cold_start",
                "adapter.cleanup_failed_restore");
  expect_operation_cleaned(rig);
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     KnownSourceIsStoppedBeforeFirstRuntimeStartAndOnlyOnce) {
  TestRig rig;
  const auto runtime_reference =
      make_identifier<RuntimeContainerRef>("runtime-container-1");
  const RuntimeIncarnation source{
      .reference = runtime_reference,
      .runtime_id = "known-source-runtime-id",
  };
  const auto request = make_request(cold_start_spec(), source);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  ASSERT_EQ(rig.adapter.stopped_incarnations().size(), 1U);
  EXPECT_EQ(rig.adapter.stopped_incarnations().front(), source);
  expect_before(rig.events, "adapter.stop_and_remove",
                "adapter.restore:image-generation-9");
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     StopKnownSourceFailureReleasesPinAndPreventsRuntimeStart) {
  TestRig rig;
  const RuntimeIncarnation source{
      .reference = make_identifier<RuntimeContainerRef>("runtime-container-1"),
      .runtime_id = "known-source-runtime-id",
  };
  const auto request = make_request(cold_start_spec(), source);
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.adapter.fail_next_stop_and_remove(make_test_error(
      StableErrorCode::restore_failed, "injected stop-and-remove failure"));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  EXPECT_EQ(restored.error().code, StableErrorCode::restore_failed);
  expect_correlation(restored.error(), request);
  ASSERT_EQ(rig.adapter.stopped_incarnations().size(), 1U);
  EXPECT_EQ(rig.adapter.stopped_incarnations().front(), source);
  EXPECT_TRUE(rig.adapter.restore_calls().empty());
  EXPECT_EQ(rig.adapter.cold_start_calls(), 0U);
  EXPECT_EQ(rig.adapter.cleanup_failed_restore_calls(), 0U);
  ASSERT_EQ(rig.storage.pin_records().size(), 1U);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     MismatchedKnownSourceReferenceIsRejectedWithoutRuntimeMutation) {
  TestRig rig;
  const RuntimeIncarnation mismatched_source{
      .reference =
          make_identifier<RuntimeContainerRef>("different-runtime-container"),
      .runtime_id = "known-source-runtime-id",
  };
  const auto request = make_request(cold_start_spec(), mismatched_source);

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_FALSE(restored.has_value());
  EXPECT_EQ(restored.error().code, StableErrorCode::invalid_argument);
  expect_correlation(restored.error(), request);
  EXPECT_TRUE(rig.adapter.stopped_incarnations().empty());
  EXPECT_TRUE(rig.adapter.restore_calls().empty());
  EXPECT_EQ(rig.adapter.cold_start_calls(), 0U);
  if (!rig.storage.operation_records().empty()) {
    expect_operation_cleaned(rig);
  }
  expect_failed_transition(rig.logger);
}

TEST(RestoreCoordinatorTest,
     ConcurrentCreateForSameContainerIsRejectedWhileRestoreLeaseIsHeld) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  std::optional<CheckpointResult<std::unique_ptr<IStorageOperation>>>
      overlapping_create;
  rig.adapter.on_next_validation([&] {
    overlapping_create.emplace(rig.storage.begin_operation(
        request.container_id,
        make_identifier<OperationId>("concurrent-create-operation"),
        StorageOperationKind::create_checkpoint));
  });

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  ASSERT_TRUE(overlapping_create.has_value());
  ASSERT_FALSE(overlapping_create->has_value());
  EXPECT_EQ(overlapping_create->error().code, StableErrorCode::conflict);
  EXPECT_TRUE(overlapping_create->error().retryable);
  EXPECT_EQ(rig.storage.maximum_active_operations(), 1U);
  expect_all_pins_released(rig);
  expect_operation_cleaned(rig);
}

TEST(RestoreCoordinatorTest,
     SuccessfulRestoreReportsAttemptedPinAndStagingCleanupFailures) {
  TestRig rig;
  const auto request = make_request();
  rig.storage.seed_snapshot(request.container_id, 9U, 0x99U);
  rig.storage.fail_pin_release(9U);
  rig.storage.fail_next_cleanup(make_test_error(
      StableErrorCode::storage_error, "injected staging cleanup failure"));

  const auto restored = rig.coordinator.restore_or_restart(request);

  ASSERT_TRUE(restored.has_value()) << restored.error().diagnostic;
  EXPECT_EQ(restored->outcome, RestoreOutcome::checkpoint);
  EXPECT_FALSE(restored->pin_release_complete);
  EXPECT_FALSE(restored->staging_cleanup_complete);
  ASSERT_EQ(rig.storage.pin_records().size(), 1U);
  EXPECT_EQ(rig.storage.pin_records().front()->release_calls, 1U);
  expect_operation_cleaned(rig, false);
  EXPECT_EQ(rig.logger.transitions().back().second, "RESTORED");
}

} // namespace
} // namespace gravesentinel::ccm
