#pragma once

#include "src/core/contracts.hxx"
#include "src/manifest/types.hxx"
#include "src/storage/file_operations.hxx"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace gravesentinel::ccm {

class ISnapshotChunker {
public:
  virtual ~ISnapshotChunker() = default;

  [[nodiscard]] virtual CheckpointResult<std::vector<ManifestArtifactV1>>
  ingest(IStorageOperation &operation,
         const std::filesystem::path &snapshot_root) = 0;
};

class SnapshotChunker final : public ISnapshotChunker {
public:
  explicit SnapshotChunker(const IHasher &hasher);
  SnapshotChunker(const IHasher &hasher,
                  std::shared_ptr<IFileOperations> file_operations);

  [[nodiscard]] CheckpointResult<std::vector<ManifestArtifactV1>>
  ingest(IStorageOperation &operation,
         const std::filesystem::path &snapshot_root) override;

private:
  const IHasher &hasher_;
  std::shared_ptr<IFileOperations> file_operations_;
};

class SnapshotReassembler final {
public:
  SnapshotReassembler(IStorageOperation &operation, const IHasher &hasher);
  SnapshotReassembler(IStorageOperation &operation, const IHasher &hasher,
                      std::shared_ptr<IFileOperations> file_operations);

  [[nodiscard]] CheckpointResult<std::filesystem::path>
  reassemble(std::span<const ManifestArtifactV1> artifacts);

private:
  IStorageOperation &operation_;
  const IHasher &hasher_;
  std::shared_ptr<IFileOperations> file_operations_;
};

} // namespace gravesentinel::ccm
