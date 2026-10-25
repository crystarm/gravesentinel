#include "tests/integration/fault_injecting_file_operations.hxx"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gravesentinel::integration {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaximumModeBytes = 128U;
constexpr std::size_t kPartialWriteBytes = 4096U;
constexpr std::string_view kModeFile = "mode";
constexpr std::string_view kReachedFile = "reached";
constexpr std::string_view kReleaseFile = "release";
constexpr std::string_view kTemporaryPrefix = ".tmp-";
constexpr std::size_t kPublicationNonceHexBytes = 32U;
constexpr std::size_t kManifestDigits = 20U;
constexpr std::string_view kManifestSuffix = ".json";

class UniqueFileDescriptor final {
public:
  explicit UniqueFileDescriptor(int value = -1) noexcept : value_(value) {}

  ~UniqueFileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

  UniqueFileDescriptor(const UniqueFileDescriptor &) = delete;
  UniqueFileDescriptor &operator=(const UniqueFileDescriptor &) = delete;

  UniqueFileDescriptor(UniqueFileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}

  UniqueFileDescriptor &operator=(UniqueFileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }

  [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

private:
  int value_;
};

[[noreturn]] void throw_system_error(int error, std::string_view operation) {
  throw std::system_error(error, std::generic_category(),
                          std::string{operation});
}

[[nodiscard]] bool is_ascii_whitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\f' || character == '\v';
}

[[nodiscard]] std::string read_mode_file(int control_directory_fd) {
  int opened = -1;
  do {
    opened = ::openat(control_directory_fd, kModeFile.data(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (opened < 0 && errno == EINTR);
  if (opened < 0) {
    throw_system_error(errno, "open fault control mode");
  }
  UniqueFileDescriptor file{opened};

  struct stat attributes{};
  int status_result = -1;
  do {
    status_result = ::fstat(file.get(), &attributes);
  } while (status_result != 0 && errno == EINTR);
  if (status_result != 0) {
    throw_system_error(errno, "inspect fault control mode");
  }
  if (!S_ISREG(attributes.st_mode)) {
    throw std::invalid_argument("fault control mode is not a regular file");
  }

  std::string value;
  std::array<char, kMaximumModeBytes> buffer{};
  while (true) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_system_error(errno, "read fault control mode");
    }
    if (count == 0) {
      break;
    }
    const auto count_size = static_cast<std::size_t>(count);
    if (count_size > kMaximumModeBytes - value.size()) {
      throw std::invalid_argument("fault control mode is too large");
    }
    value.append(buffer.data(), count_size);
  }

  const auto first =
      std::find_if_not(value.begin(), value.end(), is_ascii_whitespace);
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), is_ascii_whitespace)
          .base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

[[nodiscard]] bool is_publication_temporary(std::string_view name) noexcept {
  return name.size() == kTemporaryPrefix.size() + kPublicationNonceHexBytes &&
         name.starts_with(kTemporaryPrefix) &&
         is_lower_hex(name.substr(kTemporaryPrefix.size()));
}

[[nodiscard]] bool is_manifest_name(std::string_view name) noexcept {
  if (name.size() != kManifestDigits + kManifestSuffix.size() ||
      !name.ends_with(kManifestSuffix)) {
    return false;
  }
  return std::all_of(
      name.begin(), name.begin() + kManifestDigits,
      [](char character) { return character >= '0' && character <= '9'; });
}

} // namespace

enum class FaultInjectingFileOperations::FaultMode : std::uint8_t {
  none,
  chunk_write_block,
  manifest_before_rename_block,
  manifest_after_rename_block,
  chunk_enospc,
  chunk_eacces,
  chunk_eintr_once,
};

FaultInjectingFileOperations::FaultInjectingFileOperations(
    const std::filesystem::path &control_directory)
    : delegate_(ccm::make_posix_file_operations()), control_directory_fd_(-1),
      mode_(load_mode(control_directory, control_directory_fd_)) {}

FaultInjectingFileOperations::~FaultInjectingFileOperations() {
  if (control_directory_fd_ >= 0) {
    static_cast<void>(::close(control_directory_fd_));
  }
}

