#include "src/ipc/codec.hxx"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace gravesentinel::ipc {
TEST(IpcCodecTest, AcceptsVersionedWellFormedRequest) {
  const auto raw = nlohmann::json{{"version", 1U},
                                  {"method", "GetHealth"},
                                  {"request_id", "health-1"},
                                  {"params", nlohmann::json::object()}}
                       .dump();
  const auto decoded = decode(raw);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->at("method"), "GetHealth");
}
TEST(IpcCodecTest, RejectsUnknownVersionAndMalformedRequests) {
  EXPECT_FALSE(decode(
      R"({"version":2,"method":"GetHealth","request_id":"x","params":{}})"));
  EXPECT_FALSE(decode(
      R"({"version":1,"method":"GetHealth","request_id":"x","params":{"extra":1}})"));
  EXPECT_FALSE(decode("not json"));
}
TEST(IpcCodecTest, RejectsOversizedPacketsAndUnknownMethods) {
  EXPECT_FALSE(decode(std::string(max_message_bytes + 1, 'x')));
  EXPECT_FALSE(decode(
      R"({"version":1,"method":"Shutdown","request_id":"x","params":{}})"));
}
TEST(IpcCodecTest, EnforcesIdentifierBoundsAndCharacters) {
  EXPECT_TRUE(valid_identifier("request-123.a"));
  EXPECT_FALSE(valid_identifier(""));
  EXPECT_FALSE(valid_identifier("contains space"));
}
} // namespace gravesentinel::ipc
