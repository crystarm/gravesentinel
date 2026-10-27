#include "src/hypervisor/rpc_handler.hxx"

#include "src/ipc/codec.hxx"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::hypervisor {
namespace {

using ccm::CheckpointErrorDetail;
using ccm::CheckpointResult;
using ccm::ContainerId;
using ccm::OperationId;
using ccm::RequestId;
using ccm::RuntimeContainerRef;
using ccm::StableErrorCode;
using ipc::OperationState;
using ipc::OperationView;
using ipc::StoreError;
using ipc::StoreErrorCode;

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

template <typename Integer>
[[nodiscard]] std::optional<Integer>
nonnegative_integer(const nlohmann::json &value) {
  std::uint64_t parsed{};
  if (value.is_number_unsigned()) {
    parsed = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0)
      return std::nullopt;
    parsed = static_cast<std::uint64_t>(signed_value);
  } else {
    return std::nullopt;
  }
  if (parsed > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max()))
    return std::nullopt;
  return static_cast<Integer>(parsed);
}

template <typename Identifier>
[[nodiscard]] std::optional<Identifier>
decode_identifier(const nlohmann::json &value) {
  if (!value.is_string())
    return std::nullopt;
  const auto &text = value.get_ref<const std::string &>();
  if (!ipc::valid_identifier(text))
    return std::nullopt;
  auto identifier = Identifier::create(text);
  if (!identifier)
    return std::nullopt;
  return std::move(*identifier);
}

[[nodiscard]] std::optional<ccm::ColdStartSpec>
decode_cold_start(const nlohmann::json &value) {
  if (!has_exact_keys(value, {"image_reference", "argv", "environment",
                              "mounts", "runtime_options"}) ||
      !value.at("image_reference").is_string() ||
      !value.at("argv").is_array() || !value.at("environment").is_array() ||
      !value.at("mounts").is_array() ||
      !value.at("runtime_options").is_array()) {
    return std::nullopt;
  }

  ccm::ColdStartSpec specification{
      .image_reference = value.at("image_reference").get<std::string>(),
      .argv = {},
      .environment = {},
      .mounts = {},
      .runtime_options = {},
  };
  for (const auto &argument : value.at("argv")) {
    if (!argument.is_string())
      return std::nullopt;
    specification.argv.push_back(argument.get<std::string>());
  }
  for (const auto &entry : value.at("environment")) {
    if (!has_exact_keys(entry, {"name", "value"}) ||
        !entry.at("name").is_string() || !entry.at("value").is_string()) {
      return std::nullopt;
    }
    specification.environment.push_back(
        {.name = entry.at("name").get<std::string>(),
         .value = entry.at("value").get<std::string>()});
  }
  for (const auto &entry : value.at("mounts")) {
    if (!has_exact_keys(entry, {"source", "target", "read_only"}) ||
        !entry.at("source").is_string() || !entry.at("target").is_string() ||
        !entry.at("read_only").is_boolean()) {
      return std::nullopt;
    }
    specification.mounts.push_back(
        {.source = entry.at("source").get<std::string>(),
         .target = entry.at("target").get<std::string>(),
         .read_only = entry.at("read_only").get<bool>()});
  }
  for (const auto &option : value.at("runtime_options")) {
    if (!option.is_string())
      return std::nullopt;
    specification.runtime_options.push_back(option.get<std::string>());
  }
  return specification;
}

