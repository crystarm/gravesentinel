#include "src/core/blake3_hasher.hpp"

#include "src/crypto/blake3.hpp"

namespace gravesentinel::ccm {

ChunkHash Blake3Hasher::hash(std::span<const std::byte> bytes) const noexcept {
  return blake3_hash(bytes);
}

} // namespace gravesentinel::ccm