FaultInjectingFileOperations::FaultMode FaultInjectingFileOperations::load_mode(
    const std::filesystem::path &control_directory, int &control_directory_fd) {
  int opened = -1;
  do {
    opened = ::open(control_directory.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  } while (opened < 0 && errno == EINTR);
  if (opened < 0) {
    throw_system_error(errno, "open fault control directory");
  }
  UniqueFileDescriptor directory{opened};
  const auto mode = read_mode_file(directory.get());

  FaultMode parsed;
  if (mode == "none") {
    parsed = FaultMode::none;
  } else if (mode == "chunk-write-block") {
    parsed = FaultMode::chunk_write_block;
  } else if (mode == "manifest-before-rename-block") {
    parsed = FaultMode::manifest_before_rename_block;
  } else if (mode == "manifest-after-rename-block") {
    parsed = FaultMode::manifest_after_rename_block;
  } else if (mode == "chunk-enospc") {
    parsed = FaultMode::chunk_enospc;
  } else if (mode == "chunk-eacces") {
    parsed = FaultMode::chunk_eacces;
  } else if (mode == "chunk-eintr-once") {
    parsed = FaultMode::chunk_eintr_once;
  } else {
    throw std::invalid_argument("unsupported fault control mode: " + mode);
  }

  control_directory_fd = directory.release();
  return parsed;
}

bool FaultInjectingFileOperations::is_chunk_directory(int fd) const {
  const std::lock_guard lock{descriptor_mutex_};
  return chunk_directories_.contains(fd);
}

bool FaultInjectingFileOperations::is_chunk_publication_file(int fd) const {
  const std::lock_guard lock{descriptor_mutex_};
  return chunk_publication_files_.contains(fd);
}

void FaultInjectingFileOperations::track_directory(int fd, bool under_chunks) {
  const std::lock_guard lock{descriptor_mutex_};
  chunk_publication_files_.erase(fd);
  if (under_chunks) {
    chunk_directories_.insert(fd);
  } else {
    chunk_directories_.erase(fd);
  }
}

void FaultInjectingFileOperations::track_file(
    int fd, bool chunk_publication_temporary) {
  const std::lock_guard lock{descriptor_mutex_};
  chunk_directories_.erase(fd);
  if (chunk_publication_temporary) {
    chunk_publication_files_.insert(fd);
  } else {
    chunk_publication_files_.erase(fd);
  }
}

bool FaultInjectingFileOperations::claim_fault() noexcept {
  bool expected = false;
  return fault_injected_.compare_exchange_strong(
      expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void FaultInjectingFileOperations::publish_reached_marker() const {
  int marker = -1;
  do {
    marker =
        ::openat(control_directory_fd_, kReachedFile.data(),
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  } while (marker < 0 && errno == EINTR);
  if (marker >= 0) {
    static_cast<void>(::close(marker));
    return;
  }
  if (errno != EEXIST) {
    throw_system_error(errno, "create fault reached marker");
  }

  struct stat attributes{};
  if (::fstatat(control_directory_fd_, kReachedFile.data(), &attributes,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(attributes.st_mode)) {
    throw std::runtime_error("fault reached marker already exists unsafely");
  }
}

void FaultInjectingFileOperations::block_until_release() const noexcept {
  while (true) {
    struct stat attributes{};
    if (::fstatat(control_directory_fd_, kReleaseFile.data(), &attributes,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
        !S_ISLNK(attributes.st_mode)) {
      return;
    }
    std::this_thread::sleep_for(10ms);
  }
}

ccm::FileOperationResult<int> FaultInjectingFileOperations::open_root_directory(
    const std::filesystem::path &path) {
  auto opened = delegate_->open_root_directory(path);
  if (opened) {
    track_directory(*opened, false);
  }
  return opened;
}

ccm::FileOperationResult<int>
FaultInjectingFileOperations::open_directory_at(int parent_fd,
                                                std::string_view name) {
  const bool under_chunks = name == "chunks" || is_chunk_directory(parent_fd);
  auto opened = delegate_->open_directory_at(parent_fd, name);
  if (opened) {
    track_directory(*opened, under_chunks);
  }
  return opened;
}

ccm::FileOperationResult<int>
FaultInjectingFileOperations::open_regular_file_at(int parent_fd,
                                                   std::string_view name) {
  auto opened = delegate_->open_regular_file_at(parent_fd, name);
  if (opened) {
    track_file(*opened, false);
  }
  return opened;
}

ccm::FileOperationResult<int>
FaultInjectingFileOperations::open_read_write_file_at(int parent_fd,
                                                      std::string_view name) {
  auto opened = delegate_->open_read_write_file_at(parent_fd, name);
  if (opened) {
    track_file(*opened, false);
  }
  return opened;
}

ccm::FileOperationResult<int>
FaultInjectingFileOperations::create_exclusive_file_at(int parent_fd,
                                                       std::string_view name,
                                                       std::uint32_t mode) {
  const bool chunk_publication_temporary =
      is_chunk_directory(parent_fd) && is_publication_temporary(name);
  auto created = delegate_->create_exclusive_file_at(parent_fd, name, mode);
  if (created) {
    track_file(*created, chunk_publication_temporary);
  }
  return created;
}

ccm::FileOperationResult<bool>
FaultInjectingFileOperations::create_directory_at(int parent_fd,
                                                  std::string_view name,
                                                  std::uint32_t mode) {
  return delegate_->create_directory_at(parent_fd, name, mode);
}

ccm::FileOperationResult<ccm::FileStatus>
FaultInjectingFileOperations::status(int fd) {
  return delegate_->status(fd);
}

ccm::FileOperationResult<std::size_t>
FaultInjectingFileOperations::read(int fd, std::span<std::byte> buffer) {
  return delegate_->read(fd, buffer);
}

ccm::FileOperationResult<std::size_t>
FaultInjectingFileOperations::write(int fd, std::span<const std::byte> buffer) {
  const bool chunk_mode = mode_ == FaultMode::chunk_write_block ||
                          mode_ == FaultMode::chunk_enospc ||
                          mode_ == FaultMode::chunk_eacces ||
                          mode_ == FaultMode::chunk_eintr_once;
  if (!chunk_mode || buffer.empty() || !is_chunk_publication_file(fd) ||
      !claim_fault()) {
    return delegate_->write(fd, buffer);
  }

  switch (mode_) {
  case FaultMode::chunk_write_block: {
    const auto partial_size =
        std::min(buffer.size() - static_cast<std::size_t>(buffer.size() > 1U),
                 kPartialWriteBytes);
    auto result = delegate_->write(fd, buffer.first(partial_size));
    publish_reached_marker();
    block_until_release();
    return result;
  }
  case FaultMode::chunk_enospc:
    publish_reached_marker();
    return std::unexpected(ENOSPC);
  case FaultMode::chunk_eacces:
    publish_reached_marker();
    return std::unexpected(EACCES);
  case FaultMode::chunk_eintr_once:
    publish_reached_marker();
    return std::unexpected(EINTR);
  case FaultMode::none:
  case FaultMode::manifest_before_rename_block:
  case FaultMode::manifest_after_rename_block:
    break;
  }
  std::abort();
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::set_permissions(int fd,
                                              std::uint32_t permissions) {
  return delegate_->set_permissions(fd, permissions);
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::random_bytes(std::span<std::byte> buffer) {
  return delegate_->random_bytes(buffer);
}

ccm::FileOperationResult<void> FaultInjectingFileOperations::sync_file(int fd) {
  return delegate_->sync_file(fd);
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::sync_directory(int fd) {
  return delegate_->sync_directory(fd);
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::rename_no_replace_at(
    int source_directory_fd, std::string_view source_name,
    int destination_directory_fd, std::string_view destination_name) {
  const bool before = mode_ == FaultMode::manifest_before_rename_block;
  const bool after = mode_ == FaultMode::manifest_after_rename_block;
  if ((!before && !after) || !is_manifest_name(destination_name) ||
      !claim_fault()) {
    return delegate_->rename_no_replace_at(source_directory_fd, source_name,
                                           destination_directory_fd,
                                           destination_name);
  }

  if (before) {
    publish_reached_marker();
    block_until_release();
  }
  auto result = delegate_->rename_no_replace_at(
      source_directory_fd, source_name, destination_directory_fd,
      destination_name);
  if (after && result) {
    publish_reached_marker();
    block_until_release();
  }
  return result;
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::remove_file_at(int parent_fd,
                                             std::string_view name) {
  return delegate_->remove_file_at(parent_fd, name);
}

ccm::FileOperationResult<void>
FaultInjectingFileOperations::remove_directory_at(int parent_fd,
                                                  std::string_view name) {
  return delegate_->remove_directory_at(parent_fd, name);
}

ccm::FileOperationResult<std::vector<std::string>>
FaultInjectingFileOperations::list_directory(int directory_fd,
                                             std::size_t maximum_entries) {
  return delegate_->list_directory(directory_fd, maximum_entries);
}

ccm::FileOperationResult<bool>
FaultInjectingFileOperations::try_lock_exclusive(int fd) {
  return delegate_->try_lock_exclusive(fd);
}

ccm::FileOperationResult<void> FaultInjectingFileOperations::unlock(int fd) {
  return delegate_->unlock(fd);
}

void FaultInjectingFileOperations::close(int fd) noexcept {
  const std::lock_guard lock{descriptor_mutex_};
  chunk_directories_.erase(fd);
  chunk_publication_files_.erase(fd);
  delegate_->close(fd);
}

} // namespace gravesentinel::integration
