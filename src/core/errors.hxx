#pragma once

#include "src/protocol/types.hxx"

#include <expected>
#include <optional>
#include <string>

namespace gravesentinel::ccm {

struct CheckpointErrorDetail {
  StableErrorCode code;
  std::string diagnostic;
  bool retryable{false};
  std::optional<RequestId> correlation_id;

  bool operator==(const CheckpointErrorDetail &) const = default;
};

template <typename T>
using CheckpointResult = std::expected<T, CheckpointErrorDetail>;

} // namespace gravesentinel::ccm
