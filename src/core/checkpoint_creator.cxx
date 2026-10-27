#include "src/core/checkpoint_creator.hxx"

#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] constexpr std::string_view
state_name(CheckpointCreateState state) noexcept {
  switch (state) {
  case CheckpointCreateState::idle:
    return "IDLE";
  case CheckpointCreateState::capturing:
    return "CAPTURING";
  case CheckpointCreateState::chunking:
    return "CHUNKING";
  case CheckpointCreateState::committing:
    return "COMMITTING";
  case CheckpointCreateState::committed:
    return "COMMITTED";
  case CheckpointCreateState::failed:
    return "FAILED";
  case CheckpointCreateState::incomplete:
    return "INCOMPLETE";
  }
  return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view
error_code_name(StableErrorCode code) noexcept {
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
  return "unknown";
}

[[nodiscard]] constexpr std::string_view
storage_side_effect_name(StorageSideEffect side_effect) noexcept {
  switch (side_effect) {
  case StorageSideEffect::none:
    return "none";
  case StorageSideEffect::publication_may_be_visible:
    return "publication_may_be_visible";
  }
  return "unknown";
}

[[nodiscard]] CheckpointErrorDetail make_error(StableErrorCode code,
                                               std::string diagnostic,
                                               const RequestId &request_id,
                                               bool retryable = false) {
  return CheckpointErrorDetail{
      .code = code,
      .diagnostic = std::move(diagnostic),
      .retryable = retryable,
      .correlation_id = request_id,
      .storage_side_effect = StorageSideEffect::none,
  };
}

void attach_correlation(CheckpointErrorDetail &error,
                        const RequestId &request_id) {
  if (!error.correlation_id.has_value()) {
    error.correlation_id = request_id;
  }
}

void log_transition(ILogger &logger, const CheckpointCreateRequest &request,
                    CheckpointCreateState from, CheckpointCreateState to,
                    const CheckpointId *checkpoint_id,
                    const std::uint64_t *generation,
                    const CheckpointErrorDetail *error = nullptr) {
  std::string generation_text;
  std::vector<LogField> fields{
      {"container_id", request.container_id.value()},
      {"operation_id", request.operation_id.value()},
      {"request_id", request.request_id.value()},
      {"from_state", state_name(from)},
      {"to_state", state_name(to)},
  };
  if (checkpoint_id != nullptr) {
    fields.push_back({"checkpoint_id", checkpoint_id->value()});
  }
  if (generation != nullptr) {
    generation_text = std::to_string(*generation);
    fields.push_back({"generation", generation_text});
  }
  if (error != nullptr) {
    fields.push_back({"error_code", error_code_name(error->code)});
    fields.push_back({"retryable", error->retryable ? "true" : "false"});
    fields.push_back({"storage_side_effect",
                      storage_side_effect_name(error->storage_side_effect)});
    fields.push_back({"diagnostic", error->diagnostic});
  }

  const auto level = error == nullptr ? LogLevel::info : LogLevel::error;
  logger.log(level, "checkpoint create state transition", fields);
}

void log_maintenance_failure(ILogger &logger,
                             const CheckpointCreateRequest &request,
                             CheckpointCreateState state,
                             std::string_view message,
                             const CheckpointErrorDetail &error) {
  const std::vector<LogField> fields{
      {"container_id", request.container_id.value()},
      {"operation_id", request.operation_id.value()},
      {"request_id", request.request_id.value()},
      {"state", state_name(state)},
      {"error_code", error_code_name(error.code)},
      {"retryable", error.retryable ? "true" : "false"},
      {"diagnostic", error.diagnostic},
  };
  logger.log(LogLevel::error, message, fields);
}

void log_cleanup_failure(ILogger &logger,
                         const CheckpointCreateRequest &request,
                         CheckpointCreateState state,
                         const CheckpointErrorDetail &error) {
  log_maintenance_failure(logger, request, state,
                          "checkpoint create staging cleanup failed", error);
}

void log_post_commit_exception(ILogger &logger,
                               const CheckpointCreateRequest &request,
                               std::string_view activity) noexcept {
  try {
    const auto error = make_error(
        StableErrorCode::internal,
        std::string{activity} + " threw after the checkpoint was committed",
        request.request_id);
    log_maintenance_failure(logger, request, CheckpointCreateState::committed,
                            "post-commit checkpoint maintenance threw", error);
  } catch (...) {
  }
}

[[nodiscard]] CheckpointResult<std::uint64_t>
wall_time_nanoseconds(const IClock &clock, const RequestId &request_id) {
  const auto elapsed = clock.wall_now().time_since_epoch();
  if (elapsed < decltype(elapsed)::zero()) {
    return std::unexpected(make_error(
        StableErrorCode::internal,
        "wall clock is before the Unix epoch while creating a checkpoint",
        request_id));
  }
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  if (nanoseconds < 0) {
    return std::unexpected(make_error(
        StableErrorCode::internal,
        "wall clock timestamp cannot be represented as Unix nanoseconds",
        request_id));
  }
  return static_cast<std::uint64_t>(nanoseconds);
}

[[nodiscard]] CheckpointResult<void> validate_captured_artifact(
    const CapturedSnapshot &captured,
    const std::filesystem::path &runtime_artifact_directory,
    const RequestId &request_id) {
  const auto root = runtime_artifact_directory.lexically_normal();
  const auto artifact = captured.artifact_path.lexically_normal();
  if (!artifact.is_absolute() || artifact != captured.artifact_path ||
      artifact == root) {
    return std::unexpected(make_error(
        StableErrorCode::capture_failed,
        "runtime adapter returned an invalid checkpoint artifact path",
        request_id));
  }

  const auto relative = artifact.lexically_relative(root);
  if (relative.empty() || relative.is_absolute()) {
    return std::unexpected(make_error(
        StableErrorCode::capture_failed,
        "runtime adapter returned a checkpoint artifact outside staging",
        request_id));
  }
  for (const auto &component : relative) {
    if (component == "." || component == "..") {
      return std::unexpected(make_error(
          StableErrorCode::capture_failed,
          "runtime adapter returned a checkpoint artifact outside staging",
          request_id));
    }
  }

  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(artifact, status_error);
  if (status_error || status.type() != std::filesystem::file_type::regular) {
    return std::unexpected(make_error(
        StableErrorCode::capture_failed,
        "runtime adapter did not produce a regular checkpoint artifact",
        request_id));
  }
  return {};
}

[[nodiscard]] CheckpointErrorDetail
manifest_encoding_error(const ManifestError &error,
                        const RequestId &request_id) {
  return make_error(StableErrorCode::internal,
                    "Manifest v2 serialization failed: " + error.diagnostic,
                    request_id);
}

} // namespace

