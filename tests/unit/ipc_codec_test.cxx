#include "src/ipc/codec.hxx"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace gravesentinel::ipc {
namespace {

[[nodiscard]] nlohmann::json request(std::string method,
                                     nlohmann::json parameters) {
  return {{"version", protocol_version},
          {"method", std::move(method)},
          {"request_id", "request-1"},
          {"params", std::move(parameters)}};
}

[[nodiscard]] nlohmann::json full_registration() {
  return request("Register",
                 {{"container_id", "container-1"},
                  {"runtime_container", "runtime-1"},
                  {"checkpoint",
                   {{"interval_seconds", 60},
                    {"keep_last", 3U},
                    {"container_cpu", 0U},
                    {"checkpoint_cpu", 1U}}},
                  {"cold_start",
                   {{"image_reference", "registry.example/worker:latest"},
                    {"argv", {"worker", "", "--threads", "2"}},
                    {"environment",
                     {{{"name", "WORKER_MODE"}, {"value", "steady"}},
                      {{"name", "_EMPTY_VALUE"}, {"value", ""}}}},
                    {"mounts",
                     {{{"source", "/srv/input"},
                       {"target", "/input"},
                       {"read_only", true}}}},
                    {"runtime_options", {"--log-level=warning"}}}},
                  {"runtime_fingerprint",
                   {{"runtime_name", "podman"},
                    {"runtime_version", "5.4.2"},
                    {"runtime_build_origin", "Debian 13"},
                    {"architecture", "amd64"},
                    {"kernel_release", "6.12.0"},
                    {"criu_version", "4.1.1"},
                    {"oci_runtime_name", "crun"},
                    {"oci_runtime_version", "1.21"},
                    {"oci_runtime_commit", "abcdef"},
                    {"oci_runtime_features", "+CRIU +SECCOMP"},
                    {"cgroup_version", "v2"},
                    {"cgroup_manager", "systemd"},
                    {"cgroup_controllers", "cpu memory pids"},
                    {"privilege_mode", "rootful"},
                    {"user_namespace_mode", "host"},
                    {"capability_profile", "default"},
                    {"selinux_mode", "disabled"},
                    {"apparmor_profile", "host-default"},
                    {"seccomp_profile", "default"},
                    {"command_profile", "podman-criu-rootful-v1"}}}});
}

TEST(IpcCodecTest, AcceptsNoArgumentRequestOnlyWithObjectParams) {
  const auto message = request("GetHealth", nlohmann::json::object());
  const auto encoded = encode_request(message);
  ASSERT_TRUE(encoded) << encoded.error().message;

  const auto decoded = decode_request(*encoded);
  ASSERT_TRUE(decoded) << decoded.error().message;
  EXPECT_EQ(decoded->at("method"), "GetHealth");
  EXPECT_TRUE(decoded->at("params").is_object());
  EXPECT_TRUE(decoded->at("params").empty());

  auto null_parameters = message;
  null_parameters["params"] = nullptr;
  EXPECT_FALSE(encode_request(null_parameters));
  EXPECT_FALSE(decode_request(null_parameters.dump()));
}

TEST(IpcCodecTest, RequestAndResponseSchemasAreDistinctAndExact) {
  auto extra_request = request("GetHealth", nlohmann::json::object());
  extra_request["unknown"] = true;
  EXPECT_FALSE(decode_request(extra_request.dump()));

  const nlohmann::json success{{"version", protocol_version},
                               {"request_id", "request-1"},
                               {"ok", true},
                               {"result", nlohmann::json::object()},
                               {"close_connection", false}};
  ASSERT_TRUE(decode_response(success.dump()));
  EXPECT_FALSE(decode_request(success.dump()));
  EXPECT_FALSE(
      decode_response(request("GetHealth", nlohmann::json::object()).dump()));

  auto extra_response = success;
  extra_response["diagnostic"] = "must not be accepted";
  EXPECT_FALSE(decode_response(extra_response.dump()));

  auto missing_close_signal = success;
  missing_close_signal.erase("close_connection");
  EXPECT_FALSE(decode_response(missing_close_signal.dump()));
  auto closing_response = success;
  closing_response["close_connection"] = true;
  EXPECT_TRUE(decode_response(closing_response.dump()));
}

TEST(IpcCodecTest, RejectsUnknownVersionMethodAndMalformedPackets) {
  auto wrong_version = request("GetHealth", nlohmann::json::object());
  wrong_version["version"] = protocol_version + 1U;
  EXPECT_FALSE(decode_request(wrong_version.dump()));
  EXPECT_FALSE(
      decode_request(request("Shutdown", nlohmann::json::object()).dump()));
  EXPECT_FALSE(decode_request("not json"));
  EXPECT_FALSE(decode_request(std::string(max_message_bytes + 1, 'x')));
}

TEST(IpcCodecTest, ValidatesSuccessAndSafeErrorResponsesSeparately) {
  const nlohmann::json failure{{"version", protocol_version},
                               {"request_id", "request-1"},
                               {"ok", false},
                               {"error_code", "request_failed"},
                               {"close_connection", false}};
  ASSERT_TRUE(encode_response(failure));
  EXPECT_TRUE(decode_response(failure.dump()));

  auto unknown_code = failure;
  unknown_code["error_code"] = "database_stack_trace";
  EXPECT_FALSE(decode_response(unknown_code.dump()));

  auto mixed_shape = failure;
  mixed_shape["result"] = nlohmann::json::object();
  EXPECT_FALSE(decode_response(mixed_shape.dump()));
}

TEST(IpcCodecTest, AcceptsCompleteContainerRegistrationSchema) {
  const auto registration = full_registration();
  const auto encoded = encode_request(registration);
  ASSERT_TRUE(encoded) << encoded.error().message;

  const auto decoded = decode_request(*encoded);
  ASSERT_TRUE(decoded) << decoded.error().message;
  const auto &parameters = decoded->at("params");
  EXPECT_EQ(parameters.at("cold_start").at("environment").size(), 2U);
  EXPECT_EQ(parameters.at("cold_start").at("mounts").size(), 1U);
  EXPECT_EQ(parameters.at("runtime_fingerprint").at("runtime_name"), "podman");
}

TEST(IpcCodecTest, RejectsInexactOrUnboundedRegistrationStructures) {
  auto registration = full_registration();
  registration["params"]["checkpoint"]["extra"] = true;
  EXPECT_FALSE(decode_request(registration.dump()));

  registration = full_registration();
  registration["params"]["cold_start"]["environment"][0]["secret"] = "unknown";
  EXPECT_FALSE(decode_request(registration.dump()));

  registration = full_registration();
  registration["params"]["cold_start"]["argv"] = nlohmann::json::array();
  for (int index = 0; index < 257; ++index)
    registration["params"]["cold_start"]["argv"].push_back("argument");
  EXPECT_FALSE(decode_request(registration.dump()));

  registration = full_registration();
  registration["params"]["runtime_fingerprint"]["runtime_name"] = "";
  EXPECT_FALSE(decode_request(registration.dump()));
}

TEST(IpcCodecTest, AcceptsRegistrationLargerThanLegacyPacketLimit) {
  auto registration = full_registration();
  auto options = nlohmann::json::array();
  for (int index = 0; index < 16; ++index)
    options.push_back(std::string(4050, 'x'));
  registration["params"]["cold_start"]["runtime_options"] = std::move(options);

  ASSERT_GT(registration.dump().size(), 64U * 1024U);
  const auto encoded = encode_request(registration);
  ASSERT_TRUE(encoded) << encoded.error().message;
  EXPECT_LE(encoded->size(), max_message_bytes);
}

TEST(IpcCodecTest, AcceptsNullOptionalRegistrationMembers) {
  auto registration = full_registration();
  registration["params"]["cold_start"] = nullptr;
  registration["params"]["runtime_fingerprint"] = nullptr;
  EXPECT_TRUE(decode_request(registration.dump()));
}

TEST(IpcCodecTest, EnforcesIdentifierBoundsAndCharacters) {
  EXPECT_TRUE(valid_identifier("request-123.a"));
  EXPECT_FALSE(valid_identifier(""));
  EXPECT_FALSE(valid_identifier("contains space"));
  EXPECT_FALSE(valid_identifier(std::string("\xc3\xa9", 2)));
  EXPECT_FALSE(valid_identifier(std::string(129, 'x')));
}
} // namespace
} // namespace gravesentinel::ipc
