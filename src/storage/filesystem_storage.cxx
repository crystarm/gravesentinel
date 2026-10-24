#include "src/storage/filesystem_storage.hxx"

#include "src/crypto/blake3.hxx"
#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

constexpr std::uint32_t kDirectoryMode = 0700U;
constexpr std::uint32_t kFileMode = 0600U;
constexpr std::size_t kMaximumIdentifierBytes = 256U;
constexpr std::size_t kMaximumTemporaryAttempts = 64U;
constexpr std::size_t kManifestNameDigits = 20U;
constexpr std::size_t kMaximumDirectoryEntries = 131072U;
constexpr std::size_t kCandidateRecordMaximumBytes = 1024U;
constexpr std::size_t kHashHexBytes = ChunkHash::digest_size * 2U;
constexpr std::size_t kPublicationNonceHexBytes = 32U;
constexpr std::string_view kManifestSuffix = ".json";
constexpr std::string_view kTemporaryPrefix = ".tmp-";
constexpr std::string_view kCandidatesDirectory = "gc-candidates";

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
  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

  void reset() noexcept {
    if (valid()) {
      operations_->close(value_);
      value_ = -1;
    }
  }

private:
  std::shared_ptr<IFileOperations> operations_;
  int value_{-1};
};

struct StorageContext {
  std::filesystem::path root;
  std::shared_ptr<IFileOperations> operations;
  UniqueFileDescriptor root_fd;
  UniqueFileDescriptor containers_fd;
  UniqueFileDescriptor locks_fd;
  UniqueFileDescriptor temporary_fd;
};

struct ContainerDirectories {
  std::string key;
  UniqueFileDescriptor container;
  UniqueFileDescriptor manifests;
  UniqueFileDescriptor chunks;
  UniqueFileDescriptor staging;
};

[[nodiscard]] StableErrorCode stable_code_for_errno(int error) noexcept {
  switch (error) {
  case ENOSPC:
#ifdef EDQUOT
  case EDQUOT:
#endif
    return StableErrorCode::resource_exhausted;
  case EACCES:
  case EPERM:
  case EROFS:
    return StableErrorCode::permission_denied;
  case ENOENT:
    return StableErrorCode::not_found;
  case EEXIST:
    return StableErrorCode::already_exists;
  case EAGAIN:
#if EWOULDBLOCK != EAGAIN
  case EWOULDBLOCK:
#endif
    return StableErrorCode::conflict;
  default:
    return StableErrorCode::storage_error;
  }
}

[[nodiscard]] CheckpointErrorDetail
filesystem_error(std::string_view operation, int error,
                 StorageSideEffect side_effect = StorageSideEffect::none) {
  return CheckpointErrorDetail{
      .code = stable_code_for_errno(error),
      .diagnostic = std::string{operation} + ": " +
                    std::error_code(error, std::generic_category()).message(),
      .retryable =
          error == EINTR || error == EAGAIN || error == EBUSY || error == EIO,
      .correlation_id = std::nullopt,
      .storage_side_effect = side_effect,
  };
}

[[nodiscard]] CheckpointErrorDetail logical_error(StableErrorCode code,
                                                  std::string diagnostic) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = false,
      .correlation_id = std::nullopt,
      .storage_side_effect = StorageSideEffect::none,
  };
}

[[nodiscard]] CheckpointErrorDetail
required_namespace_error(CheckpointErrorDetail error,
                         std::string_view component) {
  if (error.code == StableErrorCode::not_found) {
    return logical_error(
        StableErrorCode::corrupted_snapshot,
        std::string{"container namespace is missing required component: "} +
            std::string{component});
  }
  return error;
}

[[nodiscard]] std::string bytes_to_hex(std::span<const std::byte> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    encoded.push_back(digits[(value >> 4U) & 0x0FU]);
    encoded.push_back(digits[value & 0x0FU]);
  }
  return encoded;
}

[[nodiscard]] std::string hash_to_hex(const ChunkHash &hash) {
  return bytes_to_hex(hash.bytes());
}

[[nodiscard]] CheckpointResult<std::string>
identifier_key(std::string_view domain, std::string_view value,
               std::string_view field) {
  if (value.empty() || value.size() > kMaximumIdentifierBytes) {
    return std::unexpected(logical_error(
        StableErrorCode::invalid_argument,
        std::string{field} + " must contain between 1 and 256 bytes"));
  }
  if (value.find('\0') != std::string_view::npos) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      std::string{field} + " must not contain NUL bytes"));
  }

  std::vector<std::byte> input;
  input.reserve(domain.size() + 1U + value.size());
  const auto domain_bytes =
      std::as_bytes(std::span(domain.data(), domain.size()));
  input.insert(input.end(), domain_bytes.begin(), domain_bytes.end());
  input.push_back(std::byte{0});
  const auto value_bytes = std::as_bytes(std::span(value.data(), value.size()));
  input.insert(input.end(), value_bytes.begin(), value_bytes.end());
  return hash_to_hex(blake3_hash(input));
}

template <typename Action>
[[nodiscard]] auto retry_interrupted(Action &&action) {
  auto result = action();
  while (!result && result.error() == EINTR) {
    result = action();
  }
  return result;
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

[[nodiscard]] bool is_container_key(std::string_view name) noexcept {
  return name.size() == kHashHexBytes && is_lower_hex(name);
}

[[nodiscard]] bool is_chunk_prefix(std::string_view name) noexcept {
  return name.size() == 2U && is_lower_hex(name);
}

[[nodiscard]] bool is_chunk_name(std::string_view name) noexcept {
  return name.size() == kHashHexBytes && is_lower_hex(name);
}

[[nodiscard]] bool is_publication_temporary(std::string_view name) noexcept {
  return name.size() == kTemporaryPrefix.size() + kPublicationNonceHexBytes &&
         name.starts_with(kTemporaryPrefix) &&
         is_lower_hex(name.substr(kTemporaryPrefix.size()));
}

[[nodiscard]] std::int64_t
unix_nanoseconds(std::chrono::system_clock::time_point value) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             value.time_since_epoch())
      .count();
}

[[nodiscard]] bool grace_elapsed(std::int64_t first_observed_nanoseconds,
                                 std::int64_t now_nanoseconds,
                                 std::chrono::seconds grace_period) noexcept {
  if (grace_period.count() < 0 ||
      now_nanoseconds < first_observed_nanoseconds) {
    return false;
  }
  const auto elapsed = static_cast<std::uint64_t>(now_nanoseconds) -
                       static_cast<std::uint64_t>(first_observed_nanoseconds);
  const auto grace_seconds = static_cast<std::uint64_t>(grace_period.count());
  if (grace_seconds >
      std::numeric_limits<std::uint64_t>::max() / 1000000000ULL) {
    return false;
  }
  return elapsed >= grace_seconds * 1000000000ULL;
}

[[nodiscard]] std::optional<std::int64_t>
timestamp_nanoseconds(FileTimestamp timestamp) noexcept {
  constexpr auto billion = 1000000000LL;
  if (timestamp.nanoseconds >= static_cast<std::uint32_t>(billion) ||
      timestamp.seconds > (std::numeric_limits<std::int64_t>::max() -
                           static_cast<std::int64_t>(timestamp.nanoseconds)) /
                              billion ||
      timestamp.seconds < std::numeric_limits<std::int64_t>::min() / billion) {
    return std::nullopt;
  }
  return timestamp.seconds * billion +
         static_cast<std::int64_t>(timestamp.nanoseconds);
}

[[nodiscard]] bool is_stale(const FileStatus &status,
                            const StartupRecoveryOptions &options) noexcept {
  const auto modified = timestamp_nanoseconds(status.modification_time);
  return modified.has_value() &&
         grace_elapsed(*modified, unix_nanoseconds(options.now),
                       options.grace_period);
}

[[nodiscard]] CheckpointResult<void>
sync_directory(const std::shared_ptr<IFileOperations> &operations, int fd,
               std::string_view description,
               StorageSideEffect side_effect = StorageSideEffect::none) {
  while (true) {
    auto result = operations->sync_directory(fd);
    if (result) {
      return {};
    }
    if (result.error() != EINTR) {
      return std::unexpected(
          filesystem_error(description, result.error(), side_effect));
    }
  }
}

[[nodiscard]] CheckpointResult<void>
sync_file(const std::shared_ptr<IFileOperations> &operations, int fd,
          std::string_view description) {
  while (true) {
    auto result = operations->sync_file(fd);
    if (result) {
      return {};
    }
    if (result.error() != EINTR) {
      return std::unexpected(filesystem_error(description, result.error()));
    }
  }
}

[[nodiscard]] CheckpointResult<UniqueFileDescriptor>
open_directory(const std::shared_ptr<IFileOperations> &operations,
               int parent_fd, std::string_view name,
               std::string_view description) {
  while (true) {
    auto opened = operations->open_directory_at(parent_fd, name);
    if (opened) {
      UniqueFileDescriptor descriptor{operations, *opened};
      auto status = retry_interrupted(
          [&] { return operations->status(descriptor.get()); });
      if (!status) {
        return std::unexpected(filesystem_error(description, status.error()));
      }
      if (status->type != FileType::directory) {
        return std::unexpected(
            logical_error(StableErrorCode::storage_error,
                          std::string{description} + " is not a directory"));
      }
      return descriptor;
    }
    if (opened.error() != EINTR) {
      return std::unexpected(filesystem_error(description, opened.error()));
    }
  }
}

[[nodiscard]] CheckpointResult<StorageDirectoryIdentity>
directory_identity(const std::shared_ptr<IFileOperations> &operations, int fd,
                   std::string_view description) {
  while (true) {
    auto status = retry_interrupted([&] { return operations->status(fd); });
    if (status) {
      if (status->type != FileType::directory) {
        return std::unexpected(
            logical_error(StableErrorCode::storage_error,
                          std::string{description} + " is not a directory"));
      }
      return StorageDirectoryIdentity{
          .device = status->device,
          .inode = status->inode,
      };
    }
    if (status.error() != EINTR) {
      return std::unexpected(filesystem_error(description, status.error()));
    }
  }
}

[[nodiscard]] CheckpointResult<UniqueFileDescriptor>
ensure_directory(const std::shared_ptr<IFileOperations> &operations,
                 int parent_fd, std::string_view name,
                 std::string_view description) {
  bool created = false;
  while (true) {
    auto result =
        operations->create_directory_at(parent_fd, name, kDirectoryMode);
    if (result) {
      created = *result;
      break;
    }
    if (result.error() != EINTR) {
      return std::unexpected(filesystem_error(description, result.error()));
    }
  }
  if (auto result =
          sync_directory(operations, parent_fd,
                         created ? "sync newly created directory parent"
                                 : "confirm existing directory durability");
      !result) {
    return std::unexpected(result.error());
  }
  return open_directory(operations, parent_fd, name, description);
}

[[nodiscard]] CheckpointResult<std::string>
temporary_name(const std::shared_ptr<IFileOperations> &operations) {
  std::array<std::byte, 16> nonce{};
  auto generated = operations->random_bytes(nonce);
  if (!generated) {
    return std::unexpected(filesystem_error(
        "generate publication temporary name", generated.error()));
  }
  return ".tmp-" + bytes_to_hex(nonce);
}

