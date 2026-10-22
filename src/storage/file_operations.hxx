#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gravesentinel::ccm {

template <typename T> using FileOperationResult = std::expected<T, int>;

enum class FileType : std::uint8_t {
  regular_file,
  directory,
  other,
};

struct FileTimestamp {
  std::int64_t seconds;
  std::uint32_t nanoseconds;

  bool operator==(const FileTimestamp &) const = default;
};

struct FileStatus {
  FileType type;
  std::uint64_t size;
  std::uint32_t mode;
  std::uint64_t device;
  std::uint64_t inode;
  std::uint64_t link_count;
  FileTimestamp modification_time;
  FileTimestamp status_change_time;
  std::optional<std::uint64_t> mount_id;

  bool operator==(const FileStatus &) const = default;
};

class IFileOperations {
public:
  virtual ~IFileOperations() = default;

  [[nodiscard]] virtual FileOperationResult<int>
  open_root_directory(const std::filesystem::path &path) = 0;
  [[nodiscard]] virtual FileOperationResult<int>
  open_directory_at(int parent_fd, std::string_view name) = 0;
  [[nodiscard]] virtual FileOperationResult<int>
  open_regular_file_at(int parent_fd, std::string_view name) = 0;
  [[nodiscard]] virtual FileOperationResult<int>
  open_read_write_file_at(int parent_fd, std::string_view name) = 0;
  [[nodiscard]] virtual FileOperationResult<int>
  create_exclusive_file_at(int parent_fd, std::string_view name,
                           std::uint32_t mode) = 0;
  [[nodiscard]] virtual FileOperationResult<bool>
  create_directory_at(int parent_fd, std::string_view name,
                      std::uint32_t mode) = 0;
  [[nodiscard]] virtual FileOperationResult<FileStatus> status(int fd) = 0;
  [[nodiscard]] virtual FileOperationResult<std::size_t>
  read(int fd, std::span<std::byte> buffer) = 0;
  [[nodiscard]] virtual FileOperationResult<std::size_t>
  write(int fd, std::span<const std::byte> buffer) = 0;
  [[nodiscard]] virtual FileOperationResult<void>
  set_permissions(int fd, std::uint32_t permissions) = 0;
  [[nodiscard]] virtual FileOperationResult<void>
  random_bytes(std::span<std::byte> buffer) = 0;
  [[nodiscard]] virtual FileOperationResult<void> sync_file(int fd) = 0;
  [[nodiscard]] virtual FileOperationResult<void> sync_directory(int fd) = 0;
  [[nodiscard]] virtual FileOperationResult<void>
  rename_no_replace_at(int source_directory_fd, std::string_view source_name,
                       int destination_directory_fd,
                       std::string_view destination_name) = 0;
  [[nodiscard]] virtual FileOperationResult<void>
  remove_file_at(int parent_fd, std::string_view name) = 0;
  [[nodiscard]] virtual FileOperationResult<void>
  remove_directory_at(int parent_fd, std::string_view name) = 0;
  [[nodiscard]] virtual FileOperationResult<std::vector<std::string>>
  list_directory(int directory_fd, std::size_t maximum_entries) = 0;
  [[nodiscard]] virtual FileOperationResult<bool>
  try_lock_exclusive(int fd) = 0;
  [[nodiscard]] virtual FileOperationResult<void> unlock(int fd) = 0;
  virtual void close(int fd) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<IFileOperations> make_posix_file_operations();

} // namespace gravesentinel::ccm