[[nodiscard]] std::optional<ccm::RuntimeFingerprint>
decode_runtime_fingerprint(const nlohmann::json &value) {
  if (!has_exact_keys(value, {"runtime_name",         "runtime_version",
                              "runtime_build_origin", "architecture",
                              "kernel_release",       "criu_version",
                              "oci_runtime_name",     "oci_runtime_version",
                              "oci_runtime_commit",   "oci_runtime_features",
                              "cgroup_version",       "cgroup_manager",
                              "cgroup_controllers",   "privilege_mode",
                              "user_namespace_mode",  "capability_profile",
                              "selinux_mode",         "apparmor_profile",
                              "seccomp_profile",      "command_profile"})) {
    return std::nullopt;
  }

  const std::string_view keys[]{
      "runtime_name",         "runtime_version",     "runtime_build_origin",
      "architecture",         "kernel_release",      "criu_version",
      "oci_runtime_name",     "oci_runtime_version", "oci_runtime_commit",
      "oci_runtime_features", "cgroup_version",      "cgroup_manager",
      "cgroup_controllers",   "privilege_mode",      "user_namespace_mode",
      "capability_profile",   "selinux_mode",        "apparmor_profile",
      "seccomp_profile",      "command_profile",
  };
  for (const auto key : keys) {
    if (!value.at(std::string(key)).is_string())
      return std::nullopt;
  }

  return ccm::RuntimeFingerprint{
      .runtime_name = value.at("runtime_name").get<std::string>(),
      .runtime_version = value.at("runtime_version").get<std::string>(),
      .runtime_build_origin =
          value.at("runtime_build_origin").get<std::string>(),
      .architecture = value.at("architecture").get<std::string>(),
      .kernel_release = value.at("kernel_release").get<std::string>(),
      .criu_version = value.at("criu_version").get<std::string>(),
      .oci_runtime_name = value.at("oci_runtime_name").get<std::string>(),
      .oci_runtime_version = value.at("oci_runtime_version").get<std::string>(),
      .oci_runtime_commit = value.at("oci_runtime_commit").get<std::string>(),
      .oci_runtime_features =
          value.at("oci_runtime_features").get<std::string>(),
      .cgroup_version = value.at("cgroup_version").get<std::string>(),
      .cgroup_manager = value.at("cgroup_manager").get<std::string>(),
      .cgroup_controllers = value.at("cgroup_controllers").get<std::string>(),
      .privilege_mode = value.at("privilege_mode").get<std::string>(),
      .user_namespace_mode = value.at("user_namespace_mode").get<std::string>(),
      .capability_profile = value.at("capability_profile").get<std::string>(),
      .selinux_mode = value.at("selinux_mode").get<std::string>(),
      .apparmor_profile = value.at("apparmor_profile").get<std::string>(),
      .seccomp_profile = value.at("seccomp_profile").get<std::string>(),
      .command_profile = value.at("command_profile").get<std::string>(),
  };
}

[[nodiscard]] const char *stable_error_name(const StableErrorCode code) {
  switch (code) {
  case StableErrorCode::invalid_argument:
    return "invalid_argument";
  case StableErrorCode::not_found:
    return "not_found";
  case StableErrorCode::already_exists:
    return "already_exists";
  case StableErrorCode::resource_exhausted:
    return "resource_exhausted";
  case StableErrorCode::conflict:
    return "conflict";
  case StableErrorCode::permission_denied:
    return "permission_denied";
  case StableErrorCode::capture_failed:
    return "capture_failed";
  case StableErrorCode::storage_error:
    return "storage_error";
  case StableErrorCode::corrupted_snapshot:
    return "corrupted_snapshot";
  case StableErrorCode::incompatible_runtime:
    return "incompatible_runtime";
  case StableErrorCode::restore_failed:
    return "restore_failed";
  case StableErrorCode::cold_start_failed:
    return "cold_start_failed";
  case StableErrorCode::deadline_exceeded:
    return "deadline_exceeded";
  case StableErrorCode::unavailable:
    return "unavailable";
  case StableErrorCode::internal:
    return "internal";
  }
  return "internal";
}

[[nodiscard]] StoreErrorCode
store_error_code(const StableErrorCode code) noexcept {
  switch (code) {
  case StableErrorCode::invalid_argument:
    return StoreErrorCode::invalid_argument;
  case StableErrorCode::not_found:
    return StoreErrorCode::not_found;
  case StableErrorCode::already_exists:
    return StoreErrorCode::already_exists;
  case StableErrorCode::resource_exhausted:
    return StoreErrorCode::resource_exhausted;
  case StableErrorCode::conflict:
    return StoreErrorCode::conflict;
  case StableErrorCode::permission_denied:
    return StoreErrorCode::permission_denied;
  case StableErrorCode::capture_failed:
    return StoreErrorCode::capture_failed;
  case StableErrorCode::storage_error:
    return StoreErrorCode::storage_error;
  case StableErrorCode::corrupted_snapshot:
    return StoreErrorCode::corrupted_snapshot;
  case StableErrorCode::incompatible_runtime:
    return StoreErrorCode::incompatible_runtime;
  case StableErrorCode::restore_failed:
    return StoreErrorCode::restore_failed;
  case StableErrorCode::cold_start_failed:
    return StoreErrorCode::cold_start_failed;
  case StableErrorCode::deadline_exceeded:
    return StoreErrorCode::deadline_exceeded;
  case StableErrorCode::unavailable:
    return StoreErrorCode::unavailable;
  case StableErrorCode::internal:
    return StoreErrorCode::internal;
  }
  return StoreErrorCode::internal;
}

