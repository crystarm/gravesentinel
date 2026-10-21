#include "tests/integration/fixture/counter_fixture.hxx"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string_view>

namespace gravesentinel::ccm::test {
namespace {

TEST(CounterOptionsTest, UsesMultithreadedDefaults) {
  const auto options = parse_counter_options({});

  ASSERT_TRUE(options);
  EXPECT_EQ(options->worker_threads, 2U);
  EXPECT_EQ(options->interval, std::chrono::milliseconds{250});
  EXPECT_FALSE(options->show_help);
}

TEST(CounterOptionsTest, ParsesExplicitValues) {
  constexpr std::array<std::string_view, 4> arguments{"--threads", "4",
                                                      "--interval-ms", "500"};

  const auto options = parse_counter_options(arguments);

  ASSERT_TRUE(options);
  EXPECT_EQ(options->worker_threads, 4U);
  EXPECT_EQ(options->interval, std::chrono::milliseconds{500});
}

TEST(CounterOptionsTest, RejectsUnknownMissingAndOutOfRangeValues) {
  constexpr std::array<std::string_view, 1> unknown{"--unknown"};
  constexpr std::array<std::string_view, 1> missing{"--threads"};
  constexpr std::array<std::string_view, 2> zero_threads{"--threads", "0"};
  constexpr std::array<std::string_view, 2> too_fast{"--interval-ms", "9"};
  constexpr std::array<std::string_view, 2> malformed{"--threads", "many"};

  EXPECT_FALSE(parse_counter_options(unknown));
  EXPECT_FALSE(parse_counter_options(missing));
  EXPECT_FALSE(parse_counter_options(zero_threads));
  EXPECT_FALSE(parse_counter_options(too_fast));
  EXPECT_FALSE(parse_counter_options(malformed));
}

TEST(CounterOptionsTest, RecognizesHelp) {
  constexpr std::array<std::string_view, 1> arguments{"--help"};

  const auto options = parse_counter_options(arguments);

  ASSERT_TRUE(options);
  EXPECT_TRUE(options->show_help);
}

} // namespace
} // namespace gravesentinel::ccm::test