[[nodiscard]] CheckpointResult<std::vector<std::byte>>
read_regular_file(const std::shared_ptr<IFileOperations> &operations,
                  int parent_fd, std::string_view name, std::size_t limit,
                  std::string_view description) {
  UniqueFileDescriptor file;
  while (true) {
    auto opened = operations->open_regular_file_at(parent_fd, name);
    if (opened) {
      file = UniqueFileDescriptor{operations, *opened};
      break;
    }
    if (opened.error() != EINTR) {
      return std::unexpected(filesystem_error(description, opened.error()));
    }
  }

  auto status =
      retry_interrupted([&] { return operations->status(file.get()); });
  if (!status) {
    return std::unexpected(filesystem_error(description, status.error()));
  }
  if (status->type != FileType::regular_file) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      std::string{description} + " is not a regular file"));
  }
  if (status->size > limit) {
    return std::unexpected(
        logical_error(StableErrorCode::resource_exhausted,
                      std::string{description} + " exceeds its size limit"));
  }

  const auto size = static_cast<std::size_t>(status->size);
  std::vector<std::byte> bytes(size);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto result = operations->read(file.get(),
                                   std::span<std::byte>{bytes}.subspan(offset));
    if (!result) {
      if (result.error() == EINTR) {
        continue;
      }
      return std::unexpected(filesystem_error(description, result.error()));
    }
    if (*result == 0U) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          std::string{description} + " ended before its recorded size"));
    }
    if (*result > bytes.size() - offset) {
      return std::unexpected(
          logical_error(StableErrorCode::internal,
                        "file operations returned an invalid read byte count"));
    }
    offset += *result;
  }

  std::array<std::byte, 1> extra{};
  while (true) {
    auto result = operations->read(file.get(), extra);
    if (!result) {
      if (result.error() == EINTR) {
        continue;
      }
      return std::unexpected(filesystem_error(description, result.error()));
    }
    if (*result != 0U) {
      return std::unexpected(logical_error(StableErrorCode::corrupted_snapshot,
                                           std::string{description} +
                                               " changed while it was read"));
    }
    break;
  }
  return bytes;
}

[[nodiscard]] CheckpointResult<void>
write_all(const std::shared_ptr<IFileOperations> &operations, int fd,
          std::span<const std::byte> bytes, std::string_view description) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto result = operations->write(fd, bytes.subspan(offset));
    if (!result) {
      if (result.error() == EINTR) {
        continue;
      }
      return std::unexpected(filesystem_error(description, result.error()));
    }
    if (*result == 0U || *result > bytes.size() - offset) {
      return std::unexpected(logical_error(
          StableErrorCode::storage_error,
          "file write made no progress or returned an invalid byte count"));
    }
    offset += *result;
  }
  return {};
}

void remove_temporary_best_effort(
    const std::shared_ptr<IFileOperations> &operations, int directory_fd,
    std::string_view name) noexcept {
  auto removed = retry_interrupted(
      [&] { return operations->remove_file_at(directory_fd, name); });
  if (removed) {
    static_cast<void>(operations->sync_directory(directory_fd));
  }
}

[[nodiscard]] CheckpointResult<std::vector<std::string>>
list_directory(const std::shared_ptr<IFileOperations> &operations,
               int directory_fd, std::string_view description) {
  auto entries = retry_interrupted([&] {
    return operations->list_directory(directory_fd, kMaximumDirectoryEntries);
  });
  if (!entries) {
    return std::unexpected(filesystem_error(description, entries.error()));
  }
  return std::move(*entries);
}

[[nodiscard]] CheckpointResult<bool> durable_remove_file(
    const std::shared_ptr<IFileOperations> &operations, int directory_fd,
    std::string_view name, std::string_view remove_description,
    std::string_view sync_description, bool allow_missing = false) {
  auto removed = retry_interrupted(
      [&] { return operations->remove_file_at(directory_fd, name); });
  if (!removed) {
    if (allow_missing && removed.error() == ENOENT) {
      return false;
    }
    return std::unexpected(
        filesystem_error(remove_description, removed.error()));
  }
  if (auto synced = sync_directory(operations, directory_fd, sync_description);
      !synced) {
    return std::unexpected(synced.error());
  }
  return true;
}

[[nodiscard]] CheckpointResult<void>
durable_publish(const std::shared_ptr<IFileOperations> &operations,
                int directory_fd, std::string_view destination_name,
                std::span<const std::byte> bytes) {
  std::string temporary;
  UniqueFileDescriptor file;
  for (std::size_t attempt = 0; attempt < kMaximumTemporaryAttempts;
       ++attempt) {
    auto generated_name = temporary_name(operations);
    if (!generated_name) {
      return std::unexpected(generated_name.error());
    }
    temporary = std::move(*generated_name);
    auto created = operations->create_exclusive_file_at(directory_fd, temporary,
                                                        kFileMode);
    if (created) {
      file = UniqueFileDescriptor{operations, *created};
      break;
    }
    if (created.error() == EINTR || created.error() == EEXIST) {
      continue;
    }
    return std::unexpected(
        filesystem_error("create publication temporary file", created.error()));
  }
  if (!file.valid()) {
    return std::unexpected(logical_error(
        StableErrorCode::resource_exhausted,
        "could not allocate a unique publication temporary file"));
  }

  if (auto result = write_all(operations, file.get(), bytes,
                              "write publication temporary file");
      !result) {
    file.reset();
    remove_temporary_best_effort(operations, directory_fd, temporary);
    return result;
  }
  auto status =
      retry_interrupted([&] { return operations->status(file.get()); });
  if (!status) {
    const auto error =
        filesystem_error("inspect publication temporary file", status.error());
    file.reset();
    remove_temporary_best_effort(operations, directory_fd, temporary);
    return std::unexpected(error);
  }
  if (status->type != FileType::regular_file || status->size != bytes.size()) {
    file.reset();
    remove_temporary_best_effort(operations, directory_fd, temporary);
    return std::unexpected(logical_error(
        StableErrorCode::storage_error,
        "publication temporary file has an unexpected type or size"));
  }
  if (auto result =
          sync_file(operations, file.get(), "sync publication temporary file");
      !result) {
    file.reset();
    remove_temporary_best_effort(operations, directory_fd, temporary);
    return result;
  }
  file.reset();

  while (true) {
    auto renamed = operations->rename_no_replace_at(
        directory_fd, temporary, directory_fd, destination_name);
    if (renamed) {
      break;
    }
    if (renamed.error() == EINTR) {
      continue;
    }
    remove_temporary_best_effort(operations, directory_fd, temporary);
    return std::unexpected(
        filesystem_error("atomically publish immutable file", renamed.error()));
  }

  if (auto result =
          sync_directory(operations, directory_fd, "sync publication directory",
                         StorageSideEffect::publication_may_be_visible);
      !result) {
    return result;
  }
  return {};
}

[[nodiscard]] std::string manifest_name(std::uint64_t generation) {
  std::array<char, kManifestNameDigits> digits{};
  digits.fill('0');
  std::array<char, kManifestNameDigits> encoded{};
  const auto result = std::to_chars(
      encoded.data(), encoded.data() + encoded.size(), generation);
  const auto count = static_cast<std::size_t>(result.ptr - encoded.data());
  std::copy(encoded.begin(),
            encoded.begin() + static_cast<std::ptrdiff_t>(count),
            digits.end() - static_cast<std::ptrdiff_t>(count));
  return std::string{digits.begin(), digits.end()} +
         std::string{kManifestSuffix};
}

[[nodiscard]] std::optional<std::uint64_t>
parse_manifest_name(std::string_view name) {
  if (name.size() != kManifestNameDigits + kManifestSuffix.size() ||
      !name.ends_with(kManifestSuffix)) {
    return std::nullopt;
  }
  const auto digits = name.substr(0, kManifestNameDigits);
  if (!std::all_of(digits.begin(), digits.end(),
                   [](char value) { return value >= '0' && value <= '9'; })) {
    return std::nullopt;
  }
  std::uint64_t generation = 0;
  const auto result =
      std::from_chars(digits.data(), digits.data() + digits.size(), generation);
  if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() ||
      generation == 0U) {
    return std::nullopt;
  }
  return generation;
}

[[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
list_manifest_references(const std::shared_ptr<IFileOperations> &operations,
                         int manifests_fd) {
  FileOperationResult<std::vector<std::string>> entries =
      std::unexpected(EINTR);
  while (!entries && entries.error() == EINTR) {
    entries =
        operations->list_directory(manifests_fd, kMaximumDirectoryEntries);
  }
  if (!entries) {
    return std::unexpected(
        filesystem_error("list manifest directory", entries.error()));
  }

  std::vector<StoredManifest> manifests;
  for (const auto &entry : *entries) {
    const auto generation = parse_manifest_name(entry);
    if (!generation.has_value()) {
      continue;
    }
    UniqueFileDescriptor file;
    while (true) {
      auto opened = operations->open_regular_file_at(manifests_fd, entry);
      if (opened) {
        file = UniqueFileDescriptor{operations, *opened};
        break;
      }
      if (opened.error() != EINTR) {
        return std::unexpected(
            filesystem_error("open published manifest", opened.error()));
      }
    }
    auto status =
        retry_interrupted([&] { return operations->status(file.get()); });
    if (!status) {
      return std::unexpected(
          filesystem_error("inspect published manifest", status.error()));
    }
    if (status->type != FileType::regular_file ||
        status->size > ManifestLimits::maximum_encoded_bytes) {
      return std::unexpected(
          logical_error(StableErrorCode::corrupted_snapshot,
                        "published manifest has an invalid type or size"));
    }
    manifests.push_back(StoredManifest{.generation = *generation});
  }
  std::ranges::sort(manifests, std::greater{}, &StoredManifest::generation);
  return manifests;
}

struct StrictManifestSnapshot {
  std::vector<StoredManifest> manifests;
  std::map<std::string, std::uint64_t> live_chunks;
};

[[nodiscard]] CheckpointResult<FileStatus>
regular_file_status(const std::shared_ptr<IFileOperations> &operations,
                    int parent_fd, std::string_view name,
                    std::string_view description) {
  auto opened = retry_interrupted(
      [&] { return operations->open_regular_file_at(parent_fd, name); });
  if (!opened) {
    return std::unexpected(filesystem_error(description, opened.error()));
  }
  UniqueFileDescriptor file{operations, *opened};
  auto status =
      retry_interrupted([&] { return operations->status(file.get()); });
  if (!status) {
    return std::unexpected(filesystem_error(description, status.error()));
  }
  if (status->type != FileType::regular_file) {
    return std::unexpected(
        logical_error(StableErrorCode::storage_error,
                      std::string{description} + " is not a regular file"));
  }
  return *status;
}

[[nodiscard]] CheckpointResult<StrictManifestSnapshot>
scan_manifests_strict(const std::shared_ptr<IFileOperations> &operations,
                      int manifests_fd, int chunks_fd,
                      const ContainerId &container_id) {
  if (auto synced =
          sync_directory(operations, manifests_fd,
                         "confirm manifest root durability before maintenance");
      !synced) {
    return std::unexpected(synced.error());
  }
  auto entries =
      list_directory(operations, manifests_fd, "strictly list manifest root");
  if (!entries) {
    return std::unexpected(entries.error());
  }

  StrictManifestSnapshot snapshot;
  for (const auto &entry : *entries) {
    const auto generation = parse_manifest_name(entry);
    if (!generation.has_value()) {
      if (is_publication_temporary(entry)) {
        continue;
      }
      return std::unexpected(
          logical_error(StableErrorCode::corrupted_snapshot,
                        "manifest root contains an unknown entry"));
    }

    auto encoded = read_regular_file(operations, manifests_fd, entry,
                                     ManifestLimits::maximum_encoded_bytes,
                                     "strictly read published manifest");
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    auto manifest = decode_manifest_v1(*encoded);
    if (!manifest || manifest->container_id != container_id ||
        manifest->generation != *generation) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "published manifest failed strict namespace validation"));
    }

    snapshot.manifests.push_back(StoredManifest{.generation = *generation});
    for (const auto &artifact : manifest->artifacts) {
      for (const auto &chunk : artifact.chunks) {
        const auto name = hash_to_hex(chunk.hash);
        const auto [iterator, inserted] =
            snapshot.live_chunks.emplace(name, chunk.size);
        if (!inserted && iterator->second != chunk.size) {
          return std::unexpected(logical_error(
              StableErrorCode::corrupted_snapshot,
              "manifests disagree about an immutable chunk size"));
        }
      }
    }
  }

  std::ranges::sort(snapshot.manifests, std::greater{},
                    &StoredManifest::generation);
  for (const auto &[name, expected_size] : snapshot.live_chunks) {
    auto prefix = open_directory(operations, chunks_fd, name.substr(0, 2),
                                 "open referenced chunk prefix");
    if (!prefix) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "manifest references a missing or invalid chunk prefix"));
    }
    auto status = regular_file_status(operations, prefix->get(), name,
                                      "inspect referenced chunk");
    if (!status || status->size != expected_size) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "manifest references a missing chunk or an unexpected chunk size"));
    }
  }
  return snapshot;
}

