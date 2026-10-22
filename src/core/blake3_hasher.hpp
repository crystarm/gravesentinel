#pragma once

#include "src/core/contracts.hpp"

#include <cstddef>
#include <span>

namespace gravesentinel::ccm {

class Blake3Hasher final : public IHasher {
public:
  [[nodiscard]] ChunkHash
  hash(std::span<const std::byte> bytes) const noexcept override;
};

} // namespace gravesentinel::ccm
