#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <sys/types.h>

namespace gravesentinel::ipc {
using Handler = std::function<nlohmann::json(const nlohmann::json &)>;
using StopPredicate = std::function<bool()>;

struct ServerConfig;

class ServerLock final {
public:
  [[nodiscard]] static std::optional<ServerLock> acquire(std::string path);

  ~ServerLock();
  ServerLock(const ServerLock &) = delete;
  ServerLock &operator=(const ServerLock &) = delete;
  ServerLock(ServerLock &&other) noexcept;
  ServerLock &operator=(ServerLock &&other) noexcept;

private:
  ServerLock(std::string path, int fd) noexcept;

  std::string path_;
  int fd_{-1};

  friend int serve(const ServerConfig &, const ServerLock &, const Handler &,
                   const StopPredicate &);
};

struct ServerConfig {
  std::string socket_path;
  std::string lock_path;
  unsigned mode{0660};

  // When unset, the peer must match the server's effective UID.
  std::optional<uid_t> allowed_uid;
  std::optional<gid_t> socket_gid;
  std::chrono::milliseconds peer_io_timeout{std::chrono::seconds(3)};
  std::size_t worker_count{4};
  std::size_t peer_queue_capacity{32};
  // Includes Handshake; serve() clamps this to [2, 1024].
  std::size_t max_requests_per_connection{64};
};

int serve(const ServerConfig &config, const ServerLock &lock,
          const Handler &handler, const StopPredicate &stop_requested);
} // namespace gravesentinel::ipc