[[nodiscard]] CheckpointResult<void> clear_referenced_candidates(
    const std::shared_ptr<IFileOperations> &operations, int container_fd,
    const std::map<std::string, std::uint64_t> &live_chunks);

[[nodiscard]] CheckpointResult<RetentionReport>
apply_retention_locked(const std::shared_ptr<IFileOperations> &operations,
                       int container_fd, int manifests_fd, int chunks_fd,
                       const ContainerId &container_id, std::size_t keep_last) {
  if (keep_last < 1U || keep_last > 3U) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "retention keep_last must be between one and three"));
  }

  auto snapshot =
      scan_manifests_strict(operations, manifests_fd, chunks_fd, container_id);
  if (!snapshot) {
    return std::unexpected(snapshot.error());
  }
  if (auto cleared = clear_referenced_candidates(operations, container_fd,
                                                 snapshot->live_chunks);
      !cleared) {
    return std::unexpected(cleared.error());
  }

  RetentionReport report{
      .manifests_deleted = 0U,
      .manifests_deferred = 0U,
      .complete = true,
  };
  for (std::size_t index = snapshot->manifests.size(); index > keep_last;
       --index) {
    auto removed = durable_remove_file(
        operations, manifests_fd,
        manifest_name(snapshot->manifests[index - 1U].generation),
        "remove retained manifest", "sync manifest retention deletion");
    if (!removed) {
      return std::unexpected(removed.error());
    }
    if (*removed) {
      ++report.manifests_deleted;
    }
  }
  return report;
}

struct ChunkIdentity {
  std::uint64_t device;
  std::uint64_t inode;
  std::uint64_t size;
  FileTimestamp modification_time;
  FileTimestamp status_change_time;

  bool operator==(const ChunkIdentity &) const = default;
};

[[nodiscard]] ChunkIdentity chunk_identity(const FileStatus &status) noexcept {
  return ChunkIdentity{
      .device = status.device,
      .inode = status.inode,
      .size = status.size,
      .modification_time = status.modification_time,
      .status_change_time = status.status_change_time,
  };
}

struct GarbageCandidate {
  std::string chunk_name;
  std::int64_t first_observed_unreferenced;
  ChunkIdentity identity;
};

[[nodiscard]] std::vector<std::byte> bytes_from_text(std::string_view text);

[[nodiscard]] std::string text_from_bytes(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const auto byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

[[nodiscard]] std::vector<std::byte>
encode_candidate(const GarbageCandidate &candidate) {
  const auto text =
      "format_version=1\nchunk=" + candidate.chunk_name +
      "\nfirst_observed_unix_nanoseconds=" +
      std::to_string(candidate.first_observed_unreferenced) +
      "\ndevice=" + std::to_string(candidate.identity.device) +
      "\ninode=" + std::to_string(candidate.identity.inode) +
      "\nsize=" + std::to_string(candidate.identity.size) + "\nmtime_seconds=" +
      std::to_string(candidate.identity.modification_time.seconds) +
      "\nmtime_nanoseconds=" +
      std::to_string(candidate.identity.modification_time.nanoseconds) +
      "\nctime_seconds=" +
      std::to_string(candidate.identity.status_change_time.seconds) +
      "\nctime_nanoseconds=" +
      std::to_string(candidate.identity.status_change_time.nanoseconds) + "\n";
  return bytes_from_text(text);
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer &value) {
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] CheckpointResult<GarbageCandidate>
decode_candidate(std::span<const std::byte> encoded,
                 std::string_view expected_chunk_name) {
  const auto text = text_from_bytes(encoded);
  std::vector<std::string_view> lines;
  std::size_t begin = 0U;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    if (end == std::string::npos) {
      return std::unexpected(
          logical_error(StableErrorCode::corrupted_snapshot,
                        "garbage candidate record is not newline terminated"));
    }
    lines.emplace_back(text.data() + begin, end - begin);
    begin = end + 1U;
  }
  if (lines.size() != 10U || lines[0] != "format_version=1") {
    return std::unexpected(
        logical_error(StableErrorCode::corrupted_snapshot,
                      "garbage candidate record has an unsupported format"));
  }

  const auto field =
      [&](std::size_t index,
          std::string_view prefix) -> std::optional<std::string_view> {
    if (!lines[index].starts_with(prefix)) {
      return std::nullopt;
    }
    return lines[index].substr(prefix.size());
  };
  const auto chunk = field(1U, "chunk=");
  const auto first = field(2U, "first_observed_unix_nanoseconds=");
  const auto device = field(3U, "device=");
  const auto inode = field(4U, "inode=");
  const auto size = field(5U, "size=");
  const auto modification_seconds = field(6U, "mtime_seconds=");
  const auto modification_nanoseconds = field(7U, "mtime_nanoseconds=");
  const auto change_seconds = field(8U, "ctime_seconds=");
  const auto change_nanoseconds = field(9U, "ctime_nanoseconds=");
  GarbageCandidate candidate;
  if (!chunk || *chunk != expected_chunk_name || !first || !device || !inode ||
      !size || !modification_seconds || !modification_nanoseconds ||
      !change_seconds || !change_nanoseconds ||
      !parse_integer(*first, candidate.first_observed_unreferenced) ||
      !parse_integer(*device, candidate.identity.device) ||
      !parse_integer(*inode, candidate.identity.inode) ||
      !parse_integer(*size, candidate.identity.size) ||
      !parse_integer(*modification_seconds,
                     candidate.identity.modification_time.seconds) ||
      !parse_integer(*modification_nanoseconds,
                     candidate.identity.modification_time.nanoseconds) ||
      !parse_integer(*change_seconds,
                     candidate.identity.status_change_time.seconds) ||
      !parse_integer(*change_nanoseconds,
                     candidate.identity.status_change_time.nanoseconds) ||
      candidate.identity.modification_time.nanoseconds >= 1000000000U ||
      candidate.identity.status_change_time.nanoseconds >= 1000000000U) {
    return std::unexpected(
        logical_error(StableErrorCode::corrupted_snapshot,
                      "garbage candidate record is corrupt"));
  }
  candidate.chunk_name = std::string{*chunk};
  return candidate;
}

[[nodiscard]] CheckpointResult<UniqueFileDescriptor>
open_or_create_lock(const std::shared_ptr<StorageContext> &context,
                    std::string_view name) {
  while (true) {
    auto created = context->operations->create_exclusive_file_at(
        context->locks_fd.get(), name, kFileMode);
    if (created) {
      UniqueFileDescriptor lock{context->operations, *created};
      if (auto result = sync_file(context->operations, lock.get(),
                                  "sync new container lock file");
          !result) {
        return std::unexpected(result.error());
      }
      if (auto result =
              sync_directory(context->operations, context->locks_fd.get(),
                             "sync lock directory");
          !result) {
        return std::unexpected(result.error());
      }
      return lock;
    }
    if (created.error() == EINTR) {
      continue;
    }
    if (created.error() != EEXIST) {
      return std::unexpected(
          filesystem_error("create container lock file", created.error()));
    }
    break;
  }

  while (true) {
    auto opened = context->operations->open_read_write_file_at(
        context->locks_fd.get(), name);
    if (opened) {
      UniqueFileDescriptor lock{context->operations, *opened};
      auto status = retry_interrupted(
          [&] { return context->operations->status(lock.get()); });
      if (!status) {
        return std::unexpected(
            filesystem_error("inspect container lock file", status.error()));
      }
      if (status->type != FileType::regular_file) {
        return std::unexpected(
            logical_error(StableErrorCode::storage_error,
                          "container lock path is not a regular file"));
      }
      return lock;
    }
    if (opened.error() != EINTR) {
      return std::unexpected(
          filesystem_error("open container lock file", opened.error()));
    }
  }
}

[[nodiscard]] CheckpointResult<void>
try_acquire_lock(const std::shared_ptr<StorageContext> &context, int lock_fd) {
  while (true) {
    auto locked = context->operations->try_lock_exclusive(lock_fd);
    if (locked) {
      if (*locked) {
        return {};
      }
      auto error = logical_error(
          StableErrorCode::conflict,
          "another operation owns the exclusive container storage lock");
      error.retryable = true;
      return std::unexpected(std::move(error));
    }
    if (locked.error() != EINTR) {
      return std::unexpected(
          filesystem_error("acquire container storage lock", locked.error()));
    }
  }
}

[[nodiscard]] CheckpointResult<ContainerDirectories>
open_existing_container(const std::shared_ptr<StorageContext> &context,
                        const ContainerId &container_id) {
  auto key = identifier_key("container", container_id.value(), "container_id");
  if (!key) {
    return std::unexpected(key.error());
  }
  auto container =
      open_directory(context->operations, context->containers_fd.get(), *key,
                     "open container namespace");
  if (!container) {
    return std::unexpected(container.error());
  }
  if (auto result =
          sync_directory(context->operations, context->containers_fd.get(),
                         "confirm container namespace durability");
      !result) {
    return std::unexpected(result.error());
  }
  if (auto result = sync_directory(context->operations, container->get(),
                                   "confirm container layout durability");
      !result) {
    return std::unexpected(result.error());
  }
  auto identity =
      read_regular_file(context->operations, container->get(), "identity",
                        kMaximumIdentifierBytes, "read container identity");
  if (!identity) {
    return std::unexpected(
        required_namespace_error(identity.error(), "identity"));
  }
  const auto expected = std::as_bytes(
      std::span(container_id.value().data(), container_id.value().size()));
  if (!std::ranges::equal(*identity, expected)) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "container namespace identity does not match its derived key"));
  }

  auto manifests = open_directory(context->operations, container->get(),
                                  "manifests", "open manifest directory");
  if (!manifests) {
    return std::unexpected(
        required_namespace_error(manifests.error(), "manifests"));
  }
  auto chunks = open_directory(context->operations, container->get(), "chunks",
                               "open chunk directory");
  if (!chunks) {
    return std::unexpected(required_namespace_error(chunks.error(), "chunks"));
  }
  auto staging = open_directory(context->operations, container->get(),
                                "staging", "open staging directory");
  if (!staging) {
    return std::unexpected(
        required_namespace_error(staging.error(), "staging"));
  }
  return ContainerDirectories{
      .key = std::move(*key),
      .container = std::move(*container),
      .manifests = std::move(*manifests),
      .chunks = std::move(*chunks),
      .staging = std::move(*staging),
  };
}

