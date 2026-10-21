#pragma once

#include "src/protocol/types.hpp"

namespace gravesentinel::ccm {

class IHypervisorClient {
public:
  virtual ~IHypervisorClient() = default;

  [[nodiscard]] virtual bool
  register_container(const ContainerRegistration &registration) = 0;
  [[nodiscard]] virtual bool
  unregister_container(const ContainerId &container_id) = 0;
  [[nodiscard]] virtual bool
  create_checkpoint(const ContainerId &container_id) = 0;
  [[nodiscard]] virtual bool
  restore_or_restart(const ContainerId &container_id) = 0;
};

} // namespace gravesentinel::ccm
