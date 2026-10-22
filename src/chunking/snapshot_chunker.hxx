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

class SnapshotChunker final {
public:
  SnapshotChunker(IStorageOperation &operation, const IHasher &hasher);
  SnapshotChunker(IStorageOperation &operation, const IHasher &hasher,
                  std::shared_ptr<IFileOperations> file_operations);

  [[nodiscard]] CheckpointResult<std::vector<ManifestArtifactV1>>
  ingest(const std::filesystem::path &snapshot_root);

private:
  IStorageOperation &operation_;
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
