#pragma once

#include "src/protocol/types.hpp"

#include <cstddef>
#include <span>

namespace gravesentinel::ccm {

[[nodiscard]] ChunkHash blake3_hash(std::span<const std::byte> bytes) noexcept;

} // namespace gravesentinel::ccm
