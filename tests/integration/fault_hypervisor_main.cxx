#include "tests/integration/fault_injecting_file_operations.hxx"

#include "src/hypervisor/application.hxx"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kFaultControlOption = "--fault-control";

[[nodiscard]] int invalid_arguments(std::string_view message) {
  std::cerr << "fault_hypervisor: " << message << '\n';
  return 2;
}

} // namespace

int main(const int argc, char **const argv) {
  std::optional<std::filesystem::path> control_directory;
  int forwarded_argc = 1;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument != kFaultControlOption) {
      argv[forwarded_argc] = argv[index];
      ++forwarded_argc;
      continue;
    }
    if (control_directory.has_value()) {
      return invalid_arguments("--fault-control may be specified only once");
    }
    if (index + 1 >= argc || std::string_view{argv[index + 1]}.empty()) {
      return invalid_arguments("--fault-control requires a directory");
    }
    control_directory = std::filesystem::path{argv[index + 1]};
    ++index;
  }

  if (!control_directory.has_value()) {
    return invalid_arguments("--fault-control DIR is required");
  }
  argv[forwarded_argc] = nullptr;

  try {
    gravesentinel::hypervisor::ApplicationDependencies dependencies{
        .storage_file_operations = std::make_shared<
            gravesentinel::integration::FaultInjectingFileOperations>(
            *control_directory),
    };
    return gravesentinel::hypervisor::run_hypervisor(forwarded_argc, argv,
                                                     std::move(dependencies));
  } catch (const std::exception &error) {
    std::cerr << "fault_hypervisor: cannot initialize fault injection: "
              << error.what() << '\n';
    return 2;
  }
}
