#include "tests/integration/fixture/counter_fixture.hpp"

#include <charconv>
#include <cstdint>
#include <limits>

namespace gravesentinel::ccm::test {
namespace {

constexpr std::size_t kMaximumWorkerThreads = 64;
constexpr std::uint64_t kMinimumIntervalMilliseconds = 10;
constexpr std::uint64_t kMaximumIntervalMilliseconds = 60'000;

[[nodiscard]] std::expected<std::uint64_t, std::string>
parse_unsigned(std::string_view value, std::string_view option_name) {
  std::uint64_t parsed{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return std::unexpected(std::string(option_name) +
                           " requires an unsigned integer");
  }
  return parsed;
}

} // namespace

std::expected<CounterOptions, std::string>
parse_counter_options(std::span<const std::string_view> arguments) {
  CounterOptions options;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--help") {
      options.show_help = true;
      continue;
    }

    if (argument != "--threads" && argument != "--interval-ms") {
      return std::unexpected("unknown argument: " + std::string(argument));
    }
    if (++index >= arguments.size()) {
      return std::unexpected("missing value for " + std::string(argument));
    }

    const auto parsed = parse_unsigned(arguments[index], argument);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }

    if (argument == "--threads") {
      if (*parsed == 0 || *parsed > kMaximumWorkerThreads) {
        return std::unexpected("--threads must be in the range 1..64");
      }
      options.worker_threads = static_cast<std::size_t>(*parsed);
      continue;
    }

    if (*parsed < kMinimumIntervalMilliseconds ||
        *parsed > kMaximumIntervalMilliseconds) {
      return std::unexpected("--interval-ms must be in the range 10..60000");
    }
    options.interval = std::chrono::milliseconds{*parsed};
  }

  return options;
}

std::string_view counter_fixture_usage() noexcept {
  return "counter_fixture [--threads 1..64] [--interval-ms 10..60000]";
}

} // namespace gravesentinel::ccm::test
