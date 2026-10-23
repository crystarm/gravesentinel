#pragma once

#include "src/protocol/types.hxx"

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace gravesentinel::ccm {

enum class RpcFailureCode {
  invalid_argument,
  unavailable,
  deadline_exceeded,
  permission_denied,
  protocol_error,
  server_busy,
  request_failed,
  internal_error,
};

struct RpcFailure {
  RpcFailureCode code;
  std::string message;

  bool operator==(const RpcFailure &) const = default;
};

enum class HealthState { healthy, degraded, unhealthy };
enum class QueueState { available, saturated };

struct HealthStatus {
  HealthState state;
  QueueState queue;
  unsigned protocol_version;

  bool operator==(const HealthStatus &) const = default;
};

// A privilege-free SOCK_SEQPACKET client. Every transport operation is bounded
// by rpc_timeout, while asynchronous operation polling is additionally bounded
// by operation_timeout. The peer UID defaults to root (UID 0).
class IHypervisorClient {
public:
  virtual ~IHypervisorClient() = default;
  [[nodiscard]] virtual bool
  register_container(const ContainerRegistration &) = 0;
  [[nodiscard]] virtual bool unregister_container(const ContainerId &) = 0;
  [[nodiscard]] virtual bool create_checkpoint(const ContainerId &) = 0;
  [[nodiscard]] virtual bool restore_or_restart(const ContainerId &) = 0;
};

class SchedulerClient final : public IHypervisorClient {
public:
  explicit SchedulerClient(
      std::string socket_path,
      std::chrono::milliseconds rpc_timeout = std::chrono::seconds(3),
      std::chrono::milliseconds operation_timeout = std::chrono::minutes(30),
      std::optional<uid_t> expected_server_uid = std::nullopt);
  ~SchedulerClient();
  SchedulerClient(const SchedulerClient &) = delete;
  SchedulerClient &operator=(const SchedulerClient &) = delete;
  SchedulerClient(SchedulerClient &&) noexcept;
  SchedulerClient &operator=(SchedulerClient &&) noexcept;

  [[nodiscard]] std::expected<void, RpcFailure> connect();
  [[nodiscard]] bool
  register_container(const ContainerRegistration &registration,
                     const RequestId &request_id);
  [[nodiscard]] bool
  register_container(const ContainerRegistration &registration) override;
  [[nodiscard]] bool unregister_container(const ContainerId &container_id,
                                          const RequestId &request_id);
  [[nodiscard]] bool
  unregister_container(const ContainerId &container_id) override;
  [[nodiscard]] bool
  create_checkpoint(const ContainerId &container_id) override;
  [[nodiscard]] bool
  restore_or_restart(const ContainerId &container_id) override;
  [[nodiscard]] std::expected<OperationId, RpcFailure>
  submit_create_checkpoint(const ContainerId &, const RequestId &);
  [[nodiscard]] std::expected<OperationId, RpcFailure>
  submit_restore_or_restart(const ContainerId &, const RequestId &);
  [[nodiscard]] std::expected<OperationStatus, RpcFailure>
  get_operation(const OperationId &operation_id);
  [[nodiscard]] std::expected<HealthStatus, RpcFailure> get_health();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace gravesentinel::ccm
