#include <gravesentinel/scheduler_client.hxx>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace {

[[nodiscard]] const char *
health_name(const gravesentinel::ccm::HealthState state) noexcept {
  switch (state) {
  case gravesentinel::ccm::HealthState::healthy:
    return "healthy";
  case gravesentinel::ccm::HealthState::degraded:
    return "degraded";
  case gravesentinel::ccm::HealthState::unhealthy:
    return "unhealthy";
  }
  return "unknown";
}

[[nodiscard]] const char *
queue_name(const gravesentinel::ccm::QueueState state) noexcept {
  switch (state) {
  case gravesentinel::ccm::QueueState::available:
    return "available";
  case gravesentinel::ccm::QueueState::saturated:
    return "saturated";
  }
  return "unknown";
}

[[nodiscard]] std::optional<uid_t>
parse_uid(const std::string_view text) noexcept {
  std::uintmax_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value >= static_cast<std::uintmax_t>(std::numeric_limits<uid_t>::max())) {
    return std::nullopt;
  }
  return static_cast<uid_t>(value);
}

} // namespace

int main(const int argc, char **const argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: scheduler_client_consumer SOCKET_PATH "
                 "[EXPECTED_SERVER_UID]\n";
    return 2;
  }
  std::optional<uid_t> expected_uid;
  if (argc == 3) {
    expected_uid = parse_uid(argv[2]);
    if (!expected_uid) {
      std::cerr << "invalid expected server UID\n";
      return 2;
    }
  }

  gravesentinel::ccm::SchedulerClient client{
      std::string{argv[1]}, std::chrono::seconds{3}, std::chrono::minutes{30},
      expected_uid};
  const auto connected = client.connect();
  if (!connected) {
    std::cerr << "connect failed: " << connected.error().message << '\n';
    return 3;
  }

  const auto health = client.get_health();
  if (!health) {
    std::cerr << "GetHealth failed: " << health.error().message << '\n';
    return 4;
  }

  std::cout << "status=" << health_name(health->state)
            << " queue=" << queue_name(health->queue)
            << " protocol_version=" << health->protocol_version << '\n';
  return 0;
}