[[nodiscard]] std::string core_error_marker(const StableErrorCode code) {
  return std::string("core_error/") + stable_error_name(code);
}

[[nodiscard]] std::optional<StableErrorCode>
stable_error_from_marker(const std::string_view marker) {
  constexpr StableErrorCode codes[]{
      StableErrorCode::invalid_argument,
      StableErrorCode::not_found,
      StableErrorCode::already_exists,
      StableErrorCode::resource_exhausted,
      StableErrorCode::conflict,
      StableErrorCode::permission_denied,
      StableErrorCode::capture_failed,
      StableErrorCode::storage_error,
      StableErrorCode::corrupted_snapshot,
      StableErrorCode::incompatible_runtime,
      StableErrorCode::restore_failed,
      StableErrorCode::cold_start_failed,
      StableErrorCode::deadline_exceeded,
      StableErrorCode::unavailable,
      StableErrorCode::internal,
  };
  for (const auto code : codes) {
    if (marker == core_error_marker(code))
      return code;
  }
  return std::nullopt;
}

[[nodiscard]] const char *fallback_wire_error(const OperationView &operation) {
  switch (operation.error_code) {
  case StoreErrorCode::invalid_argument:
    return "invalid_argument";
  case StoreErrorCode::not_found:
    return "not_found";
  case StoreErrorCode::already_exists:
    return "already_exists";
  case StoreErrorCode::conflict:
    return "conflict";
  case StoreErrorCode::resource_exhausted:
    return "resource_exhausted";
  case StoreErrorCode::permission_denied:
    return "permission_denied";
  case StoreErrorCode::capture_failed:
    return "capture_failed";
  case StoreErrorCode::storage_error:
    return "storage_error";
  case StoreErrorCode::corrupted_snapshot:
    return "corrupted_snapshot";
  case StoreErrorCode::incompatible_runtime:
    return "incompatible_runtime";
  case StoreErrorCode::restore_failed:
    return "restore_failed";
  case StoreErrorCode::cold_start_failed:
    return "cold_start_failed";
  case StoreErrorCode::deadline_exceeded:
    return "deadline_exceeded";
  case StoreErrorCode::unavailable:
  case StoreErrorCode::execution_indeterminate:
  case StoreErrorCode::interrupted:
    return "unavailable";
  case StoreErrorCode::journal_io:
  case StoreErrorCode::journal_corrupt:
    return "storage_error";
  case StoreErrorCode::executor_failed:
    if (operation.method == "SubmitCreateCheckpoint")
      return "capture_failed";
    if (operation.method == "SubmitRestoreOrRestart")
      return "restore_failed";
    return "unavailable";
  case StoreErrorCode::none:
  case StoreErrorCode::internal:
    return "internal";
  }
  return "internal";
}

[[nodiscard]] const char *wire_error(const OperationView &operation) {
  if (const auto code = stable_error_from_marker(operation.result);
      code && store_error_code(*code) == operation.error_code) {
    return stable_error_name(*code);
  }
  return fallback_wire_error(operation);
}

[[nodiscard]] nlohmann::json failure() { return {{"ok", false}}; }

[[nodiscard]] nlohmann::json busy_failure() {
  return {{"ok", false}, {"error_code", "server_busy"}};
}

[[nodiscard]] bool
has_side_effect(const CheckpointErrorDetail &error) noexcept {
  return error.storage_side_effect != ccm::StorageSideEffect::none ||
         error.runtime_side_effect != ccm::RuntimeSideEffect::none;
}

} // namespace