[[nodiscard]] CheckpointResult<ContainerDirectories>
ensure_container(const std::shared_ptr<StorageContext> &context,
                 const ContainerId &container_id) {
  auto key = identifier_key("container", container_id.value(), "container_id");
  if (!key) {
    return std::unexpected(key.error());
  }
  auto container =
      ensure_directory(context->operations, context->containers_fd.get(), *key,
                       "create or open container namespace");
  if (!container) {
    return std::unexpected(container.error());
  }

  const auto identity_bytes = std::as_bytes(
      std::span(container_id.value().data(), container_id.value().size()));
  auto identity =
      read_regular_file(context->operations, container->get(), "identity",
                        kMaximumIdentifierBytes, "read container identity");
  if (identity) {
    if (auto result = sync_directory(context->operations, container->get(),
                                     "confirm container layout durability");
        !result) {
      return std::unexpected(result.error());
    }
    if (!std::ranges::equal(*identity, identity_bytes)) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "container namespace identity does not match its derived key"));
    }

    auto manifests = open_directory(context->operations, container->get(),
                                    "manifests", "open manifest directory");
    if (!manifests) {
      return std::unexpected(
          required_namespace_error(manifests.error(), "manifests"));
    }
    auto chunks = open_directory(context->operations, container->get(),
                                 "chunks", "open chunk directory");
    if (!chunks) {
      return std::unexpected(
          required_namespace_error(chunks.error(), "chunks"));
    }
    auto staging = open_directory(context->operations, container->get(),
                                  "staging", "open staging directory");
    if (!staging) {
      return std::unexpected(
          required_namespace_error(staging.error(), "staging"));
    }
    return ContainerDirectories{
        .key = std::move(*key),
        .container = std::move(*container),
        .manifests = std::move(*manifests),
        .chunks = std::move(*chunks),
        .staging = std::move(*staging),
    };
  }
  if (identity.error().code != StableErrorCode::not_found) {
    return std::unexpected(identity.error());
  }

  auto manifests = ensure_directory(context->operations, container->get(),
                                    "manifests", "create manifest directory");
  if (!manifests) {
    return std::unexpected(manifests.error());
  }
  auto chunks = ensure_directory(context->operations, container->get(),
                                 "chunks", "create chunk directory");
  if (!chunks) {
    return std::unexpected(chunks.error());
  }
  auto staging = ensure_directory(context->operations, container->get(),
                                  "staging", "create staging directory");
  if (!staging) {
    return std::unexpected(staging.error());
  }

  auto published = durable_publish(context->operations, container->get(),
                                   "identity", identity_bytes);
  if (!published && published.error().code != StableErrorCode::already_exists) {
    return std::unexpected(published.error());
  }
  identity =
      read_regular_file(context->operations, container->get(), "identity",
                        kMaximumIdentifierBytes, "read container identity");
  if (!identity) {
    return std::unexpected(identity.error());
  }
  if (auto result = sync_directory(context->operations, container->get(),
                                   "confirm completed container durability");
      !result) {
    return std::unexpected(result.error());
  }
  if (!std::ranges::equal(*identity, identity_bytes)) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "container namespace identity does not match its derived key"));
  }
  return ContainerDirectories{
      .key = std::move(*key),
      .container = std::move(*container),
      .manifests = std::move(*manifests),
      .chunks = std::move(*chunks),
      .staging = std::move(*staging),
  };
}

[[nodiscard]] std::string operation_kind_name(StorageOperationKind kind) {
  switch (kind) {
  case StorageOperationKind::create_checkpoint:
    return "create_checkpoint";
  case StorageOperationKind::restore:
    return "restore";
  case StorageOperationKind::retention:
    return "retention";
  case StorageOperationKind::garbage_collection:
    return "garbage_collection";
  }
  return "unknown";
}

[[nodiscard]] std::vector<std::byte> bytes_from_text(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  return {bytes.begin(), bytes.end()};
}

struct ChunkFile {
  std::string prefix;
  FileStatus status;
};

[[nodiscard]] CheckpointResult<std::map<std::string, ChunkFile>>
scan_canonical_chunks(const std::shared_ptr<IFileOperations> &operations,
                      int chunks_fd) {
  auto prefixes = list_directory(operations, chunks_fd,
                                 "list chunk root for garbage collection");
  if (!prefixes) {
    return std::unexpected(prefixes.error());
  }

  std::map<std::string, ChunkFile> chunks;
  for (const auto &prefix_name : *prefixes) {
    if (!is_chunk_prefix(prefix_name)) {
      continue;
    }
    auto opened_prefix = retry_interrupted(
        [&] { return operations->open_directory_at(chunks_fd, prefix_name); });
    if (!opened_prefix) {
      if (opened_prefix.error() == ENOENT || opened_prefix.error() == ENOTDIR ||
          opened_prefix.error() == ELOOP || opened_prefix.error() == EINVAL) {
        continue;
      }
      return std::unexpected(filesystem_error(
          "open chunk prefix for garbage collection", opened_prefix.error()));
    }
    UniqueFileDescriptor prefix{operations, *opened_prefix};
    auto entries = list_directory(operations, prefix.get(),
                                  "list chunk prefix for garbage collection");
    if (!entries) {
      return std::unexpected(entries.error());
    }
    for (const auto &entry : *entries) {
      if (!is_chunk_name(entry) || !entry.starts_with(prefix_name)) {
        continue;
      }
      auto opened = retry_interrupted([&] {
        return operations->open_regular_file_at(prefix.get(), entry);
      });
      if (!opened) {
        if (opened.error() == ENOENT || opened.error() == ELOOP ||
            opened.error() == EINVAL || opened.error() == ENOTDIR) {
          continue;
        }
        return std::unexpected(
            filesystem_error("open chunk during garbage scan", opened.error()));
      }
      UniqueFileDescriptor file{operations, *opened};
      auto status =
          retry_interrupted([&] { return operations->status(file.get()); });
      if (!status) {
        return std::unexpected(filesystem_error(
            "inspect chunk during garbage scan", status.error()));
      }
      if (status->type != FileType::regular_file) {
        continue;
      }
      chunks.emplace(entry,
                     ChunkFile{.prefix = prefix_name, .status = *status});
    }
  }
  return chunks;
}

[[nodiscard]] CheckpointResult<std::map<std::string, GarbageCandidate>>
scan_candidates(const std::shared_ptr<IFileOperations> &operations,
                int candidates_fd) {
  if (auto synced = sync_directory(
          operations, candidates_fd,
          "confirm garbage candidate records before maintenance");
      !synced) {
    return std::unexpected(synced.error());
  }
  auto entries = list_directory(operations, candidates_fd,
                                "list garbage candidate records");
  if (!entries) {
    return std::unexpected(entries.error());
  }

  std::map<std::string, GarbageCandidate> candidates;
  for (const auto &entry : *entries) {
    if (!is_chunk_name(entry)) {
      continue;
    }
    auto encoded = read_regular_file(operations, candidates_fd, entry,
                                     kCandidateRecordMaximumBytes,
                                     "read garbage candidate record");
    if (!encoded) {
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "garbage candidate record is unreadable or has an invalid type"));
    }
    auto candidate = decode_candidate(*encoded, entry);
    if (!candidate) {
      return std::unexpected(candidate.error());
    }
    candidates.emplace(entry, std::move(*candidate));
  }
  return candidates;
}

[[nodiscard]] CheckpointResult<void>
publish_candidate(const std::shared_ptr<IFileOperations> &operations,
                  int candidates_fd, std::string_view chunk_name,
                  const FileStatus &status, std::int64_t observed_at) {
  const auto encoded = encode_candidate(GarbageCandidate{
      .chunk_name = std::string{chunk_name},
      .first_observed_unreferenced = observed_at,
      .identity = chunk_identity(status),
  });
  return durable_publish(operations, candidates_fd, chunk_name, encoded);
}

[[nodiscard]] CheckpointResult<bool>
remove_candidate(const std::shared_ptr<IFileOperations> &operations,
                 int candidates_fd, std::string_view chunk_name) {
  return durable_remove_file(operations, candidates_fd, chunk_name,
                             "remove garbage candidate record",
                             "sync garbage candidate removal", true);
}

[[nodiscard]] CheckpointResult<void> clear_referenced_candidates(
    const std::shared_ptr<IFileOperations> &operations, int container_fd,
    const std::map<std::string, std::uint64_t> &live_chunks) {
  auto candidates_directory =
      ensure_directory(operations, container_fd, kCandidatesDirectory,
                       "create garbage candidate directory before retention");
  if (!candidates_directory) {
    return std::unexpected(candidates_directory.error());
  }
  auto candidates = scan_candidates(operations, candidates_directory->get());
  if (!candidates) {
    return std::unexpected(candidates.error());
  }
  for (const auto &[name, unused_candidate] : *candidates) {
    static_cast<void>(unused_candidate);
    if (!live_chunks.contains(name)) {
      continue;
    }
    auto removed =
        remove_candidate(operations, candidates_directory->get(), name);
    if (!removed) {
      return std::unexpected(removed.error());
    }
  }
  return {};
}

[[nodiscard]] CheckpointResult<std::optional<FileStatus>>
recheck_chunk(const std::shared_ptr<IFileOperations> &operations, int chunks_fd,
              std::string_view chunk_name) {
  auto prefix = retry_interrupted([&] {
    return operations->open_directory_at(chunks_fd, chunk_name.substr(0, 2));
  });
  if (!prefix) {
    if (prefix.error() == ENOENT || prefix.error() == ENOTDIR ||
        prefix.error() == ELOOP || prefix.error() == EINVAL) {
      return std::optional<FileStatus>{};
    }
    return std::unexpected(
        filesystem_error("reopen garbage chunk prefix", prefix.error()));
  }
  UniqueFileDescriptor prefix_fd{operations, *prefix};
  auto file = retry_interrupted([&] {
    return operations->open_regular_file_at(prefix_fd.get(), chunk_name);
  });
  if (!file) {
    if (file.error() == ENOENT || file.error() == ELOOP ||
        file.error() == EINVAL || file.error() == ENOTDIR) {
      return std::optional<FileStatus>{};
    }
    return std::unexpected(
        filesystem_error("reopen garbage chunk", file.error()));
  }
  UniqueFileDescriptor file_fd{operations, *file};
  auto status =
      retry_interrupted([&] { return operations->status(file_fd.get()); });
  if (!status) {
    return std::unexpected(
        filesystem_error("reinspect garbage chunk", status.error()));
  }
  if (status->type != FileType::regular_file) {
    return std::optional<FileStatus>{};
  }
  return std::optional<FileStatus>{*status};
}

