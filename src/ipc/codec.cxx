#include "src/ipc/codec.hxx"

#include <algorithm>
#include <array>

#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace gravesentinel::ipc {
namespace {
using ValidationResult = std::expected<void, CodecError>;

constexpr std::size_t kMaxImageBytes = 2048;
constexpr std::size_t kMaxArrayEntries = 256;
constexpr std::size_t kMaxStringBytes = 4096;
constexpr std::size_t kMaxEnvironmentNameBytes = 256;
constexpr std::size_t kMaxMounts = 128;
constexpr std::size_t kMaxAggregateBytes = 64U * 1024U;

[[nodiscard]] CodecError make_error(std::string message) {
  return CodecError{std::move(message)};
}

[[nodiscard]] bool
has_exact_keys(const nlohmann::json &value,
               std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.size() != keys.size())
    return false;
  return std::all_of(keys.begin(), keys.end(),
                     [&value](const std::string_view key) {
                       return value.contains(std::string(key));
                     });
}

[[nodiscard]] bool bounded_string(const nlohmann::json &value,
                                  std::size_t minimum, std::size_t maximum) {
  if (!value.is_string())
    return false;
  const auto &text = value.get_ref<const std::string &>();
  return text.size() >= minimum && text.size() <= maximum &&
         text.find('\0') == std::string::npos;
}

[[nodiscard]] bool integer_in_range(const nlohmann::json &value,
                                    std::uint64_t minimum,
                                    std::uint64_t maximum) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    return number >= minimum && number <= maximum;
  }
  if (!value.is_number_integer())
    return false;
  const auto number = value.get<std::int64_t>();
  return number >= 0 && static_cast<std::uint64_t>(number) >= minimum &&
         static_cast<std::uint64_t>(number) <= maximum;
}

[[nodiscard]] bool consume(std::size_t &total, std::size_t bytes) {
  if (total > kMaxAggregateBytes || bytes > kMaxAggregateBytes - total)
    return false;
  total += bytes;
  return true;
}

[[nodiscard]] bool valid_environment_name(std::string_view name) {
  if (name.empty())
    return false;
  const auto letter = [](const char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
  };
  const auto digit = [](const char value) {
    return value >= '0' && value <= '9';
  };
  if (!letter(name.front()) && name.front() != '_')
    return false;
  return std::all_of(name.begin() + 1, name.end(), [&](const char value) {
    return letter(value) || digit(value) || value == '_';
  });
}

