#include "src/hypervisor/application.hxx"

int main(const int argc, char **const argv) {
  return gravesentinel::hypervisor::run_hypervisor(argc, argv);
}
