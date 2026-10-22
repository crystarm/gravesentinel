#include "src/crypto/blake3.hpp"

#include <blake3.h>

#include <cstdint>

namespace gravesentinel::ccm {

static_assert(ChunkHash::digest_size == BLAKE3_OUT_LEN,
              "ChunkHash must hold exactly one BLAKE3 digest");

ChunkHash blake3_hash(std::span<const std::byte> bytes) noexcept {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());

  ChunkHash::Bytes digest{};
  blake3_hasher_finalize(
      &hasher, reinterpret_cast<std::uint8_t *>(digest.data()), digest.size());
  return ChunkHash{digest};
}

} // namespace gravesentinel::ccm
