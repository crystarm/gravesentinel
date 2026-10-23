#pragma once
#include <csignal>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
namespace gravesentinel::ipc {
using Handler = std::function<nlohmann::json(const nlohmann::json &)>;
struct ServerConfig {
  std::string socket_path;
  std::string lock_path;
  unsigned mode{0660};
  bool allow_same_uid{true};
};
int serve(const ServerConfig &config, const Handler &handler,
          volatile std::sig_atomic_t *stop_requested);
} // namespace gravesentinel::ipc
