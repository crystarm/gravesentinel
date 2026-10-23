#pragma once

#include "src/core/contracts.hxx"
#include "src/ipc/operation_store.hxx"

#include <expected>
#include <nlohmann/json_fwd.hpp>

namespace gravesentinel::hypervisor {

enum class RegistrationDecodeError { invalid_schema };

[[nodiscard]] std::expected<ccm::ContainerRegistration, RegistrationDecodeError>
decode_container_registration(const nlohmann::json &parameters);

[[nodiscard]] ipc::OperationStore::Executor
make_operation_executor(ccm::ICheckpointService &service);

class RpcHandler final {
public:
  RpcHandler(ccm::ICheckpointService &service,
             ipc::OperationStore &operation_store) noexcept;

  [[nodiscard]] nlohmann::json operator()(const nlohmann::json &request) const;

private:
  ccm::ICheckpointService &service_;
  ipc::OperationStore &operation_store_;
};

} // namespace gravesentinel::hypervisor
