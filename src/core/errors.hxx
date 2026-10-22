#pragma once

#include "src/protocol/types.hxx"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace gravesentinel::ccm {

enum class StorageSideEffect : std::uint8_t {
  none,
  publication_may_be_visible,
};

struct CheckpointErrorDetail {
  StableErrorCode code;
  std::string diagnostic;
  bool retryable{false};
  std::optional<RequestId> correlation_id;
  StorageSideEffect storage_side_effect{StorageSideEffect::none};

  bool operator==(const CheckpointErrorDetail &) const = default;
};

template <typename T>
using CheckpointResult = std::expected<T, CheckpointErrorDetail>;

} // namespace gravesentinel::ccm
