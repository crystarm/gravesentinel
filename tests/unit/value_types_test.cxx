#include "src/protocol/types.hxx"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>

namespace gravesentinel::ccm {
namespace {

TEST(IdentifierTest, AcceptsNonemptyValuesForEveryIdentifierType) {
  EXPECT_TRUE(ContainerId::create("container-1"));
  EXPECT_TRUE(CheckpointId::create("checkpoint-1"));
  EXPECT_TRUE(RequestId::create("request-1"));
  EXPECT_TRUE(OperationId::create("operation-1"));
  EXPECT_TRUE(RuntimeContainerRef::create("runtime-container-1"));
}

TEST(IdentifierTest, RejectsEmptyValuesForEveryIdentifierType) {
  EXPECT_EQ(ContainerId::create("").error().code,
            ValidationErrorCode::empty_value);
  EXPECT_EQ(CheckpointId::create("").error().code,
            ValidationErrorCode::empty_value);
  EXPECT_EQ(RequestId::create("").error().code,
            ValidationErrorCode::empty_value);
  EXPECT_EQ(OperationId::create("").error().code,
            ValidationErrorCode::empty_value);
  EXPECT_EQ(RuntimeContainerRef::create("").error().code,
            ValidationErrorCode::empty_value);
}

TEST(IdentifierTest, RejectsEmbeddedNul) {
  const std::string value_with_nul{"container\0suffix", 16};

  const auto result = ContainerId::create(value_with_nul);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ValidationErrorCode::embedded_nul);
  EXPECT_EQ(result.error().field, "container_id");
}

TEST(ChunkHashTest, HoldsExactlyOneBlake3Digest) {
  ChunkHash::Bytes bytes{};
  bytes.front() = std::byte{0xAB};
  bytes.back() = std::byte{0xCD};

  const ChunkHash hash(bytes);

  static_assert(ChunkHash::digest_size == 32);
  EXPECT_EQ(hash.bytes(), bytes);
}

} // namespace
} // namespace gravesentinel::ccm