[[nodiscard]] CheckpointResult<GarbageCollectionReport>
collect_garbage_locked(const std::shared_ptr<IFileOperations> &operations,
                       int container_fd, int manifests_fd, int chunks_fd,
                       const ContainerId &container_id,
                       const GarbageCollectionOptions &options) {
  if (options.grace_period.count() < 0) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "garbage collection grace period must not be negative"));
  }

  auto roots =
      scan_manifests_strict(operations, manifests_fd, chunks_fd, container_id);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  auto candidates_directory =
      ensure_directory(operations, container_fd, kCandidatesDirectory,
                       "create garbage candidate directory");
  if (!candidates_directory) {
    return std::unexpected(candidates_directory.error());
  }
  auto candidates = scan_candidates(operations, candidates_directory->get());
  if (!candidates) {
    return std::unexpected(candidates.error());
  }
  auto chunks = scan_canonical_chunks(operations, chunks_fd);
  if (!chunks) {
    return std::unexpected(chunks.error());
  }

  GarbageCollectionReport report{
      .chunks_marked = 0U,
      .chunks_deleted = 0U,
      .candidate_records_deleted = 0U,
      .more_work = false,
  };

  for (auto iterator = candidates->begin(); iterator != candidates->end();) {
    const bool referenced = roots->live_chunks.contains(iterator->first);
    const bool missing = !chunks->contains(iterator->first);
    if (!referenced && !missing) {
      ++iterator;
      continue;
    }
    auto removed = remove_candidate(operations, candidates_directory->get(),
                                    iterator->first);
    if (!removed) {
      return std::unexpected(removed.error());
    }
    if (*removed) {
      ++report.candidate_records_deleted;
    }
    iterator = candidates->erase(iterator);
  }

  const auto now = unix_nanoseconds(options.now);
  for (const auto &[name, chunk] : *chunks) {
    if (roots->live_chunks.contains(name)) {
      continue;
    }

    auto candidate = candidates->find(name);
    if (candidate == candidates->end()) {
      if (auto marked = publish_candidate(
              operations, candidates_directory->get(), name, chunk.status, now);
          !marked) {
        return std::unexpected(marked.error());
      }
      ++report.chunks_marked;
      continue;
    }

    if (candidate->second.identity != chunk_identity(chunk.status)) {
      auto removed =
          remove_candidate(operations, candidates_directory->get(), name);
      if (!removed) {
        return std::unexpected(removed.error());
      }
      if (*removed) {
        ++report.candidate_records_deleted;
      }
      if (auto marked = publish_candidate(
              operations, candidates_directory->get(), name, chunk.status, now);
          !marked) {
        return std::unexpected(marked.error());
      }
      ++report.chunks_marked;
      continue;
    }

    if (!grace_elapsed(candidate->second.first_observed_unreferenced, now,
                       options.grace_period)) {
      continue;
    }
    if (report.chunks_deleted >= options.maximum_deletions) {
      report.more_work = true;
      continue;
    }

    auto rechecked_roots = scan_manifests_strict(operations, manifests_fd,
                                                 chunks_fd, container_id);
    if (!rechecked_roots) {
      return std::unexpected(rechecked_roots.error());
    }
    if (rechecked_roots->live_chunks.contains(name)) {
      auto removed =
          remove_candidate(operations, candidates_directory->get(), name);
      if (!removed) {
        return std::unexpected(removed.error());
      }
      if (*removed) {
        ++report.candidate_records_deleted;
      }
      continue;
    }

    auto current = recheck_chunk(operations, chunks_fd, name);
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!current->has_value() ||
        chunk_identity(**current) != candidate->second.identity) {
      auto removed =
          remove_candidate(operations, candidates_directory->get(), name);
      if (!removed) {
        return std::unexpected(removed.error());
      }
      if (*removed) {
        ++report.candidate_records_deleted;
      }
      if (current->has_value()) {
        if (auto marked = publish_candidate(
                operations, candidates_directory->get(), name, **current, now);
            !marked) {
          return std::unexpected(marked.error());
        }
        ++report.chunks_marked;
      }
      continue;
    }

    auto prefix = open_directory(operations, chunks_fd, chunk.prefix,
                                 "open garbage chunk prefix for deletion");
    if (!prefix) {
      return std::unexpected(prefix.error());
    }
    auto removed_chunk = durable_remove_file(
        operations, prefix->get(), name, "remove unreferenced chunk",
        "sync unreferenced chunk deletion");
    if (!removed_chunk) {
      return std::unexpected(removed_chunk.error());
    }
    if (!*removed_chunk) {
      continue;
    }
    ++report.chunks_deleted;

    auto removed_record =
        remove_candidate(operations, candidates_directory->get(), name);
    if (!removed_record) {
      return std::unexpected(removed_record.error());
    }
    if (*removed_record) {
      ++report.candidate_records_deleted;
    }
  }
  return report;
}

[[nodiscard]] CheckpointResult<void>
remove_tree_contents(const std::shared_ptr<IFileOperations> &operations,
                     int directory_fd) {
  auto entries = list_directory(operations, directory_fd,
                                "list staging directory for cleanup");
  if (!entries) {
    return std::unexpected(entries.error());
  }
  for (const auto &entry : *entries) {
    auto child = operations->open_directory_at(directory_fd, entry);
    if (child) {
      UniqueFileDescriptor child_fd{operations, *child};
      if (auto result = remove_tree_contents(operations, child_fd.get());
          !result) {
        return result;
      }
      child_fd.reset();
      auto removed = retry_interrupted(
          [&] { return operations->remove_directory_at(directory_fd, entry); });
      if (!removed) {
        return std::unexpected(
            filesystem_error("remove staging subdirectory", removed.error()));
      }
      continue;
    }
    if (child.error() != ENOTDIR && child.error() != ELOOP) {
      return std::unexpected(
          filesystem_error("inspect staging entry", child.error()));
    }
    auto removed = retry_interrupted(
        [&] { return operations->remove_file_at(directory_fd, entry); });
    if (!removed) {
      return std::unexpected(
          filesystem_error("remove staging file", removed.error()));
    }
  }
  return sync_directory(operations, directory_fd, "sync staging cleanup");
}

void cleanup_failed_operation_initialization_best_effort(
    const std::shared_ptr<IFileOperations> &operations, int staging_parent_fd,
    std::string_view operation_key, UniqueFileDescriptor &operation_directory,
    UniqueFileDescriptor &runtime_artifact_directory) {
  runtime_artifact_directory.reset();
  if (operation_directory.valid()) {
    static_cast<void>(
        remove_tree_contents(operations, operation_directory.get()));
    operation_directory.reset();
  }

  auto removed = retry_interrupted([&] {
    return operations->remove_directory_at(staging_parent_fd, operation_key);
  });
  if (removed) {
    static_cast<void>(sync_directory(
        operations, staging_parent_fd,
        "sync staging parent after failed operation initialization"));
  }
}

struct StorageOperationData {
  StorageOperationData(std::shared_ptr<StorageContext> storage_context,
                       ContainerId logical_container_id,
                       StorageOperationKind operation_kind)
      : context(std::move(storage_context)),
        container_id(std::move(logical_container_id)), kind(operation_kind) {}

  std::shared_ptr<StorageContext> context;
  ContainerId container_id;
  StorageOperationKind kind;
  std::string operation_key;
  std::filesystem::path staging_path;
  StorageDirectoryIdentity staging_identity{};
  std::filesystem::path runtime_artifact_path;
  StorageDirectoryIdentity runtime_artifact_identity{};
  UniqueFileDescriptor lock_fd;
  UniqueFileDescriptor container_fd;
  UniqueFileDescriptor manifests_fd;
  UniqueFileDescriptor chunks_fd;
  UniqueFileDescriptor staging_parent_fd;
  UniqueFileDescriptor operation_fd;
  UniqueFileDescriptor runtime_artifact_fd;
  std::mutex mutex;
  std::size_t pins{0};
  bool manifest_finalized{false};
  bool manifest_confirmed{false};
  bool cleaned{false};
  bool owner_alive{true};
};

[[nodiscard]] CheckpointResult<void>
cleanup_operation_locked(StorageOperationData &state) {
  if (state.cleaned) {
    return {};
  }
  if (state.pins != 0U) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "cannot clean a storage operation while manifest pins are active"));
  }
  state.runtime_artifact_fd.reset();
  if (auto result = remove_tree_contents(state.context->operations,
                                         state.operation_fd.get());
      !result) {
    return result;
  }
  state.operation_fd.reset();
  auto removed = retry_interrupted([&] {
    return state.context->operations->remove_directory_at(
        state.staging_parent_fd.get(), state.operation_key);
  });
  if (!removed) {
    return std::unexpected(filesystem_error(
        "remove operation staging directory", removed.error()));
  }
  if (auto result = sync_directory(state.context->operations,
                                   state.staging_parent_fd.get(),
                                   "sync staging parent after cleanup");
      !result) {
    return result;
  }
  state.cleaned = true;
  return {};
}

class FilesystemStoragePin final : public IStoragePin {
public:
  FilesystemStoragePin(std::shared_ptr<StorageOperationData> state,
                       std::string name)
      : state_(std::move(state)), name_(std::move(name)) {}

  ~FilesystemStoragePin() override { static_cast<void>(release()); }

  [[nodiscard]] CheckpointResult<void> release() override {
    if (!state_) {
      return {};
    }
    auto state = std::exchange(state_, {});
    std::scoped_lock lock{state->mutex};
    auto removed = retry_interrupted([&] {
      return state->context->operations->remove_file_at(
          state->operation_fd.get(), name_);
    });
    CheckpointResult<void> result;
    if (!removed && removed.error() != ENOENT) {
      result = std::unexpected(
          filesystem_error("remove manifest pin metadata", removed.error()));
    } else {
      result =
          sync_directory(state->context->operations, state->operation_fd.get(),
                         "sync manifest pin release");
    }
    if (state->pins > 0U) {
      --state->pins;
    }
    if (!state->owner_alive && state->pins == 0U) {
      static_cast<void>(cleanup_operation_locked(*state));
    }
    return result;
  }

private:
  std::shared_ptr<StorageOperationData> state_;
  std::string name_;
};

class FilesystemStorageOperation final : public IStorageOperation {
public:
  explicit FilesystemStorageOperation(
      std::shared_ptr<StorageOperationData> state)
      : state_(std::move(state)) {}

  ~FilesystemStorageOperation() override {
    std::scoped_lock lock{state_->mutex};
    state_->owner_alive = false;
    if (state_->pins == 0U) {
      static_cast<void>(cleanup_operation_locked(*state_));
    }
  }

  [[nodiscard]] const ContainerId &container_id() const noexcept override {
    return state_->container_id;
  }

  [[nodiscard]] StorageOperationKind kind() const noexcept override {
    return state_->kind;
  }

  [[nodiscard]] const std::filesystem::path &
  staging_directory() const noexcept override {
    return state_->staging_path;
  }

  [[nodiscard]] StorageDirectoryIdentity
  staging_directory_identity() const noexcept override {
    return state_->staging_identity;
  }

  [[nodiscard]] const std::filesystem::path &
  runtime_artifact_directory() const noexcept override {
    return state_->runtime_artifact_path;
  }

