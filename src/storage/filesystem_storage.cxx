#include "src/storage/filesystem_storage.hxx"

#include "src/crypto/blake3.hxx"
#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
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
constexpr std::string_view kManifestSuffix = ".json";

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
      auto status = operations->status(descriptor.get());
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

  auto status = operations->status(file.get());
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
  auto removed = operations->remove_file_at(directory_fd, name);
  if (!removed && removed.error() == EINTR) {
    removed = operations->remove_file_at(directory_fd, name);
  }
  if (removed) {
    static_cast<void>(operations->sync_directory(directory_fd));
  }
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
  auto status = operations->status(file.get());
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
    entries = operations->list_directory(manifests_fd);
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
    auto status = operations->status(file.get());
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
      auto status = context->operations->status(lock.get());
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
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "another operation owns the exclusive container storage lock"));
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
  if (!identity && identity.error().code == StableErrorCode::not_found) {
    auto published = durable_publish(context->operations, container->get(),
                                     "identity", identity_bytes);
    if (!published &&
        published.error().code != StableErrorCode::already_exists) {
      return std::unexpected(published.error());
    }
    identity =
        read_regular_file(context->operations, container->get(), "identity",
                          kMaximumIdentifierBytes, "read container identity");
  }
  if (!identity) {
    return std::unexpected(identity.error());
  }
  if (auto result = sync_directory(context->operations, container->get(),
                                   "confirm container identity durability");
      !result) {
    return std::unexpected(result.error());
  }
  if (!std::ranges::equal(*identity, identity_bytes)) {
    return std::unexpected(logical_error(
        StableErrorCode::conflict,
        "container namespace identity does not match its derived key"));
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

[[nodiscard]] CheckpointResult<void>
remove_tree_contents(const std::shared_ptr<IFileOperations> &operations,
                     int directory_fd) {
  auto entries = operations->list_directory(directory_fd);
  if (!entries) {
    return std::unexpected(filesystem_error(
        "list staging directory for cleanup", entries.error()));
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
      auto removed = operations->remove_directory_at(directory_fd, entry);
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
    auto removed = operations->remove_file_at(directory_fd, entry);
    if (!removed) {
      return std::unexpected(
          filesystem_error("remove staging file", removed.error()));
    }
  }
  return sync_directory(operations, directory_fd, "sync staging cleanup");
}

struct StorageOperationData {
  StorageOperationData(std::shared_ptr<StorageContext> storage_context,
                       ContainerId logical_container_id)
      : context(std::move(storage_context)),
        container_id(std::move(logical_container_id)) {}

  std::shared_ptr<StorageContext> context;
  ContainerId container_id;
  std::string operation_key;
  std::filesystem::path staging_path;
  UniqueFileDescriptor lock_fd;
  UniqueFileDescriptor container_fd;
  UniqueFileDescriptor manifests_fd;
  UniqueFileDescriptor chunks_fd;
  UniqueFileDescriptor staging_parent_fd;
  UniqueFileDescriptor operation_fd;
  std::mutex mutex;
  std::size_t pins{0};
  bool manifest_finalized{false};
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
  if (auto result = remove_tree_contents(state.context->operations,
                                         state.operation_fd.get());
      !result) {
    return result;
  }
  state.operation_fd.reset();
  auto removed = state.context->operations->remove_directory_at(
      state.staging_parent_fd.get(), state.operation_key);
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
    auto removed = state->context->operations->remove_file_at(
        state->operation_fd.get(), name_);
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

  [[nodiscard]] const std::filesystem::path &
  staging_directory() const noexcept override {
    return state_->staging_path;
  }

  [[nodiscard]] CheckpointResult<void>
  publish_chunk(const ChunkHash &hash,
                std::span<const std::byte> bytes) override {
    std::scoped_lock lock{state_->mutex};
    if (state_->cleaned || state_->manifest_finalized) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict, "chunks cannot be published after "
                                     "operation cleanup or manifest commit"));
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
    if (state_->cleaned || state_->manifest_finalized) {
      return std::unexpected(logical_error(
          StableErrorCode::conflict,
          "manifest publication is final for a storage operation"));
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
    if (published || published.error().storage_side_effect ==
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

  [[nodiscard]] CheckpointResult<void> cleanup() override {
    std::scoped_lock lock{state_->mutex};
    return cleanup_operation_locked(*state_);
  }

private:
  std::shared_ptr<StorageOperationData> state_;
};

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

  auto root_status = context->operations->status(context->root_fd.get());
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
  if (auto result = sync_directory(implementation_->context_->operations,
                                   directories->staging.get(),
                                   "sync new operation staging directory");
      !result) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(result.error());
  }
  auto operation_directory = open_directory(
      implementation_->context_->operations, directories->staging.get(),
      *operation_key, "open new operation staging directory");
  if (!operation_directory) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(operation_directory.error());
  }

  const auto state_text = "format_version=1\noperation_key=" + *operation_key +
                          "\nkind=" + operation_kind_name(kind) +
                          "\nstate=active\n";
  const auto state_bytes = bytes_from_text(state_text);
  if (auto result =
          durable_publish(implementation_->context_->operations,
                          operation_directory->get(), "state", state_bytes);
      !result) {
    static_cast<void>(
        implementation_->context_->operations->unlock(lock_fd->get()));
    return std::unexpected(result.error());
  }

  auto state = std::make_shared<StorageOperationData>(implementation_->context_,
                                                      container_id);
  state->operation_key = *operation_key;
  state->staging_path = implementation_->context_->root / "containers" /
                        directories->key / "staging" / *operation_key;
  state->lock_fd = std::move(*lock_fd);
  state->container_fd = std::move(directories->container);
  state->manifests_fd = std::move(directories->manifests);
  state->chunks_fd = std::move(directories->chunks);
  state->staging_parent_fd = std::move(directories->staging);
  state->operation_fd = std::move(*operation_directory);

  return std::unique_ptr<IStorageOperation>{
      std::make_unique<FilesystemStorageOperation>(std::move(state))};
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
