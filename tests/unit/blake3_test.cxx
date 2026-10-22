#include "src/core/blake3_hasher.hxx"
#include "src/crypto/blake3.hxx"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <type_traits>

namespace gravesentinel::ccm {
namespace {

static_assert(std::is_base_of_v<IHasher, Blake3Hasher>);
static_assert(!std::is_abstract_v<Blake3Hasher>);
static_assert(std::is_final_v<Blake3Hasher>);
static_assert(noexcept(blake3_hash(std::span<const std::byte>{})));
static_assert(noexcept(Blake3Hasher{}.hash(std::span<const std::byte>{})));

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes) {
  constexpr std::array hex_digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

  std::string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<std::size_t>(byte);
    result.push_back(hex_digits[value >> 4U]);
    result.push_back(hex_digits[value & 0x0FU]);
  }
  return result;
}

[[nodiscard]] constexpr std::array<std::byte, 1024> make_1024_byte_input() {
  std::array<std::byte, 1024> input{};
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<std::byte>(index % std::size_t{251});
  }
  return input;
}

TEST(Blake3HashTest, MatchesOfficialEmptyInputVector) {
  const auto hash = blake3_hash(std::span<const std::byte>{});

  EXPECT_EQ(to_hex(hash.bytes()),
            "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST(Blake3HashTest, MatchesOfficialThreeByteVector) {
  constexpr std::array input{
      std::byte{0x00},
      std::byte{0x01},
      std::byte{0x02},
  };

  const auto hash = blake3_hash(input);

  EXPECT_EQ(to_hex(hash.bytes()),
            "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f");
}

TEST(Blake3HashTest, MatchesOfficial1024ByteVector) {
  constexpr auto input = make_1024_byte_input();

  const auto hash = blake3_hash(input);

  EXPECT_EQ(to_hex(hash.bytes()),
            "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
}

TEST(Blake3HasherTest, IsDeterministicAndDelegatesToOneShotHash) {
  constexpr auto input = make_1024_byte_input();
  const Blake3Hasher hasher;
  const IHasher &interface = hasher;

  const auto first = interface.hash(input);
  const auto second = interface.hash(input);

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_EQ(*first, blake3_hash(input));
}

} // namespace
} // namespace gravesentinel::ccm
