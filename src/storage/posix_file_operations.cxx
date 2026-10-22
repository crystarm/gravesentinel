#include "src/storage/file_operations.hxx"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] std::string owned_name(std::string_view name) {
  return std::string{name};
}

[[nodiscard]] FileOperationResult<int>
open_verified_regular_at(int parent_fd, std::string_view name,
                         int access_flags) {
  const auto path = owned_name(name);
  const int path_fd =
      ::openat(parent_fd, path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
  if (path_fd < 0) {
    return std::unexpected(errno);
  }

  struct stat expected_attributes{};
  if (::fstat(path_fd, &expected_attributes) != 0) {
    const int status_error = errno;
    static_cast<void>(::close(path_fd));
    return std::unexpected(status_error);
  }
  if (!S_ISREG(expected_attributes.st_mode)) {
    static_cast<void>(::close(path_fd));
    return std::unexpected(EINVAL);
  }

  const auto proc_path = "/proc/self/fd/" + std::to_string(path_fd);
  const int fd =
      ::open(proc_path.c_str(), access_flags | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    const int open_error = errno;
    static_cast<void>(::close(path_fd));
    return std::unexpected(open_error);
  }

  struct stat actual_attributes{};
  if (::fstat(fd, &actual_attributes) != 0) {
    const int status_error = errno;
    static_cast<void>(::close(fd));
    static_cast<void>(::close(path_fd));
    return std::unexpected(status_error);
  }
  if (!S_ISREG(actual_attributes.st_mode) ||
      actual_attributes.st_dev != expected_attributes.st_dev ||
      actual_attributes.st_ino != expected_attributes.st_ino) {
    static_cast<void>(::close(fd));
    static_cast<void>(::close(path_fd));
    return std::unexpected(EIO);
  }
  static_cast<void>(::close(path_fd));
  return fd;
}

class PosixFileOperations final : public IFileOperations {
public:
  [[nodiscard]] FileOperationResult<int>
  open_root_directory(const std::filesystem::path &path) override {
    const int fd =
        ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return std::unexpected(errno);
    }
    return fd;
  }

  [[nodiscard]] FileOperationResult<int>
  open_directory_at(int parent_fd, std::string_view name) override {
    const auto path = owned_name(name);
    const int fd = ::openat(parent_fd, path.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return std::unexpected(errno);
    }
    return fd;
  }

  [[nodiscard]] FileOperationResult<int>
  open_regular_file_at(int parent_fd, std::string_view name) override {
    return open_verified_regular_at(parent_fd, name, O_RDONLY);
  }

  [[nodiscard]] FileOperationResult<int>
  open_read_write_file_at(int parent_fd, std::string_view name) override {
    return open_verified_regular_at(parent_fd, name, O_RDWR);
  }

  [[nodiscard]] FileOperationResult<int>
  create_exclusive_file_at(int parent_fd, std::string_view name,
                           std::uint32_t mode) override {
    const auto path = owned_name(name);
    const int fd = ::openat(parent_fd, path.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW |
                                O_NONBLOCK,
                            static_cast<mode_t>(mode));
    if (fd < 0) {
      return std::unexpected(errno);
    }
    return fd;
  }

  [[nodiscard]] FileOperationResult<bool>
  create_directory_at(int parent_fd, std::string_view name,
                      std::uint32_t mode) override {
    const auto path = owned_name(name);
    if (::mkdirat(parent_fd, path.c_str(), static_cast<mode_t>(mode)) == 0) {
      return true;
    }
    if (errno == EEXIST) {
      return false;
    }
    return std::unexpected(errno);
  }

  [[nodiscard]] FileOperationResult<FileStatus> status(int fd) override {
    struct stat attributes{};
    if (::fstat(fd, &attributes) != 0) {
      return std::unexpected(errno);
    }
    if (attributes.st_size < 0) {
      return std::unexpected(EIO);
    }

    auto type = FileType::other;
    if (S_ISREG(attributes.st_mode)) {
      type = FileType::regular_file;
    } else if (S_ISDIR(attributes.st_mode)) {
      type = FileType::directory;
    }
    return FileStatus{
        .type = type,
        .size = static_cast<std::uint64_t>(attributes.st_size),
    };
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  read(int fd, std::span<std::byte> buffer) override {
    const auto bytes_read = ::read(fd, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      return std::unexpected(errno);
    }
    return static_cast<std::size_t>(bytes_read);
  }

  [[nodiscard]] FileOperationResult<std::size_t>
  write(int fd, std::span<const std::byte> buffer) override {
    const auto bytes_written = ::write(fd, buffer.data(), buffer.size());
    if (bytes_written < 0) {
      return std::unexpected(errno);
    }
    return static_cast<std::size_t>(bytes_written);
  }

  [[nodiscard]] FileOperationResult<void>
  random_bytes(std::span<std::byte> buffer) override {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
      const auto count =
          ::getrandom(buffer.data() + offset, buffer.size() - offset, 0);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(errno);
      }
      if (count == 0) {
        return std::unexpected(EIO);
      }
      offset += static_cast<std::size_t>(count);
    }
    return {};
  }

  [[nodiscard]] FileOperationResult<void> sync_file(int fd) override {
    if (::fsync(fd) != 0) {
      return std::unexpected(errno);
    }
    return {};
  }

  [[nodiscard]] FileOperationResult<void> sync_directory(int fd) override {
    if (::fsync(fd) != 0) {
      return std::unexpected(errno);
    }
    return {};
  }

  [[nodiscard]] FileOperationResult<void>
  rename_no_replace_at(int source_directory_fd, std::string_view source_name,
                       int destination_directory_fd,
                       std::string_view destination_name) override {
    const auto source = owned_name(source_name);
    const auto destination = owned_name(destination_name);
    if (::syscall(SYS_renameat2, source_directory_fd, source.c_str(),
                  destination_directory_fd, destination.c_str(),
                  RENAME_NOREPLACE) == 0) {
      return {};
    }

    const int rename_error = errno;
    if (rename_error != ENOSYS && rename_error != EINVAL) {
      return std::unexpected(rename_error);
    }

    if (::linkat(source_directory_fd, source.c_str(), destination_directory_fd,
                 destination.c_str(), 0) != 0) {
      return std::unexpected(errno);
    }
    static_cast<void>(::unlinkat(source_directory_fd, source.c_str(), 0));
    return {};
  }

  [[nodiscard]] FileOperationResult<void>
  remove_file_at(int parent_fd, std::string_view name) override {
    const auto path = owned_name(name);
    if (::unlinkat(parent_fd, path.c_str(), 0) != 0) {
      return std::unexpected(errno);
    }
    return {};
  }

  [[nodiscard]] FileOperationResult<void>
  remove_directory_at(int parent_fd, std::string_view name) override {
    const auto path = owned_name(name);
    if (::unlinkat(parent_fd, path.c_str(), AT_REMOVEDIR) != 0) {
      return std::unexpected(errno);
    }
    return {};
  }

  [[nodiscard]] FileOperationResult<std::vector<std::string>>
  list_directory(int directory_fd) override {
    const int listing_fd =
        ::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listing_fd < 0) {
      return std::unexpected(errno);
    }

    DIR *raw_directory = ::fdopendir(listing_fd);
    if (raw_directory == nullptr) {
      const int open_error = errno;
      static_cast<void>(::close(listing_fd));
      return std::unexpected(open_error);
    }

    std::vector<std::string> entries;
    errno = 0;
    while (const auto *entry = ::readdir(raw_directory)) {
      const std::string_view name{entry->d_name};
      if (name != "." && name != "..") {
        entries.emplace_back(name);
      }
      errno = 0;
    }
    const int listing_error = errno;
    static_cast<void>(::closedir(raw_directory));
    if (listing_error != 0) {
      return std::unexpected(listing_error);
    }
    return entries;
  }

  [[nodiscard]] FileOperationResult<bool> try_lock_exclusive(int fd) override {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      return true;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return false;
    }
    return std::unexpected(errno);
  }

  [[nodiscard]] FileOperationResult<void> unlock(int fd) override {
    if (::flock(fd, LOCK_UN) != 0) {
      return std::unexpected(errno);
    }
    return {};
  }

  void close(int fd) noexcept override {
    if (fd >= 0) {
      static_cast<void>(::close(fd));
    }
  }
};

} // namespace

std::shared_ptr<IFileOperations> make_posix_file_operations() {
  return std::make_shared<PosixFileOperations>();
}

} // namespace gravesentinel::ccm
