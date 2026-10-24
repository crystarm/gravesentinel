#include "tests/integration/fixture/counter_fixture.hxx"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <string_view>

namespace gravesentinel::ccm::test {
namespace {

TEST(CounterOptionsTest, UsesMultithreadedDefaults) {
  const auto options = parse_counter_options({});

  ASSERT_TRUE(options);
  EXPECT_EQ(options->worker_threads, 2U);
  EXPECT_EQ(options->interval, std::chrono::milliseconds{250});
  EXPECT_EQ(options->state_file,
            "/dev/shm/gravesentinel-counter-fixture.state");
  EXPECT_EQ(options->instance_tag, "default");
  EXPECT_FALSE(options->show_help);
}

TEST(CounterOptionsTest, ParsesExplicitValues) {
  constexpr std::array<std::string_view, 8> arguments{
      "--threads",    "4",          "--interval-ms",  "500",
      "--state-file", "/tmp/state", "--instance-tag", "restore-01"};

  const auto options = parse_counter_options(arguments);

  ASSERT_TRUE(options);
  EXPECT_EQ(options->worker_threads, 4U);
  EXPECT_EQ(options->interval, std::chrono::milliseconds{500});
  EXPECT_EQ(options->state_file, "/tmp/state");
  EXPECT_EQ(options->instance_tag, "restore-01");
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

TEST(CounterOptionsTest, RejectsInvalidStateFileValues) {
  constexpr std::array<std::string_view, 2> empty{"--state-file", ""};
  constexpr std::array<std::string_view, 2> relative{"--state-file",
                                                     "tmp/state"};
  constexpr std::array<std::string_view, 2> root{"--state-file", "/"};
  constexpr std::array<std::string_view, 2> trailing_slash{"--state-file",
                                                           "/tmp/state/"};
  constexpr std::array<std::string_view, 2> parent_component{"--state-file",
                                                             "/tmp/../state"};
  const std::string oversized_path = "/" + std::string(4'095, 'x');
  const std::array<std::string_view, 2> oversized{"--state-file",
                                                  oversized_path};
  const std::string nul_path{"/tmp/state\0suffix", 17};
  const std::array<std::string_view, 2> embedded_nul{"--state-file", nul_path};

  EXPECT_FALSE(parse_counter_options(empty));
  EXPECT_FALSE(parse_counter_options(relative));
  EXPECT_FALSE(parse_counter_options(root));
  EXPECT_FALSE(parse_counter_options(trailing_slash));
  EXPECT_FALSE(parse_counter_options(parent_component));
  EXPECT_FALSE(parse_counter_options(oversized));
  EXPECT_FALSE(parse_counter_options(embedded_nul));
}

TEST(CounterOptionsTest, RejectsInvalidInstanceTagValues) {
  constexpr std::array<std::string_view, 2> empty{"--instance-tag", ""};
  constexpr std::array<std::string_view, 2> whitespace{"--instance-tag",
                                                       "restore one"};
  constexpr std::array<std::string_view, 2> separator{"--instance-tag",
                                                      "restore=one"};
  const std::string oversized_tag(65, 'x');
  const std::array<std::string_view, 2> oversized{"--instance-tag",
                                                  oversized_tag};

  EXPECT_FALSE(parse_counter_options(empty));
  EXPECT_FALSE(parse_counter_options(whitespace));
  EXPECT_FALSE(parse_counter_options(separator));
  EXPECT_FALSE(parse_counter_options(oversized));
}

TEST(CounterOptionsTest, RecognizesHelp) {
  constexpr std::array<std::string_view, 1> arguments{"--help"};

  const auto options = parse_counter_options(arguments);

  ASSERT_TRUE(options);
  EXPECT_TRUE(options->show_help);
}

} // namespace
} // namespace gravesentinel::ccm::test
