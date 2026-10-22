#pragma once

#include "src/core/contracts.hxx"

namespace gravesentinel::ccm {

class PosixProcessRunner final : public IProcessRunner {
public:
  [[nodiscard]] CheckpointResult<ProcessResult>
  run(const ProcessRequest &request) override;
};

} // namespace gravesentinel::ccm
