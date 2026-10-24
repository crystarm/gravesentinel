#include "tests/integration/fixture/counter_fixture.hxx"

#include <charconv>
#include <cstdint>
#include <limits>

namespace gravesentinel::ccm::test {
namespace {

constexpr std::size_t kMaximumWorkerThreads = 64;
constexpr std::uint64_t kMinimumIntervalMilliseconds = 10;
constexpr std::uint64_t kMaximumIntervalMilliseconds = 60'000;
constexpr std::size_t kMaximumStateFileBytes = 4'095;
constexpr std::size_t kMaximumInstanceTagBytes = 64;

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

[[nodiscard]] std::expected<void, std::string>
validate_state_file(std::string_view value) {
  if (value.empty()) {
    return std::unexpected("--state-file must not be empty");
  }
  if (value.size() > kMaximumStateFileBytes) {
    return std::unexpected("--state-file must not exceed 4095 bytes");
  }
  if (!value.starts_with('/')) {
    return std::unexpected("--state-file must be an absolute path");
  }
  if (value == "/" || value.ends_with('/') || value.find("//") != value.npos) {
    return std::unexpected("--state-file must name a normalized file path");
  }
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte == 0x7FU) {
      return std::unexpected(
          "--state-file must not contain control characters");
    }
  }

  std::size_t component_start = 1;
  while (component_start < value.size()) {
    const auto separator = value.find('/', component_start);
    const auto component = value.substr(
        component_start,
        separator == value.npos ? value.npos : separator - component_start);
    if (component == "." || component == "..") {
      return std::unexpected("--state-file must name a normalized file path");
    }
    if (separator == value.npos) {
      break;
    }
    component_start = separator + 1;
  }
  return {};
}

[[nodiscard]] constexpr bool is_instance_tag_character(char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '-' ||
         character == '_' || character == '.' || character == ':';
}

[[nodiscard]] std::expected<void, std::string>
validate_instance_tag(std::string_view value) {
  if (value.empty()) {
    return std::unexpected("--instance-tag must not be empty");
  }
  if (value.size() > kMaximumInstanceTagBytes) {
    return std::unexpected("--instance-tag must not exceed 64 bytes");
  }
  for (const char character : value) {
    if (!is_instance_tag_character(character)) {
      return std::unexpected(
          "--instance-tag may contain only ASCII letters, digits, '.', '_', "
          "'-', and ':'");
    }
  }
  return {};
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

    if (argument != "--threads" && argument != "--interval-ms" &&
        argument != "--state-file" && argument != "--instance-tag") {
      return std::unexpected("unknown argument: " + std::string(argument));
    }
    if (++index >= arguments.size()) {
      return std::unexpected("missing value for " + std::string(argument));
    }

    const auto value = arguments[index];
    if (argument == "--state-file") {
      const auto validation = validate_state_file(value);
      if (!validation) {
        return std::unexpected(validation.error());
      }
      options.state_file = value;
      continue;
    }
    if (argument == "--instance-tag") {
      const auto validation = validate_instance_tag(value);
      if (!validation) {
        return std::unexpected(validation.error());
      }
      options.instance_tag = value;
      continue;
    }

    const auto parsed = parse_unsigned(value, argument);
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
  return "counter_fixture [--threads 1..64] [--interval-ms 10..60000] "
         "[--state-file ABSOLUTE_PATH] [--instance-tag TAG]";
}

} // namespace gravesentinel::ccm::test
