#pragma once

#include <cstddef>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace gravesentinel::ipc {
inline constexpr unsigned protocol_version = 1;
inline constexpr std::size_t max_message_bytes = 64U * 1024U;

struct CodecError {
  std::string message;
};
[[nodiscard]] std::expected<nlohmann::json, CodecError>
decode(std::string_view bytes);
[[nodiscard]] std::expected<std::string, CodecError>
encode(const nlohmann::json &message);
[[nodiscard]] bool valid_identifier(std::string_view value) noexcept;
} // namespace gravesentinel::ipc