CheckpointCreator::CheckpointCreator(IStorage &storage,
                                     IPodmanAdapter &podman_adapter,
                                     ISnapshotChunker &snapshot_chunker,
                                     IClock &clock, IIdGenerator &id_generator,
                                     ILogger &logger)
    : storage_(storage), podman_adapter_(podman_adapter),
      snapshot_chunker_(snapshot_chunker), clock_(clock),
      id_generator_(id_generator), logger_(logger) {}

CheckpointResult<CreatedCheckpoint>
CheckpointCreator::create(const CheckpointCreateRequest &request) {
  CheckpointCreateState state = CheckpointCreateState::idle;
  std::optional<CheckpointId> checkpoint_id;
  std::optional<std::uint64_t> generation;

  auto operation =
      storage_.begin_operation(request.container_id, request.operation_id,
                               StorageOperationKind::create_checkpoint);
  if (!operation) {
    auto error = operation.error();
    attach_correlation(error, request.request_id);
    log_transition(logger_, request, state, CheckpointCreateState::failed,
                   nullptr, nullptr, &error);
    return std::unexpected(std::move(error));
  }

  const auto fail =
      [&](CheckpointErrorDetail error) -> CheckpointResult<CreatedCheckpoint> {
    attach_correlation(error, request.request_id);
    const auto cleaned = (*operation)->cleanup();
    const bool incomplete =
        (state == CheckpointCreateState::committing &&
         error.storage_side_effect ==
             StorageSideEffect::publication_may_be_visible) ||
        !cleaned;
    if (!cleaned) {
      auto cleanup_error = cleaned.error();
      attach_correlation(cleanup_error, request.request_id);
      log_cleanup_failure(logger_, request, state, cleanup_error);
      error.retryable = error.retryable || cleanup_error.retryable;
      if (error.storage_side_effect == StorageSideEffect::none) {
        error.storage_side_effect = cleanup_error.storage_side_effect;
      }
      error.diagnostic +=
          "; staging cleanup also failed: " + cleanup_error.diagnostic;
    }
    const auto terminal = incomplete ? CheckpointCreateState::incomplete
                                     : CheckpointCreateState::failed;
    log_transition(logger_, request, state, terminal,
                   checkpoint_id ? &*checkpoint_id : nullptr,
                   generation ? &*generation : nullptr, &error);
    state = terminal;
    return std::unexpected(std::move(error));
  };

  auto generated_id = id_generator_.new_checkpoint_id();
  if (!generated_id) {
    return fail(generated_id.error());
  }
  checkpoint_id = std::move(*generated_id);

  auto manifests = storage_.list_manifests(request.container_id);
  if (!manifests) {
    return fail(manifests.error());
  }
  std::uint64_t maximum_generation = 0;
  for (const auto &manifest : *manifests) {
    maximum_generation = std::max(maximum_generation, manifest.generation);
  }
  if (maximum_generation == std::numeric_limits<std::uint64_t>::max()) {
    return fail(
        make_error(StableErrorCode::resource_exhausted,
                   "checkpoint generation space is exhausted for the container",
                   request.request_id));
  }
  generation = maximum_generation + 1U;

  log_transition(logger_, request, state, CheckpointCreateState::capturing,
                 &*checkpoint_id, &*generation);
  state = CheckpointCreateState::capturing;

  auto captured = podman_adapter_.capture(
      request.container_id, request.runtime_container,
      (*operation)->runtime_artifact_directory(), request.cpu_assignment);
  if (!captured) {
    return fail(captured.error());
  }
  if (auto valid = validate_captured_artifact(
          *captured, (*operation)->runtime_artifact_directory(),
          request.request_id);
      !valid) {
    return fail(valid.error());
  }
  auto created_at = wall_time_nanoseconds(clock_, request.request_id);
  if (!created_at) {
    return fail(created_at.error());
  }

  log_transition(logger_, request, state, CheckpointCreateState::chunking,
                 &*checkpoint_id, &*generation);
  state = CheckpointCreateState::chunking;

  auto artifacts = snapshot_chunker_.ingest(
      **operation, (*operation)->runtime_artifact_directory());
  if (!artifacts) {
    return fail(artifacts.error());
  }

  log_transition(logger_, request, state, CheckpointCreateState::committing,
                 &*checkpoint_id, &*generation);
  state = CheckpointCreateState::committing;

  const ManifestV2 manifest{
      .container_id = request.container_id,
      .checkpoint_id = *checkpoint_id,
      .generation = *generation,
      .created_at_unix_nanoseconds = *created_at,
      .container_cpus = request.cpu_assignment.container_cpus,
      .source_image_identity = captured->source_image_identity,
      .runtime_fingerprint = captured->runtime_fingerprint,
      .artifacts = std::move(*artifacts),
  };
  auto encoded_manifest = encode_manifest_v2(manifest);
  if (!encoded_manifest) {
    return fail(
        manifest_encoding_error(encoded_manifest.error(), request.request_id));
  }
  CreatedCheckpoint created{
      .operation_id = request.operation_id,
      .checkpoint_id = *checkpoint_id,
      .generation = *generation,
      .source_incarnation = captured->source_incarnation,
      .retention_complete = false,
      .staging_cleanup_complete = false,
  };
  if (auto published =
          (*operation)->publish_manifest(*checkpoint_id, *encoded_manifest);
      !published) {
    return fail(published.error());
  }

  state = CheckpointCreateState::committed;
  try {
    log_transition(logger_, request, CheckpointCreateState::committing, state,
                   &*checkpoint_id, &*generation);
  } catch (...) {
  }

  try {
    auto retained = (*operation)->apply_retention(request.keep_last);
    if (!retained) {
      auto retention_error = retained.error();
      attach_correlation(retention_error, request.request_id);
      log_maintenance_failure(logger_, request, state,
                              "post-commit checkpoint retention failed",
                              retention_error);
    } else {
      created.retention_complete = retained->complete;
    }
  } catch (...) {
    log_post_commit_exception(logger_, request, "checkpoint retention");
  }

  try {
    auto cleaned = (*operation)->cleanup();
    created.staging_cleanup_complete = cleaned.has_value();
    if (!cleaned) {
      auto cleanup_error = cleaned.error();
      attach_correlation(cleanup_error, request.request_id);
      log_cleanup_failure(logger_, request, state, cleanup_error);
    }
  } catch (...) {
    log_post_commit_exception(logger_, request, "checkpoint staging cleanup");
  }

  return created;
}

} // namespace gravesentinel::ccm
