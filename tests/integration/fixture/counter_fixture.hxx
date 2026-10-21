#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace gravesentinel::ccm::test {

struct CounterOptions {
  std::size_t worker_threads{2};
  std::chrono::milliseconds interval{250};
  bool show_help{false};

  bool operator==(const CounterOptions &) const = default;
};

[[nodiscard]] std::expected<CounterOptions, std::string>
parse_counter_options(std::span<const std::string_view> arguments);

[[nodiscard]] std::string_view counter_fixture_usage() noexcept;

} // namespace gravesentinel::ccm::test
