#include "src/core/blake3_hasher.hxx"

#include "src/crypto/blake3.hxx"

namespace gravesentinel::ccm {

CheckpointResult<ChunkHash>
Blake3Hasher::hash(std::span<const std::byte> bytes) const noexcept {
  return blake3_hash(bytes);
}

} // namespace gravesentinel::ccm