std::expected<ccm::ContainerRegistration, RegistrationDecodeError>
decode_container_registration(const nlohmann::json &parameters) {
  try {
    if (!has_exact_keys(parameters,
                        {"container_id", "runtime_container", "checkpoint",
                         "cold_start", "runtime_fingerprint"})) {
      return std::unexpected(RegistrationDecodeError::invalid_schema);
    }
    auto container =
        decode_identifier<ContainerId>(parameters.at("container_id"));
    auto runtime = decode_identifier<RuntimeContainerRef>(
        parameters.at("runtime_container"));
    const auto &checkpoint = parameters.at("checkpoint");
    if (!container || !runtime ||
        !has_exact_keys(checkpoint, {"interval_seconds", "keep_last",
                                     "container_cpus", "checkpoint_cpu"}) ||
        !checkpoint.at("container_cpus").is_array()) {
      return std::unexpected(RegistrationDecodeError::invalid_schema);
    }

    const auto interval = nonnegative_integer<std::chrono::seconds::rep>(
        checkpoint.at("interval_seconds"));
    const auto keep_last =
        nonnegative_integer<std::size_t>(checkpoint.at("keep_last"));
    std::vector<std::uint32_t> container_cpu_values;
    container_cpu_values.reserve(checkpoint.at("container_cpus").size());
    for (const auto &encoded_cpu : checkpoint.at("container_cpus")) {
      const auto cpu = nonnegative_integer<std::uint32_t>(encoded_cpu);
      if (!cpu) {
        return std::unexpected(RegistrationDecodeError::invalid_schema);
      }
      container_cpu_values.push_back(*cpu);
    }
    auto container_cpus = ccm::CpuSet::create(std::move(container_cpu_values));
    const auto checkpoint_cpu =
        nonnegative_integer<std::uint32_t>(checkpoint.at("checkpoint_cpu"));
    if (!interval || !keep_last || !container_cpus || !checkpoint_cpu)
      return std::unexpected(RegistrationDecodeError::invalid_schema);

    std::optional<ccm::ColdStartSpec> cold_start;
    if (!parameters.at("cold_start").is_null()) {
      cold_start = decode_cold_start(parameters.at("cold_start"));
      if (!cold_start)
        return std::unexpected(RegistrationDecodeError::invalid_schema);
    }
    std::optional<ccm::RuntimeFingerprint> fingerprint;
    if (!parameters.at("runtime_fingerprint").is_null()) {
      fingerprint =
          decode_runtime_fingerprint(parameters.at("runtime_fingerprint"));
      if (!fingerprint)
        return std::unexpected(RegistrationDecodeError::invalid_schema);
    }

    return ccm::ContainerRegistration{
        .container_id = std::move(*container),
        .runtime_container = std::move(*runtime),
        .checkpoint = {.interval = std::chrono::seconds{*interval},
                       .keep_last = *keep_last,
                       .container_cpus = std::move(*container_cpus),
                       .checkpoint_cpu = *checkpoint_cpu},
        .cold_start = std::move(cold_start),
        .runtime_fingerprint = std::move(fingerprint),
    };
  } catch (const std::exception &) {
    return std::unexpected(RegistrationDecodeError::invalid_schema);
  }
}

ipc::OperationStore::Executor
make_operation_executor(ccm::ICheckpointService &service) {
  return [&service](const OperationView &operation)
             -> std::expected<std::string, StoreError> {
    try {
      auto request = RequestId::create(operation.request_id);
      auto container = ContainerId::create(operation.container_id);
      auto operation_id = OperationId::create(operation.operation_id);
      if (!request || !container || !operation_id ||
          !ipc::valid_identifier(operation.request_id) ||
          !ipc::valid_identifier(operation.container_id) ||
          !ipc::valid_identifier(operation.operation_id)) {
        return std::unexpected(StoreError{StoreErrorCode::invalid_argument,
                                          "operation_identifiers_invalid"});
      }

      CheckpointResult<void> result;
      if (operation.method == "SubmitCreateCheckpoint") {
        result = service.create_checkpoint(*container, *request, *operation_id);
      } else if (operation.method == "SubmitRestoreOrRestart") {
        result =
            service.restore_or_restart(*container, *request, *operation_id);
      } else if (operation.method == "SubmitStopContainer") {
        result = service.stop_container(*container, *request, *operation_id);
      } else {
        return std::unexpected(StoreError{StoreErrorCode::internal,
                                          "operation_method_unsupported"});
      }
      if (!result) {
        const auto &error = result.error();
        return std::unexpected(StoreError{store_error_code(error.code),
                                          core_error_marker(error.code),
                                          has_side_effect(error)});
      }
      return std::string("success");
    } catch (...) {
      return std::unexpected(StoreError{StoreErrorCode::execution_indeterminate,
                                        "operation_executor_exception", true});
    }
  };
}

