#pragma once

#include <cstddef>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace gravesentinel::ipc {
inline constexpr unsigned protocol_version = 4;
inline constexpr std::size_t max_message_bytes = 256U * 1024U;

struct CodecError {
  std::string message;
};

[[nodiscard]] std::expected<nlohmann::json, CodecError>
decode_request(std::string_view bytes);
[[nodiscard]] std::expected<nlohmann::json, CodecError>
decode_response(std::string_view bytes);
[[nodiscard]] std::expected<std::string, CodecError>
encode_request(const nlohmann::json &message);
[[nodiscard]] std::expected<std::string, CodecError>
encode_response(const nlohmann::json &message);
[[nodiscard]] bool valid_identifier(std::string_view value) noexcept;
} // namespace gravesentinel::ipc
