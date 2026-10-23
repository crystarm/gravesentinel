#include "src/ipc/server.hxx"
#include "src/ipc/codec.hxx"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace gravesentinel::ipc {
namespace {
bool write_packet(int fd, const nlohmann::json &j) {
  auto encoded = encode(j);
  if (!encoded)
    return false;
  return ::send(fd, encoded->data(), encoded->size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(encoded->size());
}
} // namespace
int serve(const ServerConfig &config, const Handler &handler,
          volatile std::sig_atomic_t *stop_requested) {
  if (config.socket_path.empty() ||
      config.socket_path.size() >= sizeof(sockaddr_un{}.sun_path))
    return 2;
  const int lock_fd =
      ::open(config.lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    if (lock_fd >= 0)
      ::close(lock_fd);
    return 3;
  }
  struct stat st{};
  if (::lstat(config.socket_path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      ::close(lock_fd);
      return 4;
    }
    const int probe = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un old{};
    old.sun_family = AF_UNIX;
    std::memcpy(old.sun_path, config.socket_path.c_str(),
                config.socket_path.size() + 1);
    const bool live =
        probe >= 0 &&
        ::connect(probe, reinterpret_cast<sockaddr *>(&old), sizeof(old)) == 0;
    if (probe >= 0)
      ::close(probe);
    if (live || ::unlink(config.socket_path.c_str()) != 0) {
      ::close(lock_fd);
      return 5;
    }
  } else if (errno != ENOENT) {
    ::close(lock_fd);
    return 6;
  }
  const int server = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (server < 0) {
    ::close(lock_fd);
    return 7;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, config.socket_path.c_str(),
              config.socket_path.size() + 1);
  if (::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
          0 ||
      ::chmod(config.socket_path.c_str(), static_cast<mode_t>(config.mode)) !=
          0 ||
      ::listen(server, 32) != 0) {
    ::close(server);
    ::unlink(config.socket_path.c_str());
    ::close(lock_fd);
    return 8;
  }
  while (!*stop_requested) {
    pollfd pfd{server, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0)
      continue;
    const int peer = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (peer < 0)
      continue;
    ucred credentials{};
    socklen_t credential_size = sizeof(credentials);
    const bool authorized =
        !config.allow_same_uid ||
        (::getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials,
                      &credential_size) == 0 &&
         credentials.uid == ::geteuid());
    if (!authorized) {
      (void)write_packet(peer, {{"version", protocol_version},
                                {"method", "Response"},
                                {"request_id", "response"},
                                {"params", nlohmann::json::object()},
                                {"ok", false},
                                {"error", "permission denied"}});
      ::close(peer);
      continue;
    }
    char buffer[max_message_bytes + 1];
    const auto n = ::recv(peer, buffer, sizeof(buffer), 0);
    if (n > 0 && static_cast<std::size_t>(n) <= max_message_bytes) {
      auto request =
          decode(std::string_view(buffer, static_cast<std::size_t>(n)));
      if (!request)
        (void)write_packet(peer, {{"version", protocol_version},
                                  {"method", "Response"},
                                  {"request_id", "response"},
                                  {"params", nlohmann::json::object()},
                                  {"ok", false},
                                  {"error", request.error().message}});
      else {
        try {
          auto response = handler(*request);
          if (response.value("ok", true))
            response["ok"] = true;
          response["version"] = protocol_version;
          response["method"] = "Response";
          response["request_id"] = "response";
          response["params"] = nlohmann::json::object();
          (void)write_packet(peer, response);
        } catch (const std::exception &) {
          (void)write_packet(peer, {{"version", protocol_version},
                                    {"method", "Response"},
                                    {"request_id", "response"},
                                    {"params", nlohmann::json::object()},
                                    {"ok", false},
                                    {"error", "internal error"}});
        }
      }
    }
    ::close(peer);
  }
  ::close(server);
  (void)::unlink(config.socket_path.c_str());
  ::close(lock_fd);
  return 0;
}
} // namespace gravesentinel::ipc
