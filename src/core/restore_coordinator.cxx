#include "src/core/restore_coordinator.hxx"

#include "src/manifest/codec.hxx"
#include "src/manifest/types.hxx"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

[[nodiscard]] constexpr std::string_view
state_name(RestoreState state) noexcept {
  switch (state) {
  case RestoreState::validating:
    return "VALIDATING";
  case RestoreState::reassembling:
    return "REASSEMBLING";
  case RestoreState::restoring:
    return "RESTORING";
  case RestoreState::restored:
    return "RESTORED";
  case RestoreState::failed:
    return "FAILED";
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
      .runtime_side_effect = RuntimeSideEffect::none,
  };
}

void attach_correlation(CheckpointErrorDetail &error,
                        const RequestId &request_id) {
  if (!error.correlation_id.has_value()) {
    error.correlation_id = request_id;
  }
}

void merge_secondary_error(CheckpointErrorDetail &primary,
                           CheckpointErrorDetail secondary,
                           const RequestId &request_id,
                           std::string_view context) {
  attach_correlation(secondary, request_id);
  primary.retryable = primary.retryable || secondary.retryable;
  if (primary.storage_side_effect == StorageSideEffect::none) {
    primary.storage_side_effect = secondary.storage_side_effect;
  }
  if (secondary.runtime_side_effect ==
      RuntimeSideEffect::incarnation_may_exist) {
    primary.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
  }
  primary.diagnostic += "; ";
  primary.diagnostic += context;
  primary.diagnostic += ": ";
  primary.diagnostic += secondary.diagnostic;
}

void log_transition(ILogger &logger, const RestoreRequest &request,
                    RestoreState from, RestoreState to,
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
    fields.push_back(
        {"diagnostic",
         std::string_view{error->diagnostic.data(), error->diagnostic.size()}});
  }

  logger.log(error == nullptr ? LogLevel::info : LogLevel::error,
             "checkpoint restore state transition", fields);
}

void log_candidate_rejected(ILogger &logger, const RestoreRequest &request,
                            std::uint64_t generation,
                            const CheckpointId *checkpoint_id,
                            const CheckpointErrorDetail &error) {
  const auto generation_text = std::to_string(generation);
  std::vector<LogField> fields{
      {"container_id", request.container_id.value()},
      {"operation_id", request.operation_id.value()},
      {"request_id", request.request_id.value()},
      {"generation", generation_text},
      {"error_code", error_code_name(error.code)},
      {"diagnostic", error.diagnostic},
  };
  if (checkpoint_id != nullptr) {
    fields.push_back({"checkpoint_id", checkpoint_id->value()});
  }
  logger.log(LogLevel::warning, "checkpoint restore candidate rejected",
             fields);
}

void log_maintenance_failure(ILogger &logger, const RestoreRequest &request,
                             std::string_view phase,
                             const CheckpointErrorDetail &error) {
  const std::vector<LogField> fields{
      {"container_id", request.container_id.value()},
      {"operation_id", request.operation_id.value()},
      {"request_id", request.request_id.value()},
      {"phase", phase},
      {"error_code", error_code_name(error.code)},
      {"diagnostic", error.diagnostic},
  };
  logger.log(LogLevel::error, "checkpoint restore maintenance failed", fields);
}

[[nodiscard]] CheckpointErrorDetail
manifest_error(const ManifestError &error, const RequestId &request_id) {
  return make_error(StableErrorCode::corrupted_snapshot,
                    "Manifest v1 validation failed: " + error.diagnostic,
                    request_id);
}

[[nodiscard]] bool
candidate_can_be_skipped(const CheckpointErrorDetail &error) {
  return error.code == StableErrorCode::corrupted_snapshot ||
         error.code == StableErrorCode::incompatible_runtime ||
         error.code == StableErrorCode::not_found;
}

[[nodiscard]] std::string candidate_directory_name(std::uint64_t generation) {
  return "snapshot-" + std::to_string(generation);
}

} // namespace

RestoreCoordinator::RestoreCoordinator(IStorage &storage,
                                       IPodmanAdapter &podman_adapter,
                                       const IHasher &hasher, ILogger &logger)
    : storage_(storage), podman_adapter_(podman_adapter), hasher_(hasher),
      logger_(logger) {}

