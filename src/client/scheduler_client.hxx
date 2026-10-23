#pragma once

#include "src/protocol/types.hxx"
#include <chrono>
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace gravesentinel::ccm {

struct RpcFailure {
  std::string message;
};
struct RpcReply {
  nlohmann::json payload;
};

// A deliberately small, privilege-free SOCK_SEQPACKET client. Calls are bounded
// by the supplied deadline; a reconnect never changes a caller-supplied ID.
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
      std::chrono::milliseconds timeout = std::chrono::seconds(3));
  ~SchedulerClient();
  SchedulerClient(const SchedulerClient &) = delete;
  SchedulerClient &operator=(const SchedulerClient &) = delete;
  SchedulerClient(SchedulerClient &&) noexcept;
  SchedulerClient &operator=(SchedulerClient &&) noexcept;

  [[nodiscard]] std::expected<void, RpcFailure> connect();
  [[nodiscard]] std::expected<RpcReply, RpcFailure>
  call(std::string_view method, std::string request_id,
       nlohmann::json params = {});
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
  [[nodiscard]] std::expected<std::string, RpcFailure>
  submit_create_checkpoint(const ContainerId &, const RequestId &);
  [[nodiscard]] std::expected<std::string, RpcFailure>
  submit_restore_or_restart(const ContainerId &, const RequestId &);
  [[nodiscard]] std::expected<RpcReply, RpcFailure>
  get_operation(std::string_view operation_id);
  [[nodiscard]] std::expected<RpcReply, RpcFailure> get_health();

private:
  struct Impl;
  Impl *impl_;
};
} // namespace gravesentinel::ccm
