#include "src/protocol/types.hxx"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

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

TEST(CpuSetTest, CanonicalizesAndOwnsLogicalCpuIds) {
  const auto cpus = CpuSet::create({4U, 1U, 3U});

  ASSERT_TRUE(cpus);
  EXPECT_EQ(cpus->values(), (std::vector<std::uint32_t>{1U, 3U, 4U}));
  EXPECT_TRUE(cpus->contains(3U));
  EXPECT_FALSE(cpus->contains(2U));
}

TEST(CpuSetTest, RejectsEmptyDuplicateAndOutOfProfileSets) {
  for (auto values :
       {std::vector<std::uint32_t>{}, std::vector<std::uint32_t>{1U, 1U},
        std::vector<std::uint32_t>{0U, 8U}}) {
    const auto result = CpuSet::create(std::move(values));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ValidationErrorCode::invalid_cpu_set);
  }
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
