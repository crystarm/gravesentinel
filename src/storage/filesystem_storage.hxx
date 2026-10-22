#pragma once

#include "src/core/contracts.hxx"
#include "src/storage/file_operations.hxx"

#include <filesystem>
#include <memory>

namespace gravesentinel::ccm {

class FilesystemStorage final : public IStorage {
public:
  [[nodiscard]] static CheckpointResult<std::unique_ptr<FilesystemStorage>>
  open(std::filesystem::path root);
  [[nodiscard]] static CheckpointResult<std::unique_ptr<FilesystemStorage>>
  open(std::filesystem::path root,
       std::shared_ptr<IFileOperations> file_operations);

  ~FilesystemStorage() override;

  FilesystemStorage(const FilesystemStorage &) = delete;
  FilesystemStorage &operator=(const FilesystemStorage &) = delete;
  FilesystemStorage(FilesystemStorage &&) = delete;
  FilesystemStorage &operator=(FilesystemStorage &&) = delete;

  [[nodiscard]] CheckpointResult<std::unique_ptr<IStorageOperation>>
  begin_operation(const ContainerId &container_id,
                  const OperationId &operation_id,
                  StorageOperationKind kind) override;
  [[nodiscard]] CheckpointResult<bool>
  contains_chunk(const ContainerId &container_id,
                 const ChunkHash &hash) const override;
  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_chunk(const ContainerId &container_id,
             const ChunkHash &hash) const override;
  [[nodiscard]] CheckpointResult<std::vector<StoredManifest>>
  list_manifests(const ContainerId &container_id) const override;
  [[nodiscard]] CheckpointResult<std::vector<std::byte>>
  read_manifest(const ContainerId &container_id,
                std::uint64_t generation) const override;

private:
  class Impl;

  explicit FilesystemStorage(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;
};

} // namespace gravesentinel::ccm