[[nodiscard]] ValidationResult
validate_cold_start(const nlohmann::json &cold_start) {
  if (!has_exact_keys(cold_start, {"image_reference", "argv", "environment",
                                   "mounts", "runtime_options"}) ||
      !bounded_string(cold_start.at("image_reference"), 1, kMaxImageBytes) ||
      !cold_start.at("argv").is_array() ||
      cold_start.at("argv").size() > kMaxArrayEntries ||
      !cold_start.at("environment").is_array() ||
      cold_start.at("environment").size() > kMaxArrayEntries ||
      !cold_start.at("mounts").is_array() ||
      cold_start.at("mounts").size() > kMaxMounts ||
      !cold_start.at("runtime_options").is_array() ||
      cold_start.at("runtime_options").size() > kMaxArrayEntries) {
    return std::unexpected(make_error("invalid cold_start shape"));
  }

  std::size_t total = 0;
  if (!consume(total, cold_start.at("image_reference")
                          .get_ref<const std::string &>()
                          .size())) {
    return std::unexpected(make_error("cold_start exceeds size bound"));
  }
  for (const auto &argument : cold_start.at("argv")) {
    if (!bounded_string(argument, 0, kMaxStringBytes) ||
        !consume(total, argument.get_ref<const std::string &>().size())) {
      return std::unexpected(make_error("invalid cold_start argv"));
    }
  }

  std::unordered_set<std::string> names;
  names.reserve(cold_start.at("environment").size());
  for (const auto &variable : cold_start.at("environment")) {
    if (!has_exact_keys(variable, {"name", "value"}) ||
        !bounded_string(variable.at("name"), 1, kMaxEnvironmentNameBytes) ||
        !bounded_string(variable.at("value"), 0, kMaxStringBytes)) {
      return std::unexpected(make_error("invalid cold_start environment"));
    }
    const auto &name = variable.at("name").get_ref<const std::string &>();
    const auto &value = variable.at("value").get_ref<const std::string &>();
    if (!valid_environment_name(name) || !names.insert(name).second ||
        !consume(total, name.size()) || !consume(total, value.size())) {
      return std::unexpected(make_error("invalid cold_start environment"));
    }
  }

  for (const auto &mount : cold_start.at("mounts")) {
    if (!has_exact_keys(mount, {"source", "target", "read_only"}) ||
        !bounded_string(mount.at("source"), 1, kMaxStringBytes) ||
        !bounded_string(mount.at("target"), 1, kMaxStringBytes) ||
        !mount.at("read_only").is_boolean()) {
      return std::unexpected(make_error("invalid cold_start mount"));
    }
    if (!consume(total,
                 mount.at("source").get_ref<const std::string &>().size()) ||
        !consume(total,
                 mount.at("target").get_ref<const std::string &>().size())) {
      return std::unexpected(make_error("cold_start exceeds size bound"));
    }
  }
  for (const auto &option : cold_start.at("runtime_options")) {
    if (!bounded_string(option, 0, kMaxStringBytes) ||
        !consume(total, option.get_ref<const std::string &>().size())) {
      return std::unexpected(make_error("invalid cold_start runtime option"));
    }
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_runtime_fingerprint(const nlohmann::json &fingerprint) {
  if (!has_exact_keys(fingerprint,
                      {"runtime_name",         "runtime_version",
                       "runtime_build_origin", "architecture",
                       "kernel_release",       "criu_version",
                       "oci_runtime_name",     "oci_runtime_version",
                       "oci_runtime_commit",   "oci_runtime_features",
                       "cgroup_version",       "cgroup_manager",
                       "cgroup_controllers",   "privilege_mode",
                       "user_namespace_mode",  "capability_profile",
                       "selinux_mode",         "apparmor_profile",
                       "seccomp_profile",      "command_profile"})) {
    return std::unexpected(make_error("invalid runtime_fingerprint shape"));
  }
  constexpr std::array<std::string_view, 20> keys{
      "runtime_name",         "runtime_version",     "runtime_build_origin",
      "architecture",         "kernel_release",      "criu_version",
      "oci_runtime_name",     "oci_runtime_version", "oci_runtime_commit",
      "oci_runtime_features", "cgroup_version",      "cgroup_manager",
      "cgroup_controllers",   "privilege_mode",      "user_namespace_mode",
      "capability_profile",   "selinux_mode",        "apparmor_profile",
      "seccomp_profile",      "command_profile",
  };
  std::size_t total = 0;
  for (const auto key : keys) {
    const auto &field = fingerprint.at(std::string(key));
    if (!bounded_string(field, 1, kMaxStringBytes) ||
        !consume(total, field.get_ref<const std::string &>().size())) {
      return std::unexpected(make_error("invalid runtime_fingerprint field"));
    }
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_registration(const nlohmann::json &parameters) {
  if (!has_exact_keys(parameters,
                      {"container_id", "runtime_container", "checkpoint",
                       "cold_start", "runtime_fingerprint"}) ||
      !parameters.at("container_id").is_string() ||
      !valid_identifier(
          parameters.at("container_id").get_ref<const std::string &>()) ||
      !parameters.at("runtime_container").is_string() ||
      !valid_identifier(
          parameters.at("runtime_container").get_ref<const std::string &>())) {
    return std::unexpected(make_error("invalid registration identifiers"));
  }

  const auto &checkpoint = parameters.at("checkpoint");
  if (!has_exact_keys(checkpoint, {"interval_seconds", "keep_last",
                                   "container_cpu", "checkpoint_cpu"}) ||
      !integer_in_range(checkpoint.at("interval_seconds"), 1,
                        365U * 24U * 60U * 60U) ||
      !integer_in_range(checkpoint.at("keep_last"), 1, 3) ||
      !integer_in_range(checkpoint.at("container_cpu"), 0, 7) ||
      !integer_in_range(checkpoint.at("checkpoint_cpu"), 0, 7)) {
    return std::unexpected(make_error("invalid checkpoint configuration"));
  }
  const auto container_cpu =
      checkpoint.at("container_cpu").get<std::uint64_t>();
  const auto checkpoint_cpu =
      checkpoint.at("checkpoint_cpu").get<std::uint64_t>();
  const bool standard_pair = (container_cpu == 0 && checkpoint_cpu == 1) ||
                             (container_cpu == 2 && checkpoint_cpu == 3) ||
                             (container_cpu == 4 && checkpoint_cpu == 5) ||
                             (container_cpu == 6 && checkpoint_cpu == 7);
  if (!standard_pair)
    return std::unexpected(make_error("invalid checkpoint CPU pair"));

  const auto &cold_start = parameters.at("cold_start");
  if (!cold_start.is_null()) {
    if (auto validated = validate_cold_start(cold_start); !validated)
      return validated;
  }
  const auto &fingerprint = parameters.at("runtime_fingerprint");
  if (!fingerprint.is_null()) {
    if (auto validated = validate_runtime_fingerprint(fingerprint); !validated)
      return validated;
  }
  return {};
}

[[nodiscard]] ValidationResult
validate_request_message(const nlohmann::json &message) {
  if (!has_exact_keys(message, {"version", "method", "request_id", "params"}) ||
      !message.at("version").is_number_unsigned() ||
      message.at("version").get<unsigned>() != protocol_version ||
      !message.at("method").is_string() ||
      !message.at("request_id").is_string() ||
      !valid_identifier(
          message.at("request_id").get_ref<const std::string &>()) ||
      !message.at("params").is_object()) {
    return std::unexpected(make_error("invalid request envelope"));
  }

  const auto &method = message.at("method").get_ref<const std::string &>();
  const auto &parameters = message.at("params");
  if (method == "Handshake" || method == "GetHealth") {
    if (!parameters.empty())
      return std::unexpected(make_error("method takes no parameters"));
    return {};
  }
  if (method == "Register")
    return validate_registration(parameters);
  if (method == "Unregister" || method == "GetRuntimeStatus" ||
      method == "SubmitCreateCheckpoint" ||
      method == "SubmitRestoreOrRestart" || method == "SubmitStopContainer") {
    if (!has_exact_keys(parameters, {"container_id"}) ||
        !parameters.at("container_id").is_string() ||
        !valid_identifier(
            parameters.at("container_id").get_ref<const std::string &>())) {
      return std::unexpected(make_error("invalid container parameters"));
    }
    return {};
  }
  if (method == "GetOperation") {
    if (!has_exact_keys(parameters, {"operation_id"}) ||
        !parameters.at("operation_id").is_string() ||
        !valid_identifier(
            parameters.at("operation_id").get_ref<const std::string &>())) {
      return std::unexpected(make_error("invalid operation parameters"));
    }
    return {};
  }
  return std::unexpected(make_error("unknown method"));
}

[[nodiscard]] bool valid_error_code(const std::string_view code) {
  return code == "invalid_request" || code == "permission_denied" ||
         code == "server_busy" || code == "request_failed" ||
         code == "internal_error";
}

[[nodiscard]] ValidationResult
validate_response_message(const nlohmann::json &message) {
  if (!message.is_object() || !message.contains("version") ||
      !message.at("version").is_number_unsigned() ||
      message.at("version").get<unsigned>() != protocol_version ||
      !message.contains("request_id") ||
      !message.at("request_id").is_string() ||
      !valid_identifier(
          message.at("request_id").get_ref<const std::string &>()) ||
      !message.contains("ok") || !message.at("ok").is_boolean() ||
      !message.contains("close_connection") ||
      !message.at("close_connection").is_boolean()) {
    return std::unexpected(make_error("invalid response envelope"));
  }
  if (message.at("ok").get<bool>()) {
    if (!has_exact_keys(message, {"version", "request_id", "ok", "result",
                                  "close_connection"}) ||
        !message.at("result").is_object()) {
      return std::unexpected(make_error("invalid success response"));
    }
  } else {
    if (!has_exact_keys(message, {"version", "request_id", "ok", "error_code",
                                  "close_connection"}) ||
        !message.at("error_code").is_string() ||
        !valid_error_code(
            message.at("error_code").get_ref<const std::string &>())) {
      return std::unexpected(make_error("invalid error response"));
    }
  }
  return {};
}

template <typename Validator>
[[nodiscard]] std::expected<nlohmann::json, CodecError>
decode_message(const std::string_view bytes, Validator validator) {
  if (bytes.empty() || bytes.size() > max_message_bytes) {
    return std::unexpected(
        make_error("message size is outside allowed bounds"));
  }
  try {
    auto message = nlohmann::json::parse(bytes);
    if (auto validated = validator(message); !validated)
      return std::unexpected(validated.error());
    return message;
  } catch (const std::exception &) {
    return std::unexpected(make_error("malformed JSON"));
  }
}

template <typename Validator>
[[nodiscard]] std::expected<std::string, CodecError>
encode_message(const nlohmann::json &message, Validator validator) {
  try {
    if (auto validated = validator(message); !validated)
      return std::unexpected(validated.error());
    auto encoded =
        message.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
    if (encoded.empty() || encoded.size() > max_message_bytes) {
      return std::unexpected(make_error("encoded message exceeds size bound"));
    }
    return encoded;
  } catch (const std::exception &) {
    return std::unexpected(make_error("message cannot be encoded"));
  }
}
} // namespace

bool valid_identifier(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 128)
    return false;
  return std::all_of(value.begin(), value.end(), [](const unsigned char c) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    return letter || digit || c == '_' || c == '-' || c == '.' || c == ':';
  });
}

std::expected<nlohmann::json, CodecError>
decode_request(const std::string_view bytes) {
  return decode_message(bytes, validate_request_message);
}

std::expected<nlohmann::json, CodecError>
decode_response(const std::string_view bytes) {
  return decode_message(bytes, validate_response_message);
}

std::expected<std::string, CodecError>
encode_request(const nlohmann::json &message) {
  return encode_message(message, validate_request_message);
}

std::expected<std::string, CodecError>
encode_response(const nlohmann::json &message) {
  return encode_message(message, validate_response_message);
}
} // namespace gravesentinel::ipc