RpcHandler::RpcHandler(ccm::ICheckpointService &service,
                       ipc::OperationStore &operation_store) noexcept
    : service_(service), operation_store_(operation_store) {}

nlohmann::json RpcHandler::operator()(const nlohmann::json &request) const {
  try {
    if (!request.is_object() || !request.contains("method") ||
        !request.at("method").is_string() || !request.contains("request_id") ||
        !request.at("request_id").is_string() || !request.contains("params") ||
        !request.at("params").is_object()) {
      return failure();
    }
    const auto &method = request.at("method").get_ref<const std::string &>();
    const auto &request_id_text =
        request.at("request_id").get_ref<const std::string &>();
    const auto &parameters = request.at("params");

    if (method == "Handshake") {
      return {{"protocol_version", ipc::protocol_version},
              {"service", "gravesentinel_hypervisor"}};
    }
    if (method == "GetHealth") {
      return {{"status", "healthy"},
              {"queue", operation_store_.accepting_submissions() ? "available"
                                                                 : "saturated"},
              {"protocol_version", ipc::protocol_version}};
    }

    auto request_id = decode_identifier<RequestId>(request.at("request_id"));
    if (!request_id)
      return failure();

    if (method == "GetOperation") {
      if (!has_exact_keys(parameters, {"operation_id"}) ||
          !decode_identifier<OperationId>(parameters.at("operation_id"))) {
        return failure();
      }
      const auto &operation_id =
          parameters.at("operation_id").get_ref<const std::string &>();
      auto found = operation_store_.get(operation_id);
      if (!found)
        return failure();

      nlohmann::json error_code = nullptr;
      if (found->state == OperationState::failed ||
          found->state == OperationState::indeterminate) {
        error_code = wire_error(*found);
      }
      return {{"operation_id", found->operation_id},
              {"state", ipc::state_name(found->state)},
              {"error_code", std::move(error_code)}};
    }

    if (method == "GetRuntimeStatus") {
      if (!has_exact_keys(parameters, {"container_id"}))
        return failure();
      auto container =
          decode_identifier<ContainerId>(parameters.at("container_id"));
      if (!container)
        return failure();
      auto status = service_.get_runtime_status(*container, *request_id);
      if (!status)
        return failure();
      const auto state = status->state == ccm::RuntimeState::absent ? "absent"
                         : status->state == ccm::RuntimeState::running
                             ? "running"
                             : "exited";
      nlohmann::json exit_code = nullptr;
      if (status->exit_code.has_value())
        exit_code = *status->exit_code;
      return {{"state", state}, {"exit_code", std::move(exit_code)}};
    }

    if (method == "Register") {
      auto registration = decode_container_registration(parameters);
      if (!registration ||
          !ccm::validate_container_registration(*registration)) {
        return failure();
      }
      if (!service_.register_container(*registration, *request_id))
        return failure();
      return nlohmann::json::object();
    }

    if (method == "Unregister") {
      if (!has_exact_keys(parameters, {"container_id"}))
        return failure();
      auto container =
          decode_identifier<ContainerId>(parameters.at("container_id"));
      if (!container || !service_.unregister_container(*container, *request_id))
        return failure();
      return nlohmann::json::object();
    }

    if (method == "SubmitCreateCheckpoint" ||
        method == "SubmitRestoreOrRestart" || method == "SubmitStopContainer") {
      if (!has_exact_keys(parameters, {"container_id"}))
        return failure();
      auto container =
          decode_identifier<ContainerId>(parameters.at("container_id"));
      if (!container)
        return failure();
      const auto canonical = nlohmann::json{
          {"method", method},
          {"container_id",
           container->value()}}.dump();
      auto submitted = operation_store_.submit(request_id_text, method,
                                               container->value(), canonical);
      if (!submitted) {
        switch (submitted.error().code) {
        case StoreErrorCode::resource_exhausted:
        case StoreErrorCode::unavailable:
        case StoreErrorCode::journal_io:
        case StoreErrorCode::journal_corrupt:
          return busy_failure();
        default:
          return failure();
        }
      }
      return {{"operation_id", submitted->operation_id},
              {"duplicate", submitted->duplicate}};
    }
    return failure();
  } catch (const std::exception &) {
    return failure();
  }
}

} // namespace gravesentinel::hypervisor
