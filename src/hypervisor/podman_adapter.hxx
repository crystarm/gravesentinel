#pragma once

#include "src/core/contracts.hxx"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace gravesentinel::ccm {

struct PodmanAdapterConfig {
  std::filesystem::path podman_executable;
  std::filesystem::path criu_executable;
  std::filesystem::path staging_root;
  std::vector<EnvironmentVariable> environment;

  std::chrono::milliseconds profile_timeout{std::chrono::seconds{10}};
  std::chrono::milliseconds inspection_timeout{std::chrono::seconds{10}};
  std::chrono::milliseconds operation_timeout{std::chrono::minutes{5}};
  std::chrono::milliseconds cleanup_timeout{std::chrono::seconds{30}};
  std::chrono::milliseconds termination_grace_period{std::chrono::seconds{2}};

  std::size_t maximum_json_bytes{1024U * 1024U};
  std::size_t maximum_standard_output_bytes{1024U * 1024U};
  std::size_t maximum_standard_error_bytes{64U * 1024U};
  std::size_t maximum_arguments{256U};
  std::size_t maximum_argument_bytes{16U * 1024U};
  std::size_t maximum_environment_variables{256U};
  std::size_t maximum_environment_value_bytes{64U * 1024U};
};

class PodmanAdapter final : public IPodmanAdapter {
public:
  static constexpr std::string_view managed_label =
      "io.gravesentinel.ccm.managed";
  static constexpr std::string_view logical_id_label =
      "io.gravesentinel.ccm.logical-id";
  static constexpr std::string_view command_profile = "podman-criu-rootful-v2";
  static constexpr std::string_view checkpoint_artifact_name = "checkpoint.tar";

  PodmanAdapter(IProcessRunner &process_runner,
                IAffinityController &affinity_controller, ILogger &logger,
                PodmanAdapterConfig config);
  ~PodmanAdapter() override;

  PodmanAdapter(const PodmanAdapter &) = delete;
  PodmanAdapter &operator=(const PodmanAdapter &) = delete;
  PodmanAdapter(PodmanAdapter &&) = delete;
  PodmanAdapter &operator=(PodmanAdapter &&) = delete;

  [[nodiscard]] CheckpointResult<void>
  validate_restore_compatibility(const RuntimeFingerprint &snapshot_fingerprint,
                                 std::string_view source_image_identity,
                                 RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<CapturedSnapshot>
  capture(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &staging_directory,
          RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<RuntimeObservation>
  inspect_runtime(const ContainerId &container_id,
                  const RuntimeContainerRef &runtime_container,
                  RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<void>
  stop_and_remove(const ContainerId &container_id,
                  const RuntimeIncarnation &expected_incarnation,
                  RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<void>
  confirm_absence(const ContainerId &container_id,
                  const RuntimeContainerRef &runtime_container,
                  RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  restore(const ContainerId &container_id,
          const RuntimeContainerRef &runtime_container,
          const std::filesystem::path &snapshot_artifact,
          const RuntimeFingerprint &snapshot_fingerprint,
          std::string_view source_image_identity,
          RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<RuntimeIncarnation>
  cold_start(const ContainerId &container_id,
             const RuntimeContainerRef &runtime_container,
             const ColdStartSpec &spec,
             RuntimeCpuAssignment cpu_assignment) override;
  [[nodiscard]] CheckpointResult<void>
  cleanup_failed_restore(const ContainerId &container_id,
                         const RuntimeContainerRef &runtime_container,
                         RuntimeCpuAssignment cpu_assignment) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gravesentinel::ccm