  [[nodiscard]] StorageDirectoryIdentity
  runtime_artifact_directory_identity() const noexcept override {
    return state_->runtime_artifact_identity;
  }

  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ChunkHash &hash) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->cleaned) {
      return std::unexpected(
          logical_error(StableErrorCode::conflict,
                        "chunks cannot be read after operation cleanup"));
    }

    if (auto result =
            sync_directory(state_->context->operations, state_->chunks_fd.get(),
                           "confirm operation chunk prefix durability");
        !result) {
      return std::unexpected(result.error());
    }
    const auto encoded_hash = hash_to_hex(hash);
    auto prefix = open_directory(
        state_->context->operations, state_->chunks_fd.get(),
        encoded_hash.substr(0, 2), "open operation chunk prefix directory");
    if (!prefix) {
      return std::unexpected(prefix.error());
    }
    auto bytes = read_regular_file(state_->context->operations, prefix->get(),
                                   encoded_hash, ManifestLimits::chunk_bytes,
                                   "read operation chunk");
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    if (auto result = sync_directory(state_->context->operations, prefix->get(),
                                     "confirm operation chunk durability");
        !result) {
      return std::unexpected(result.error());
    }
    return bytes;
  }

  [[nodiscard]] CheckpointResult<void>
  publish_chunk(const ChunkHash &hash,
                std::span<const std::byte> bytes) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->kind != StorageOperationKind::create_checkpoint ||
        state_->cleaned || state_->manifest_finalized) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "chunks can be published only by an active create operation before "
          "manifest commit"));
    }
    if (bytes.empty() || bytes.size() > ManifestLimits::chunk_bytes) {
      return std::unexpected(
          logical_error(StableErrorCode::invalid_argument,
                        "chunk bytes must contain between 1 byte and 4 MiB"));
    }

    const auto encoded_hash = hash_to_hex(hash);
    const auto prefix = encoded_hash.substr(0, 2);
    auto directory =
        ensure_directory(state_->context->operations, state_->chunks_fd.get(),
                         prefix, "create chunk prefix directory");
    if (!directory) {
      return std::unexpected(directory.error());
    }

    auto existing = read_regular_file(
        state_->context->operations, directory->get(), encoded_hash,
        ManifestLimits::chunk_bytes, "read existing chunk");
    if (existing) {
      if (std::ranges::equal(*existing, bytes)) {
        return sync_directory(state_->context->operations, directory->get(),
                              "confirm existing chunk durability",
                              StorageSideEffect::publication_may_be_visible);
      }
      return std::unexpected(logical_error(
          StableErrorCode::corrupted_snapshot,
          "existing immutable chunk differs from publication bytes"));
    }
    if (existing.error().code != StableErrorCode::not_found) {
      return std::unexpected(existing.error());
    }

    auto published = durable_publish(state_->context->operations,
                                     directory->get(), encoded_hash, bytes);
    if (!published &&
        published.error().code == StableErrorCode::already_exists) {
      existing = read_regular_file(
          state_->context->operations, directory->get(), encoded_hash,
          ManifestLimits::chunk_bytes, "read concurrently published chunk");
      if (existing && std::ranges::equal(*existing, bytes)) {
        return sync_directory(state_->context->operations, directory->get(),
                              "confirm concurrently published chunk durability",
                              StorageSideEffect::publication_may_be_visible);
      }
    }
    return published;
  }

  [[nodiscard]] CheckpointResult<void>
  publish_manifest(const CheckpointId &checkpoint_id,
                   std::span<const std::byte> manifest_bytes) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->kind != StorageOperationKind::create_checkpoint ||
        state_->cleaned || state_->manifest_finalized) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "manifest publication is final and requires an active create "
          "operation"));
    }
    auto manifest = decode_manifest_v1(manifest_bytes);
    if (!manifest) {
      return std::unexpected(logical_error(
          StableErrorCode::invalid_argument,
          "manifest publication requires a valid canonical Manifest v1"));
    }
    if (manifest->container_id != state_->container_id ||
        manifest->checkpoint_id != checkpoint_id) {
      return std::unexpected(logical_error(
          StableErrorCode::invalid_argument,
          "manifest identity does not match the storage operation"));
    }

    auto listed = list_manifest_references(state_->context->operations,
                                           state_->manifests_fd.get());
    if (!listed) {
      return std::unexpected(listed.error());
    }
    const auto destination = manifest_name(manifest->generation);
    for (const auto &reference : *listed) {
      auto encoded = read_regular_file(
          state_->context->operations, state_->manifests_fd.get(),
          manifest_name(reference.generation),
          ManifestLimits::maximum_encoded_bytes, "read published manifest");
      if (!encoded) {
        return std::unexpected(encoded.error());
      }
      if (reference.generation == manifest->generation) {
        if (*encoded == std::vector<std::byte>{manifest_bytes.begin(),
                                               manifest_bytes.end()}) {
          auto confirmed = sync_directory(
              state_->context->operations, state_->manifests_fd.get(),
              "confirm existing manifest durability",
              StorageSideEffect::publication_may_be_visible);
          if (confirmed) {
            state_->manifest_finalized = true;
            state_->manifest_confirmed = true;
          }
          return confirmed;
        }
        return std::unexpected(logical_error(
            StableErrorCode::conflict,
            "a different immutable manifest already uses this generation"));
      }

      auto existing = decode_manifest_v1(*encoded);
      if (!existing) {
        continue;
      }
      if (existing->container_id != state_->container_id ||
          existing->generation != reference.generation) {
        return std::unexpected(logical_error(
            StableErrorCode::corrupted_snapshot,
            "published manifest does not match its storage namespace"));
      }
      if (existing->checkpoint_id == checkpoint_id) {
        return std::unexpected(logical_error(
            StableErrorCode::conflict,
            "checkpoint_id already belongs to another manifest generation"));
      }
    }
    if (!listed->empty() &&
        manifest->generation <= listed->front().generation) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict, "manifest generation must be greater than "
                                     "every committed generation"));
    }

    auto published =
        durable_publish(state_->context->operations, state_->manifests_fd.get(),
                        destination, manifest_bytes);
    if (published) {
      state_->manifest_finalized = true;
      state_->manifest_confirmed = true;
    } else if (published.error().storage_side_effect ==
               StorageSideEffect::publication_may_be_visible) {
      state_->manifest_finalized = true;
    }
    return published;
  }

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStoragePin>>
  pin_manifest(std::uint64_t generation) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->cleaned || generation == 0U) {
      return std::unexpected(logical_error(
          StableErrorCode::invalid_argument,
          "a positive committed generation is required for a manifest pin"));
    }
    if (auto result = sync_directory(state_->context->operations,
                                     state_->manifests_fd.get(),
                                     "confirm manifest durability before pin");
        !result) {
      return std::unexpected(result.error());
    }
    auto manifest = read_regular_file(
        state_->context->operations, state_->manifests_fd.get(),
        manifest_name(generation), ManifestLimits::maximum_encoded_bytes,
        "read manifest before pinning");
    if (!manifest) {
      return std::unexpected(manifest.error());
    }

    const auto pin_name =
        "pin-" + manifest_name(generation).substr(0, kManifestNameDigits);
    const auto metadata = bytes_from_text(
        "format_version=1\ngeneration=" + std::to_string(generation) + "\n");
    auto published =
        durable_publish(state_->context->operations, state_->operation_fd.get(),
                        pin_name, metadata);
    if (!published) {
      return std::unexpected(published.error());
    }
    ++state_->pins;
    return std::unique_ptr<IStoragePin>{
        std::make_unique<FilesystemStoragePin>(state_, pin_name)};
  }

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(std::size_t keep_last) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->kind != StorageOperationKind::create_checkpoint ||
        state_->cleaned || !state_->manifest_confirmed) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "immediate retention requires a confirmed active create manifest"));
    }
    return apply_retention_locked(
        state_->context->operations, state_->container_fd.get(),
        state_->manifests_fd.get(), state_->chunks_fd.get(),
        state_->container_id, keep_last);
  }

  [[nodiscard]] CheckpointResult<void> cleanup() override {
    std::scoped_lock lock{state_->mutex};
    return cleanup_operation_locked(*state_);
  }

private:
  std::shared_ptr<StorageOperationData> state_;
};

struct StorageMaintenanceData {
  std::shared_ptr<StorageContext> context;
  ContainerId container_id;
  StorageOperationKind kind;
  UniqueFileDescriptor lock_fd;
  UniqueFileDescriptor container_fd;
  UniqueFileDescriptor manifests_fd;
  UniqueFileDescriptor chunks_fd;
  UniqueFileDescriptor staging_fd;
};

class FilesystemStorageMaintenanceOperation final
    : public IStorageMaintenanceOperation {
public:
  explicit FilesystemStorageMaintenanceOperation(StorageMaintenanceData data)
      : data_(std::move(data)) {}

  [[nodiscard]] const ContainerId &container_id() const noexcept override {
    return data_.container_id;
  }

  [[nodiscard]] StorageOperationKind kind() const noexcept override {
    return data_.kind;
  }

  [[nodiscard]] CheckpointResult<RetentionReport>
  apply_retention(std::size_t keep_last) override {
    if (data_.kind != StorageOperationKind::retention) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "retention is allowed only by a retention maintenance operation"));
    }
    return apply_retention_locked(
        data_.context->operations, data_.container_fd.get(),
        data_.manifests_fd.get(), data_.chunks_fd.get(), data_.container_id,
        keep_last);
  }

  [[nodiscard]] CheckpointResult<GarbageCollectionReport>
  collect_garbage(const GarbageCollectionOptions &options) override {
    if (data_.kind != StorageOperationKind::garbage_collection) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "garbage collection is allowed only by a garbage collection "
          "maintenance operation"));
    }
    return collect_garbage_locked(
        data_.context->operations, data_.container_fd.get(),
        data_.manifests_fd.get(), data_.chunks_fd.get(), data_.container_id,
        options);
  }

private:
  StorageMaintenanceData data_;
};

struct RecoveryContainer {
  ContainerId container_id;
  ContainerDirectories directories;
};

[[nodiscard]] CheckpointResult<std::optional<RecoveryContainer>>
open_recovery_container(const std::shared_ptr<StorageContext> &context,
                        std::string_view key) {
  auto opened = retry_interrupted([&] {
    return context->operations->open_directory_at(context->containers_fd.get(),
                                                  key);
  });
  if (!opened) {
    if (opened.error() == ENOENT || opened.error() == ENOTDIR ||
        opened.error() == ELOOP || opened.error() == EINVAL) {
      return std::optional<RecoveryContainer>{};
    }
    return std::unexpected(
        filesystem_error("open recovery container namespace", opened.error()));
  }
  UniqueFileDescriptor container{context->operations, *opened};
  auto status = retry_interrupted(
      [&] { return context->operations->status(container.get()); });
  if (!status) {
    return std::unexpected(filesystem_error(
        "inspect recovery container namespace", status.error()));
  }
  if (status->type != FileType::directory) {
    return std::optional<RecoveryContainer>{};
  }
  if (auto synced = sync_directory(context->operations, container.get(),
                                   "confirm recovery container durability");
      !synced) {
    return std::unexpected(synced.error());
  }

  auto identity = read_regular_file(context->operations, container.get(),
                                    "identity", kMaximumIdentifierBytes,
                                    "read recovery container identity");
  if (!identity) {
    if (identity.error().code == StableErrorCode::not_found) {
      return std::optional<RecoveryContainer>{};
    }
    return std::unexpected(identity.error());
  }
  const auto identity_text = text_from_bytes(*identity);
  auto container_id = ContainerId::create(identity_text);
  if (!container_id || identity_text.size() > kMaximumIdentifierBytes) {
    return std::unexpected(
        logical_error(StableErrorCode::corrupted_snapshot,
                      "recovery container identity is invalid"));
  }
  auto expected_key =
      identifier_key("container", container_id->value(), "container_id");
  if (!expected_key) {
    return std::unexpected(expected_key.error());
  }
  if (*expected_key != key) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "recovery container identity does not match its namespace key"));
  }

  auto manifests = open_directory(context->operations, container.get(),
                                  "manifests", "open recovery manifests");
  if (!manifests) {
    return std::unexpected(
        required_namespace_error(manifests.error(), "manifests"));
  }
  auto chunks = open_directory(context->operations, container.get(), "chunks",
                               "open recovery chunks");
  if (!chunks) {
    return std::unexpected(required_namespace_error(chunks.error(), "chunks"));
  }
  auto staging = open_directory(context->operations, container.get(), "staging",
                                "open recovery staging");
  if (!staging) {
    return std::unexpected(
        required_namespace_error(staging.error(), "staging"));
  }

  return std::optional<RecoveryContainer>{RecoveryContainer{
      .container_id = std::move(*container_id),
      .directories =
          ContainerDirectories{
              .key = std::string{key},
              .container = std::move(container),
              .manifests = std::move(*manifests),
              .chunks = std::move(*chunks),
              .staging = std::move(*staging),
          },
  }};
}