CheckpointResult<RestoredContainer>
RestoreCoordinator::restore_or_restart(const RestoreRequest &request) {
  RestoreState state = RestoreState::validating;

  if (request.expected_source_incarnation.has_value() &&
      request.expected_source_incarnation->reference !=
          request.runtime_container) {
    auto error = make_error(
        StableErrorCode::invalid_argument,
        "expected source incarnation does not use the registered runtime "
        "reference",
        request.request_id);
    log_transition(logger_, request, state, RestoreState::failed, nullptr,
                   nullptr, &error);
    return std::unexpected(std::move(error));
  }

  auto operation =
      storage_.begin_operation(request.container_id, request.operation_id,
                               StorageOperationKind::restore);
  if (!operation) {
    auto error = operation.error();
    attach_correlation(error, request.request_id);
    log_transition(logger_, request, state, RestoreState::failed, nullptr,
                   nullptr, &error);
    return std::unexpected(std::move(error));
  }

  const auto finish_failure =
      [&](CheckpointErrorDetail error, std::unique_ptr<IStoragePin> *pin,
          const CheckpointId *checkpoint_id, const std::uint64_t *generation)
      -> CheckpointResult<RestoredContainer> {
    attach_correlation(error, request.request_id);
    if (pin != nullptr && *pin != nullptr) {
      auto released = (*pin)->release();
      pin->reset();
      if (!released) {
        log_maintenance_failure(logger_, request, "release manifest pin",
                                released.error());
        merge_secondary_error(error, released.error(), request.request_id,
                              "manifest pin release failed");
      }
    }
    auto cleaned = (*operation)->cleanup();
    if (!cleaned) {
      log_maintenance_failure(logger_, request, "cleanup restore staging",
                              cleaned.error());
      merge_secondary_error(error, cleaned.error(), request.request_id,
                            "restore staging cleanup failed");
    }
    log_transition(logger_, request, state, RestoreState::failed, checkpoint_id,
                   generation, &error);
    state = RestoreState::failed;
    return std::unexpected(std::move(error));
  };

  auto manifests = storage_.list_manifests(request.container_id);
  if (!manifests) {
    return finish_failure(manifests.error(), nullptr, nullptr, nullptr);
  }
  std::ranges::sort(*manifests, std::greater{}, &StoredManifest::generation);
  for (std::size_t index = 0; index < manifests->size(); ++index) {
    if ((*manifests)[index].generation == 0U ||
        (index > 0U && (*manifests)[index - 1U].generation ==
                           (*manifests)[index].generation)) {
      return finish_failure(
          make_error(StableErrorCode::corrupted_snapshot,
                     "storage returned invalid or duplicate manifest "
                     "generations",
                     request.request_id),
          nullptr, nullptr, nullptr);
    }
  }

  bool absence_confirmed = false;
  const auto ensure_initial_absence = [&]() -> CheckpointResult<void> {
    if (absence_confirmed) {
      return {};
    }
    CheckpointResult<void> absent;
    if (request.expected_source_incarnation.has_value()) {
      absent = podman_adapter_.stop_and_remove(
          request.container_id, *request.expected_source_incarnation,
          request.cpu_assignment);
    } else {
      absent = podman_adapter_.confirm_absence(request.container_id,
                                               request.runtime_container,
                                               request.cpu_assignment);
    }
    if (!absent) {
      auto error = absent.error();
      attach_correlation(error, request.request_id);
      error.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
      return std::unexpected(std::move(error));
    }
    absence_confirmed = true;
    return {};
  };

  const auto confirm_after_failed_start =
      [&](const CheckpointErrorDetail &start_error) -> CheckpointResult<void> {
    CheckpointResult<void> absent;
    if (start_error.runtime_side_effect ==
        RuntimeSideEffect::incarnation_may_exist) {
      absent = podman_adapter_.cleanup_failed_restore(request.container_id,
                                                      request.runtime_container,
                                                      request.cpu_assignment);
    } else {
      absent = podman_adapter_.confirm_absence(request.container_id,
                                               request.runtime_container,
                                               request.cpu_assignment);
    }
    if (!absent) {
      auto error = absent.error();
      attach_correlation(error, request.request_id);
      error.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
      return std::unexpected(std::move(error));
    }
    absence_confirmed = true;
    return {};
  };

  for (const auto &stored_manifest : *manifests) {
    const auto generation = stored_manifest.generation;
    auto pin = (*operation)->pin_manifest(generation);
    if (!pin) {
      auto error = pin.error();
      attach_correlation(error, request.request_id);
      if (candidate_can_be_skipped(error)) {
        log_candidate_rejected(logger_, request, generation, nullptr, error);
        continue;
      }
      return finish_failure(std::move(error), nullptr, nullptr, &generation);
    }

    const auto reject_candidate =
        [&](CheckpointErrorDetail error,
            const CheckpointId *checkpoint_id =
                nullptr) -> std::optional<CheckpointResult<RestoredContainer>> {
      attach_correlation(error, request.request_id);
      log_candidate_rejected(logger_, request, generation, checkpoint_id,
                             error);
      auto released = (*pin)->release();
      pin->reset();
      if (!released) {
        auto release_error = released.error();
        attach_correlation(release_error, request.request_id);
        return finish_failure(std::move(release_error), nullptr, checkpoint_id,
                              &generation);
      }
      if (state == RestoreState::reassembling) {
        log_transition(logger_, request, state, RestoreState::validating,
                       checkpoint_id, &generation, &error);
        state = RestoreState::validating;
      }
      return std::nullopt;
    };

    auto encoded = storage_.read_manifest(request.container_id, generation);
    if (!encoded) {
      auto error = encoded.error();
      attach_correlation(error, request.request_id);
      if (!candidate_can_be_skipped(error)) {
        return finish_failure(std::move(error), &*pin, nullptr, &generation);
      }
      if (auto terminal = reject_candidate(std::move(error)); terminal) {
        return std::move(*terminal);
      }
      continue;
    }

    auto manifest = decode_manifest_v1(*encoded);
    if (!manifest) {
      if (auto terminal = reject_candidate(
              manifest_error(manifest.error(), request.request_id));
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }
    if (manifest->container_id != request.container_id ||
        manifest->generation != generation) {
      if (auto terminal = reject_candidate(
              make_error(StableErrorCode::corrupted_snapshot,
                         "manifest identity does not match its storage "
                         "reference",
                         request.request_id),
              &manifest->checkpoint_id);
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }

    auto compatible = podman_adapter_.validate_restore_compatibility(
        manifest->runtime_fingerprint, manifest->source_image_identity,
        request.cpu_assignment);
    if (!compatible) {
      auto error = compatible.error();
      attach_correlation(error, request.request_id);
      if (!candidate_can_be_skipped(error)) {
        return finish_failure(std::move(error), &*pin, &manifest->checkpoint_id,
                              &generation);
      }
      if (auto terminal =
              reject_candidate(std::move(error), &manifest->checkpoint_id);
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }
    if (manifest->artifacts.size() != 1U) {
      if (auto terminal = reject_candidate(
              make_error(StableErrorCode::incompatible_runtime,
                         "the runtime restore profile requires exactly one "
                         "opaque checkpoint artifact",
                         request.request_id),
              &manifest->checkpoint_id);
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }

    log_transition(logger_, request, state, RestoreState::reassembling,
                   &manifest->checkpoint_id, &generation);
    state = RestoreState::reassembling;

    SnapshotReassembler reassembler{**operation, hasher_};
    auto snapshot_root = reassembler.reassemble(
        manifest->artifacts, candidate_directory_name(generation));
    if (!snapshot_root) {
      auto error = snapshot_root.error();
      attach_correlation(error, request.request_id);
      if (!candidate_can_be_skipped(error)) {
        return finish_failure(std::move(error), &*pin, &manifest->checkpoint_id,
                              &generation);
      }
      if (auto terminal =
              reject_candidate(std::move(error), &manifest->checkpoint_id);
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }

    auto artifact =
        join_within_root(*snapshot_root, manifest->artifacts.front().path);
    if (!artifact) {
      if (auto terminal = reject_candidate(
              manifest_error(artifact.error(), request.request_id),
              &manifest->checkpoint_id);
          terminal) {
        return std::move(*terminal);
      }
      continue;
    }

    if (auto absent = ensure_initial_absence(); !absent) {
      return finish_failure(absent.error(), &*pin, &manifest->checkpoint_id,
                            &generation);
    }

    log_transition(logger_, request, state, RestoreState::restoring,
                   &manifest->checkpoint_id, &generation);
    state = RestoreState::restoring;

    auto restored = podman_adapter_.restore(
        request.container_id, request.runtime_container, *artifact,
        manifest->runtime_fingerprint, manifest->source_image_identity,
        request.cpu_assignment);
    if (!restored) {
      auto restore_error = restored.error();
      attach_correlation(restore_error, request.request_id);
      auto absent = confirm_after_failed_start(restore_error);
      if (!absent) {
        merge_secondary_error(restore_error, absent.error(), request.request_id,
                              "failed restore cleanup did not prove absence");
        restore_error.runtime_side_effect =
            RuntimeSideEffect::incarnation_may_exist;
        return finish_failure(std::move(restore_error), &*pin,
                              &manifest->checkpoint_id, &generation);
      }

      auto released = (*pin)->release();
      pin->reset();
      if (!released) {
        return finish_failure(released.error(), nullptr,
                              &manifest->checkpoint_id, &generation);
      }
      log_transition(logger_, request, state, RestoreState::validating,
                     &manifest->checkpoint_id, &generation, &restore_error);
      state = RestoreState::validating;
      continue;
    }

    log_transition(logger_, request, state, RestoreState::restored,
                   &manifest->checkpoint_id, &generation);
    state = RestoreState::restored;

    auto released = (*pin)->release();
    pin->reset();
    const bool pin_release_complete = released.has_value();
    if (!released) {
      auto error = released.error();
      attach_correlation(error, request.request_id);
      log_maintenance_failure(logger_, request, "release manifest pin", error);
    }
    auto cleaned = (*operation)->cleanup();
    const bool staging_cleanup_complete = cleaned.has_value();
    if (!cleaned) {
      auto error = cleaned.error();
      attach_correlation(error, request.request_id);
      log_maintenance_failure(logger_, request, "cleanup restore staging",
                              error);
    }

    return RestoredContainer{
        .operation_id = request.operation_id,
        .incarnation = std::move(*restored),
        .outcome = RestoreOutcome::checkpoint,
        .checkpoint_id = manifest->checkpoint_id,
        .generation = generation,
        .pin_release_complete = pin_release_complete,
        .staging_cleanup_complete = staging_cleanup_complete,
    };
  }

  if (!request.cold_start.has_value()) {
    return finish_failure(
        make_error(StableErrorCode::restore_failed,
                   manifests->empty()
                       ? "no committed checkpoint is available and cold start "
                         "is not configured"
                       : "no committed checkpoint is usable and cold start is "
                         "not configured",
                   request.request_id),
        nullptr, nullptr, nullptr);
  }

  if (auto absent = ensure_initial_absence(); !absent) {
    return finish_failure(absent.error(), nullptr, nullptr, nullptr);
  }
  log_transition(logger_, request, state, RestoreState::restoring, nullptr,
                 nullptr);
  state = RestoreState::restoring;

  auto started = podman_adapter_.cold_start(
      request.container_id, request.runtime_container, *request.cold_start,
      request.cpu_assignment);
  if (!started) {
    auto error = started.error();
    attach_correlation(error, request.request_id);
    auto absent = confirm_after_failed_start(error);
    if (!absent) {
      merge_secondary_error(error, absent.error(), request.request_id,
                            "failed cold-start cleanup did not prove absence");
      error.runtime_side_effect = RuntimeSideEffect::incarnation_may_exist;
    }
    return finish_failure(std::move(error), nullptr, nullptr, nullptr);
  }

  log_transition(logger_, request, state, RestoreState::restored, nullptr,
                 nullptr);
  state = RestoreState::restored;
  auto cleaned = (*operation)->cleanup();
  const bool staging_cleanup_complete = cleaned.has_value();
  if (!cleaned) {
    auto error = cleaned.error();
    attach_correlation(error, request.request_id);
    log_maintenance_failure(logger_, request, "cleanup restore staging", error);
  }
  return RestoredContainer{
      .operation_id = request.operation_id,
      .incarnation = std::move(*started),
      .outcome = RestoreOutcome::cold_start,
      .checkpoint_id = std::nullopt,
      .generation = std::nullopt,
      .pin_release_complete = true,
      .staging_cleanup_complete = staging_cleanup_complete,
  };
}

} // namespace gravesentinel::ccm
