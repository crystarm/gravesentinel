#pragma once

#include <memory>

namespace gravesentinel::ccm {
class IFileOperations;
}

namespace gravesentinel::hypervisor {

struct ApplicationDependencies {
  std::shared_ptr<ccm::IFileOperations> storage_file_operations;
};

[[nodiscard]] int run_hypervisor(int argc, char **argv,
                                 ApplicationDependencies dependencies = {});

} // namespace gravesentinel::hypervisor
