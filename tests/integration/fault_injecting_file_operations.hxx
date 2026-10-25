#pragma once

#include "src/storage/file_operations.hxx"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gravesentinel::integration {

class FaultInjectingFileOperations final : public ccm::IFileOperations {
public:
  explicit FaultInjectingFileOperations(
      const std::filesystem::path &control_directory);
  ~FaultInjectingFileOperations() override;

  FaultInjectingFileOperations(const FaultInjectingFileOperations &) = delete;
  FaultInjectingFileOperations &
  operator=(const FaultInjectingFileOperations &) = delete;
  FaultInjectingFileOperations(FaultInjectingFileOperations &&) = delete;
  FaultInjectingFileOperations &
  operator=(FaultInjectingFileOperations &&) = delete;

  [[nodiscard]] ccm::FileOperationResult<int>
  open_root_directory(const std::filesystem::path &path) override;
  [[nodiscard]] ccm::FileOperationResult<int>
  open_directory_at(int parent_fd, std::string_view name) override;
  [[nodiscard]] ccm::FileOperationResult<int>
  open_regular_file_at(int parent_fd, std::string_view name) override;
  [[nodiscard]] ccm::FileOperationResult<int>
  open_read_write_file_at(int parent_fd, std::string_view name) override;
  [[nodiscard]] ccm::FileOperationResult<int>
  create_exclusive_file_at(int parent_fd, std::string_view name,
                           std::uint32_t mode) override;
  [[nodiscard]] ccm::FileOperationResult<bool>
  create_directory_at(int parent_fd, std::string_view name,
                      std::uint32_t mode) override;
  [[nodiscard]] ccm::FileOperationResult<ccm::FileStatus>
  status(int fd) override;
  [[nodiscard]] ccm::FileOperationResult<std::size_t>
  read(int fd, std::span<std::byte> buffer) override;
  [[nodiscard]] ccm::FileOperationResult<std::size_t>
  write(int fd, std::span<const std::byte> buffer) override;
  [[nodiscard]] ccm::FileOperationResult<void>
  set_permissions(int fd, std::uint32_t permissions) override;
  [[nodiscard]] ccm::FileOperationResult<void>
  random_bytes(std::span<std::byte> buffer) override;
  [[nodiscard]] ccm::FileOperationResult<void> sync_file(int fd) override;
  [[nodiscard]] ccm::FileOperationResult<void> sync_directory(int fd) override;
  [[nodiscard]] ccm::FileOperationResult<void>
  rename_no_replace_at(int source_directory_fd, std::string_view source_name,
                       int destination_directory_fd,
                       std::string_view destination_name) override;
  [[nodiscard]] ccm::FileOperationResult<void>
  remove_file_at(int parent_fd, std::string_view name) override;
  [[nodiscard]] ccm::FileOperationResult<void>
  remove_directory_at(int parent_fd, std::string_view name) override;
  [[nodiscard]] ccm::FileOperationResult<std::vector<std::string>>
  list_directory(int directory_fd, std::size_t maximum_entries) override;
  [[nodiscard]] ccm::FileOperationResult<bool>
  try_lock_exclusive(int fd) override;
  [[nodiscard]] ccm::FileOperationResult<void> unlock(int fd) override;
  void close(int fd) noexcept override;

private:
  enum class FaultMode : std::uint8_t;

  [[nodiscard]] static FaultMode
  load_mode(const std::filesystem::path &control_directory,
            int &control_directory_fd);
  [[nodiscard]] bool is_chunk_directory(int fd) const;
  [[nodiscard]] bool is_chunk_publication_file(int fd) const;
  void track_directory(int fd, bool under_chunks);
  void track_file(int fd, bool chunk_publication_temporary);
  [[nodiscard]] bool claim_fault() noexcept;
  void publish_reached_marker() const;
  void block_until_release() const noexcept;

  std::shared_ptr<ccm::IFileOperations> delegate_;
  int control_directory_fd_;
  FaultMode mode_;
  std::atomic<bool> fault_injected_{false};
  mutable std::mutex descriptor_mutex_;
  std::unordered_set<int> chunk_directories_;
  std::unordered_set<int> chunk_publication_files_;
};

} // namespace gravesentinel::integration