[[nodiscard]] CheckpointResult<std::size_t>
remove_stale_temporaries(const std::shared_ptr<IFileOperations> &operations,
                         int directory_fd,
                         const StartupRecoveryOptions &options) {
  auto entries = list_directory(operations, directory_fd,
                                "list directory for stale publication files");
  if (!entries) {
    return std::unexpected(entries.error());
  }
  std::size_t removed_count = 0U;
  for (const auto &entry : *entries) {
    if (!is_publication_temporary(entry)) {
      continue;
    }
    auto opened = retry_interrupted(
        [&] { return operations->open_regular_file_at(directory_fd, entry); });
    if (!opened) {
      if (opened.error() == ENOENT || opened.error() == ELOOP ||
          opened.error() == EINVAL || opened.error() == ENOTDIR) {
        continue;
      }
      return std::unexpected(
          filesystem_error("open stale publication temporary", opened.error()));
    }
    UniqueFileDescriptor file{operations, *opened};
    auto status =
        retry_interrupted([&] { return operations->status(file.get()); });
    if (!status) {
      return std::unexpected(filesystem_error(
          "inspect stale publication temporary", status.error()));
    }
    if (status->type != FileType::regular_file || !is_stale(*status, options)) {
      continue;
    }
    file.reset();
    auto removed = durable_remove_file(
        operations, directory_fd, entry, "remove stale publication temporary",
        "sync stale publication temporary removal", true);
    if (!removed) {
      return std::unexpected(removed.error());
    }
    if (*removed) {
      ++removed_count;
    }
  }
  return removed_count;
}

[[nodiscard]] CheckpointResult<std::size_t>
recover_staging(const std::shared_ptr<IFileOperations> &operations,
                int staging_fd, const StartupRecoveryOptions &options,
                std::size_t &temporary_entries_deleted) {
  auto root_temporaries =
      remove_stale_temporaries(operations, staging_fd, options);
  if (!root_temporaries) {
    return std::unexpected(root_temporaries.error());
  }
  temporary_entries_deleted += *root_temporaries;

  auto entries = list_directory(operations, staging_fd,
                                "list staging root for startup recovery");
  if (!entries) {
    return std::unexpected(entries.error());
  }
  std::size_t deleted = 0U;
  for (const auto &entry : *entries) {
    if (!is_container_key(entry)) {
      continue;
    }
    auto opened = retry_interrupted(
        [&] { return operations->open_directory_at(staging_fd, entry); });
    if (!opened) {
      if (opened.error() == ENOENT || opened.error() == ENOTDIR ||
          opened.error() == ELOOP || opened.error() == EINVAL) {
        continue;
      }
      return std::unexpected(
          filesystem_error("open recovery staging operation", opened.error()));
    }
    UniqueFileDescriptor operation{operations, *opened};
    auto status =
        retry_interrupted([&] { return operations->status(operation.get()); });
    if (!status) {
      return std::unexpected(filesystem_error(
          "inspect recovery staging operation", status.error()));
    }
    if (!is_stale(*status, options)) {
      auto temporaries =
          remove_stale_temporaries(operations, operation.get(), options);
      if (!temporaries) {
        return std::unexpected(temporaries.error());
      }
      temporary_entries_deleted += *temporaries;
      continue;
    }

    if (auto cleaned = remove_tree_contents(operations, operation.get());
        !cleaned) {
      return std::unexpected(cleaned.error());
    }
    operation.reset();
    auto removed = retry_interrupted(
        [&] { return operations->remove_directory_at(staging_fd, entry); });
    if (!removed) {
      return std::unexpected(
          filesystem_error("remove stale staging operation", removed.error()));
    }
    if (auto synced = sync_directory(operations, staging_fd,
                                     "sync stale staging operation removal");
        !synced) {
      return std::unexpected(synced.error());
    }
    ++deleted;
  }
  return deleted;
}

[[nodiscard]] CheckpointResult<std::size_t>
recover_chunk_temporaries(const std::shared_ptr<IFileOperations> &operations,
                          int chunks_fd,
                          const StartupRecoveryOptions &options) {
  auto root_removed = remove_stale_temporaries(operations, chunks_fd, options);
  if (!root_removed) {
    return std::unexpected(root_removed.error());
  }
  std::size_t deleted = *root_removed;
  auto entries = list_directory(operations, chunks_fd,
                                "list chunk root for temporary recovery");
  if (!entries) {
    return std::unexpected(entries.error());
  }
  for (const auto &entry : *entries) {
    if (!is_chunk_prefix(entry)) {
      continue;
    }
    auto opened = retry_interrupted(
        [&] { return operations->open_directory_at(chunks_fd, entry); });
    if (!opened) {
      if (opened.error() == ENOENT || opened.error() == ENOTDIR ||
          opened.error() == ELOOP || opened.error() == EINVAL) {
        continue;
      }
      return std::unexpected(filesystem_error(
          "open chunk prefix for temporary recovery", opened.error()));
    }
    UniqueFileDescriptor prefix{operations, *opened};
    auto removed = remove_stale_temporaries(operations, prefix.get(), options);
    if (!removed) {
      return std::unexpected(removed.error());
    }
    deleted += *removed;
  }
  return deleted;
}

[[nodiscard]] CheckpointResult<void>
recover_container_files(const std::shared_ptr<IFileOperations> &operations,
                        ContainerDirectories &directories,
                        const StartupRecoveryOptions &options,
                        StartupRecoveryReport &report) {
  auto candidates = ensure_directory(
      operations, directories.container.get(), kCandidatesDirectory,
      "create recovery garbage candidate directory");
  if (!candidates) {
    return std::unexpected(candidates.error());
  }

  const std::array<int, 3> publication_roots{directories.container.get(),
                                             directories.manifests.get(),
                                             candidates->get()};
  for (const int directory_fd : publication_roots) {
    auto removed = remove_stale_temporaries(operations, directory_fd, options);
    if (!removed) {
      return std::unexpected(removed.error());
    }
    report.temporary_entries_deleted += *removed;
  }
  auto chunk_temporaries =
      recover_chunk_temporaries(operations, directories.chunks.get(), options);
  if (!chunk_temporaries) {
    return std::unexpected(chunk_temporaries.error());
  }
  report.temporary_entries_deleted += *chunk_temporaries;

  auto staging = recover_staging(operations, directories.staging.get(), options,
                                 report.temporary_entries_deleted);
  if (!staging) {
    return std::unexpected(staging.error());
  }
  report.staging_entries_deleted += *staging;
  return {};
}

} // namespace

class FilesystemStorage::Impl final {
public:
  explicit Impl(std::shared_ptr<StorageContext> context)
      : context_(std::move(context)) {}

  std::shared_ptr<StorageContext> context_;
};

FilesystemStorage::FilesystemStorage(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

FilesystemStorage::~FilesystemStorage() = default;

CheckpointResult<std::unique_ptr<FilesystemStorage>>
FilesystemStorage::open(std::filesystem::path root) {
  return open(std::move(root), make_posix_file_operations());
}

CheckpointResult<std::unique_ptr<FilesystemStorage>>
FilesystemStorage::open(std::filesystem::path root,
                        std::shared_ptr<IFileOperations> file_operations) {
  if (!file_operations || !root.is_absolute()) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "storage root must be an existing absolute directory"));
  }
  root = root.lexically_normal();

  FileOperationResult<int> opened = std::unexpected(EINTR);
  while (!opened && opened.error() == EINTR) {
    opened = file_operations->open_root_directory(root);
  }
  if (!opened) {
    return std::unexpected(
        filesystem_error("open trusted storage root", opened.error()));
  }
  auto context = std::make_shared<StorageContext>();
  context->root = std::move(root);
  context->operations = std::move(file_operations);
  context->root_fd = UniqueFileDescriptor{context->operations, *opened};

  auto root_status = retry_interrupted(
      [&] { return context->operations->status(context->root_fd.get()); });
  if (!root_status) {
    return std::unexpected(
        filesystem_error("inspect trusted storage root", root_status.error()));
  }
  if (root_status->type != FileType::directory) {
    return std::unexpected(logical_error(StableErrorCode::invalid_argument,
                                         "storage root must be a directory"));
  }

  auto containers =
      ensure_directory(context->operations, context->root_fd.get(),
                       "containers", "create containers directory");
  if (!containers) {
    return std::unexpected(containers.error());
  }
  context->containers_fd = std::move(*containers);
  auto locks = ensure_directory(context->operations, context->root_fd.get(),
                                "locks", "create locks directory");
  if (!locks) {
    return std::unexpected(locks.error());
  }
  context->locks_fd = std::move(*locks);
  auto temporary = ensure_directory(context->operations, context->root_fd.get(),
                                    "tmp", "create temporary directory");
  if (!temporary) {
    return std::unexpected(temporary.error());
  }
  context->temporary_fd = std::move(*temporary);

  return std::unique_ptr<FilesystemStorage>{
      new FilesystemStorage(std::make_unique<Impl>(std::move(context)))};
}

