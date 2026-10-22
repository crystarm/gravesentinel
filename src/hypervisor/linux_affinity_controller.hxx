#pragma once

#include "src/core/contracts.hxx"

#include <cstdint>
#include <memory>

namespace gravesentinel::ccm {

class LinuxAffinityController final : public IAffinityController {
public:
  [[nodiscard]] CheckpointResult<std::unique_ptr<IThreadAffinityBinding>>
  bind_current_thread(std::uint32_t logical_cpu) override;
  [[nodiscard]] CheckpointResult<void>
  validate_cpu_available(std::uint32_t logical_cpu) const override;
};

} // namespace gravesentinel::ccm
