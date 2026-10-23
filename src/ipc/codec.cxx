#include "src/ipc/codec.hxx"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace gravesentinel::ipc {
namespace {
bool has_exact_keys(const nlohmann::json &j,
                    std::initializer_list<std::string_view> keys) {
  if (!j.is_object() || j.size() != keys.size())
    return false;
  return std::all_of(keys.begin(), keys.end(), [&j](std::string_view key) {
    return j.contains(std::string(key));
  });
}
} // namespace

bool valid_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128)
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
  });
}

std::expected<nlohmann::json, CodecError> decode(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > max_message_bytes)
    return std::unexpected(
        CodecError{"message size is outside allowed bounds"});
  try {
    auto j = nlohmann::json::parse(bytes);
    if (!j.is_object() || !j.contains("version") ||
        !j["version"].is_number_unsigned())
      return std::unexpected(CodecError{"missing or invalid protocol version"});
    if (j["version"].get<unsigned>() != protocol_version)
      return std::unexpected(CodecError{"protocol version mismatch"});
    if (!j.contains("method") || !j["method"].is_string() ||
        !j.contains("request_id") || !j["request_id"].is_string() ||
        !j.contains("params") || !j["params"].is_object())
      return std::unexpected(CodecError{"invalid request envelope"});
    const auto method = j["method"].get<std::string>();
    const auto id = j["request_id"].get<std::string>();
    if (!valid_identifier(id))
      return std::unexpected(CodecError{"invalid request_id"});
    if (method != "Handshake" && method != "Register" &&
        method != "Unregister" && method != "SubmitCreateCheckpoint" &&
        method != "SubmitRestoreOrRestart" && method != "GetOperation" &&
        method != "GetHealth" && method != "Response")
      return std::unexpected(CodecError{"unknown method"});
    if (method == "Response") {
      if (!j["params"].empty())
        return std::unexpected(CodecError{"invalid response envelope"});
    } else if (method == "Handshake" || method == "GetHealth") {
      if (!j["params"].empty())
        return std::unexpected(CodecError{"method takes no parameters"});
    } else if (method == "Unregister" || method == "SubmitCreateCheckpoint" ||
               method == "SubmitRestoreOrRestart" || method == "GetOperation") {
      const std::string key = method == "GetOperation" ? "operation_id"
                              : method == "Unregister" ? "container_id"
                                                       : "container_id";
      if (!has_exact_keys(j["params"], {key}) ||
          !j["params"][key].is_string() ||
          !valid_identifier(j["params"][key].get<std::string>()))
        return std::unexpected(CodecError{"invalid method parameters"});
    } else if (method == "Register") {
      const auto &p = j["params"];
      if (!has_exact_keys(p, {"container_id", "runtime_container",
                              "interval_seconds", "keep_last", "container_cpu",
                              "checkpoint_cpu"}) ||
          !p["container_id"].is_string() ||
          !valid_identifier(p["container_id"].get<std::string>()) ||
          !p["runtime_container"].is_string() ||
          !valid_identifier(p["runtime_container"].get<std::string>()) ||
          !p["interval_seconds"].is_number_integer() ||
          p["interval_seconds"].get<std::int64_t>() <= 0 ||
          !p["keep_last"].is_number_integer() ||
          p["keep_last"].get<int>() < 1 || p["keep_last"].get<int>() > 3 ||
          !p["container_cpu"].is_number_unsigned() ||
          !p["checkpoint_cpu"].is_number_unsigned() ||
          p["container_cpu"].get<unsigned>() > 7 ||
          p["checkpoint_cpu"].get<unsigned>() > 7 ||
          p["container_cpu"].get<unsigned>() ==
              p["checkpoint_cpu"].get<unsigned>())
        return std::unexpected(CodecError{"invalid registration parameters"});
    }
    return j;
  } catch (const std::exception &) {
    return std::unexpected(CodecError{"malformed JSON"});
  }
}

std::expected<std::string, CodecError> encode(const nlohmann::json &message) {
  try {
    auto out = message.dump();
    if (out.empty() || out.size() > max_message_bytes)
      return std::unexpected(CodecError{"encoded message exceeds size bound"});
    return out;
  } catch (const std::exception &) {
    return std::unexpected(CodecError{"message cannot be encoded"});
  }
}
} // namespace gravesentinel::ipc