CheckpointResult<std::unique_ptr<IStorageOperation>>
FilesystemStorage::begin_operation(const ContainerId &container_id,
                                   const OperationId &operation_id,
                                   StorageOperationKind kind) {
  if (kind != StorageOperationKind::create_checkpoint &&
      kind != StorageOperationKind::restore) {
    return std::unexpected(logical_error(
        StableErrorCode::invalid_argument,
        "retention and garbage collection require begin_maintenance"));
  }
  auto container_key =
      identifier_key("container", container_id.value(), "container_id");
  if (!container_key) {
    return std::unexpected(container_key.error());
  }
  auto operation_key =
      identifier_key("operation", operation_id.value(), "operation_id");
  if (!operation_key) {
    return std::unexpected(operation_key.error());
  }
  const auto lock_name = *container_key + ".lock";
  auto lock_fd = open_or_create_lock(implementation_->context_, lock_name);
  if (!lock_fd) {
    return std::unexpected(lock_fd.error());
  }
  if (auto result = try_acquire_lock(implementation_->context_, lock_fd->get());
      !result) {
    return std::unexpected(result.error());
  }

  auto directories = ensure_container(implementation_->context_, container_id);
  if (!directories) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(directories.error());
  }
  FileOperationResult<bool> staging_created = std::unexpected(EINTR);
  while (!staging_created && staging_created.error() == EINTR) {
    staging_created =
        implementation_->context_->operations->create_directory_at(
            directories->staging.get(), *operation_key, kDirectoryMode);
  }
  if (!staging_created) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(filesystem_error(
        "create operation staging directory", staging_created.error()));
  }
  if (!*staging_created) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(logical_error(
        StableErrorCode::conflict, "operation_id refers to existing incomplete "
                                   "staging and cannot be resumed"));
  }

  UniqueFileDescriptor operation_directory;
  UniqueFileDescriptor runtime_artifact_directory;
  const auto fail_initialization = [&](CheckpointErrorDetail error)
      -> CheckpointResult<std::unique_ptr<IStorageOperation>> {
    cleanup_failed_operation_initialization_best_effort(
        implementation_->context_->operations, directories->staging.get(),
        *operation_key, operation_directory, runtime_artifact_directory);
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(std::move(error));
  };

  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->staging.get(),
                                   "sync new operation staging directory");
      !result) {
    return fail_initialization(result.error());
  }
  auto opened_operation = open_directory(
      implementation_->context_->operations, directories->staging.get(),
      *operation_key, "open new operation staging directory");
  if (!opened_operation) {
    return fail_initialization(opened_operation.error());
  }
  operation_directory = std::move(*opened_operation);

  FileOperationResult<bool> runtime_artifact_created = std::unexpected(EINTR);
  while (!runtime_artifact_created &&
         runtime_artifact_created.error() == EINTR) {
    runtime_artifact_created =
        implementation_->context_->operations->create_directory_at(
            operation_directory.get(), "runtime-artifact", kDirectoryMode);
  }
  if (!runtime_artifact_created) {
    return fail_initialization(filesystem_error(
        "create runtime artifact directory", runtime_artifact_created.error()));
  }
  if (!*runtime_artifact_created) {
    return fail_initialization(logical_error(
        StableErrorCode::conflict,
        "runtime artifact directory already exists in new operation staging"));
  }
  if (auto result = sync_directory(implementation_->context_->operations,
                                   operation_directory.get(),
                                   "sync runtime artifact directory creation");
      !result) {
    return fail_initialization(result.error());
  }

  auto opened_runtime_artifact = open_directory(
      implementation_->context_->operations, operation_directory.get(),
      "runtime-artifact", "open runtime artifact directory");
  if (!opened_runtime_artifact) {
    return fail_initialization(opened_runtime_artifact.error());
  }
  runtime_artifact_directory = std::move(*opened_runtime_artifact);

  auto staging_identity = directory_identity(
      implementation_->context_->operations, operation_directory.get(),
      "inspect operation staging directory");
  if (!staging_identity) {
    return fail_initialization(staging_identity.error());
  }
  auto runtime_artifact_identity = directory_identity(
      implementation_->context_->operations, runtime_artifact_directory.get(),
      "inspect runtime artifact directory");
  if (!runtime_artifact_identity) {
    return fail_initialization(runtime_artifact_identity.error());
  }

  const auto state_text = "format_version=1\noperation_key=" + *operation_key +
                          "\nkind=" + operation_kind_name(kind) +
                          "\nstate=active\n";
  const auto state_bytes = bytes_from_text(state_text);
  if (auto result =
          durable_publish(implementation_->context_->operations,
                          operation_directory.get(), "state", state_bytes);
      !result) {
    return fail_initialization(result.error());
  }

  auto state = std::make_shared<StorageOperationData>(implementation_->context_,
                                                      container_id, kind);
  state->operation_key = *operation_key;
  state->staging_identity = *staging_identity;
  state->staging_path = implementation_->context_->root / "containers" /
                        directories->key / "staging" / *operation_key;
  state->runtime_artifact_identity = *runtime_artifact_identity;
  state->runtime_artifact_path = state->staging_path / "runtime-artifact";
  state->lock_fd = std::move(*lock_fd);
  state->container_fd = std::move(directories->container);
  state->manifests_fd = std::move(directories->manifests);
  state->chunks_fd = std::move(directories->chunks);
  state->staging_parent_fd = std::move(directories->staging);
  state->operation_fd = std::move(operation_directory);
  state->runtime_artifact_fd = std::move(runtime_artifact_directory);

  return std::unique_ptr<IStorageOperation>{
      std::make_unique<FilesystemStorageOperation>(std::move(state))};
}

CheckpointResult<std::unique_ptr<IStorageMaintenanceOperation>>
FilesystemStorage::begin_maintenance(const ContainerId &container_id,
                                     StorageOperationKind kind) {
  if (kind != StorageOperationKind::retention &&
      kind != StorageOperationKind::garbage_collection) {
    return std::unexpected(logical_error(
        StableErrorCode::invalid_argument,
        "maintenance kind must be retention or garbage_collection"));
  }
  auto container_key =
      identifier_key("container", container_id.value(), "container_id");
  if (!container_key) {
    return std::unexpected(container_key.error());
  }
  auto lock_fd =
      open_or_create_lock(implementation_->context_, *container_key + ".lock");
  if (!lock_fd) {
    return std::unexpected(lock_fd.error());
  }
  if (auto acquired =
          try_acquire_lock(implementation_->context_, lock_fd->get());
      !acquired) {
    return std::unexpected(acquired.error());
  }
  auto directories =
      open_existing_container(implementation_->context_, container_id);
  if (!directories) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(directories.error());
  }

  StorageMaintenanceData data{
      .context = implementation_->context_,
      .container_id = container_id,
      .kind = kind,
      .lock_fd = std::move(*lock_fd),
      .container_fd = std::move(directories->container),
      .manifests_fd = std::move(directories->manifests),
      .chunks_fd = std::move(directories->chunks),
      .staging_fd = std::move(directories->staging),
  };
  return std::unique_ptr<IStorageMaintenanceOperation>{
      std::make_unique<FilesystemStorageMaintenanceOperation>(std::move(data))};
}

CheckpointResult<StartupRecoveryReport>
FilesystemStorage::recover(const StartupRecoveryOptions &options) {
  if (options.grace_period.count() < 0) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "startup recovery grace period must not be negative"));
  }
  auto entries =
      list_directory(implementation_->context_->operations,
                     implementation_->context_->containers_fd.get(),
                     "list container namespaces for startup recovery");
  if (!entries) {
    return std::unexpected(entries.error());
  }

  StartupRecoveryReport report{
      .containers_scanned = 0U,
      .containers_deferred = 0U,
      .staging_entries_deleted = 0U,
      .temporary_entries_deleted = 0U,
      .chunks_marked = 0U,
      .chunks_deleted = 0U,
      .more_work = false,
  };
  for (const auto &key : *entries) {
    if (!is_container_key(key)) {
      continue;
    }
    auto lock_fd =
        open_or_create_lock(implementation_->context_, key + ".lock");
    if (!lock_fd) {
      return std::unexpected(lock_fd.error());
    }
    auto acquired = try_acquire_lock(implementation_->context_, lock_fd->get());
    if (!acquired) {
      if (acquired.error().code == StableErrorCode::conflict) {
        ++report.containers_deferred;
        report.more_work = true;
        continue;
      }
      return std::unexpected(acquired.error());
    }

    auto recovered = open_recovery_container(implementation_->context_, key);
    if (!recovered) {
      return std::unexpected(recovered.error());
    }
    if (!recovered->has_value()) {
      continue;
    }
    auto container = std::move(**recovered);
    ++report.containers_scanned;

    if (auto cleaned =
            recover_container_files(implementation_->context_->operations,
                                    container.directories, options, report);
        !cleaned) {
      return std::unexpected(cleaned.error());
    }
    auto garbage = collect_garbage_locked(
        implementation_->context_->operations,
        container.directories.container.get(),
        container.directories.manifests.get(),
        container.directories.chunks.get(), container.container_id,
        GarbageCollectionOptions{
            .now = options.now,
            .grace_period = options.grace_period,
            .maximum_deletions = options.maximum_chunk_deletions_per_container,
        });
    if (!garbage) {
      return std::unexpected(garbage.error());
    }
    report.chunks_marked += garbage->chunks_marked;
    report.chunks_deleted += garbage->chunks_deleted;
    report.more_work = report.more_work || garbage->more_work;
  }
  return report;
}

CheckpointResult<bool>
FilesystemStorage::contains_chunk(const ContainerId &container_id,
                                  const ChunkHash &hash) const {
  auto directories =
      open_existing_container(implementation_->context_, container_id);
  if (!directories) {
    if (directories.error().code == StableErrorCode::not_found) {
      return false;
    }
    return std::unexpected(directories.error());
  }
  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->chunks.get(),
                                   "confirm chunk prefix durability");
      !result) {
    return std::unexpected(result.error());
  }
  const auto encoded_hash = hash_to_hex(hash);
  auto prefix = open_directory(
      implementation_->context_->operations, directories->chunks.get(),
      encoded_hash.substr(0, 2), "open chunk prefix directory");
  if (!prefix) {
    if (prefix.error().code == StableErrorCode::not_found) {
      return false;
    }
    return std::unexpected(prefix.error());
  }
  auto bytes = read_regular_file(implementation_->context_->operations,
                                 prefix->get(), encoded_hash,
                                 ManifestLimits::chunk_bytes, "inspect chunk");
  if (!bytes) {
    if (bytes.error().code == StableErrorCode::not_found) {
      return false;
    }
    return std::unexpected(bytes.error());
  }
  if (auto result =
          sync_directory(implementation_->context_->operations, prefix->get(),
                         "confirm existing chunk durability");
      !result) {
    return std::unexpected(result.error());
  }
  return true;
}

CheckpointResult<std::vector<std::byte>>
FilesystemStorage::read_chunk(const ContainerId &container_id,
                              const ChunkHash &hash) const {
  auto directories =
      open_existing_container(implementation_->context_, container_id);
  if (!directories) {
    return std::unexpected(directories.error());
  }
  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->chunks.get(),
                                   "confirm chunk prefix durability");
      !result) {
    return std::unexpected(result.error());
  }
  const auto encoded_hash = hash_to_hex(hash);
  auto prefix = open_directory(
      implementation_->context_->operations, directories->chunks.get(),
      encoded_hash.substr(0, 2), "open chunk prefix directory");
  if (!prefix) {
    return std::unexpected(prefix.error());
  }
  auto bytes = read_regular_file(implementation_->context_->operations,
                                 prefix->get(), encoded_hash,
                                 ManifestLimits::chunk_bytes, "read chunk");
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  if (auto result =
          sync_directory(implementation_->context_->operations, prefix->get(),
                         "confirm existing chunk durability");
      !result) {
    return std::unexpected(result.error());
  }
  return bytes;
}

CheckpointResult<std::vector<StoredManifest>>
FilesystemStorage::list_manifests(const ContainerId &container_id) const {
  auto directories =
      open_existing_container(implementation_->context_, container_id);
  if (!directories) {
    if (directories.error().code == StableErrorCode::not_found) {
      return std::vector<StoredManifest>{};
    }
    return std::unexpected(directories.error());
  }
  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->manifests.get(),
                                   "confirm manifest listing durability");
      !result) {
    return std::unexpected(result.error());
  }
  return list_manifest_references(implementation_->context_->operations,
                                  directories->manifests.get());
}

CheckpointResult<std::vector<std::byte>>
FilesystemStorage::read_manifest(const ContainerId &container_id,
                                 std::uint64_t generation) const {
  if (generation == 0U) {
    return std::unexpected(
        logical_error(StableErrorCode::invalid_argument,
                      "manifest generation must be greater than zero"));
  }
  auto directories =
      open_existing_container(implementation_->context_, container_id);
  if (!directories) {
    return std::unexpected(directories.error());
  }
  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->manifests.get(),
                                   "confirm manifest durability before read");
      !result) {
    return std::unexpected(result.error());
  }
  auto encoded =
      read_regular_file(implementation_->context_->operations,
                        directories->manifests.get(), manifest_name(generation),
                        ManifestLimits::maximum_encoded_bytes, "read manifest");
  if (!encoded) {
    return std::unexpected(encoded.error());
  }
  auto manifest = decode_manifest_v1(*encoded);
  if (!manifest || manifest->container_id != container_id ||
      manifest->generation != generation) {
    return std::unexpected(logical_error(
        StableErrorCode::corrupted_snapshot,
        "published manifest does not match its storage reference"));
  }
  return encoded;
}

} // namespace gravesentinel::ccm
