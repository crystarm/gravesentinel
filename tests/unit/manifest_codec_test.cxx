#include "src/manifest/codec.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

constexpr std::uint32_t kRegularFileMode = 0100644U;
constexpr std::string_view kFirstHash =
    "abababababababababababababababababababababababababababababababab";
constexpr std::string_view kSourceImage =
    "registry.example/gravesentinel@sha256:0123456789abcdef";
constexpr std::string_view kGoldenEncoding =
    R"({"artifacts":[{"chunks":[{"hash":"abababababababababababababababababababababababababababababababab","size":4194304},)"
    R"({"hash":"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd","size":17}],"mode":33188,)"
    R"("path":"memory/pages-1.img","size":4194321},{"chunks":[{"hash":"efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef",)"
    R"("size":29}],"mode":33188,"path":"metadata/inventory.json","size":29}],"checkpoint_id":"checkpoint-0007",)"
    R"("checksum":"de16f4eabc04edca6b98b3b9a3b19854a6b4c541c41aa3eb0b83230bae40298f","container_id":"container-01",)"
    R"("created_at_unix_nanoseconds":1730000000123456789,"format_version":1,"generation":7,"runtime_fingerprint":{)"
    R"("apparmor_profile":"host-default","architecture":"amd64","capability_profile":"CapEff=000001ffffffffff",)"
    R"("cgroup_controllers":"cpu cpuset hugetlb io memory misc pids rdma","cgroup_manager":"systemd","cgroup_version":"v2",)"
    R"("command_profile":"podman-criu-rootful-v1","criu_version":"4.1.1","kernel_release":"6.12.111+deb13-amd64",)"
    R"("oci_runtime_commit":"10269840aa07fb7e6b7e1acff6198692d8ff5c88","oci_runtime_features":"+APPARMOR +CAP +CRIU +SECCOMP +SELINUX",)"
    R"("oci_runtime_name":"crun","oci_runtime_version":"1.21","privilege_mode":"rootful","runtime_build_origin":"Debian 13",)"
    R"("runtime_name":"podman","runtime_version":"5.4.2","seccomp_profile":"default","selinux_mode":"disabled",)"
    R"("user_namespace_mode":"host"},"source_image_identity":"registry.example/gravesentinel@sha256:0123456789abcdef"})";

static_assert(kRegularFileMode ==
              (ManifestArtifactV1::posix_regular_file | 0644U));
static_assert(kFirstHash.size() == ChunkHash::digest_size * 2U);

[[nodiscard]] ChunkHash make_hash(std::byte value) {
  ChunkHash::Bytes bytes{};
  bytes.fill(value);
  return ChunkHash{bytes};
}

[[nodiscard]] RelativeSnapshotPath make_path(std::string value) {
  return std::move(RelativeSnapshotPath::create(std::move(value))).value();
}

[[nodiscard]] RelativeSnapshotPath
make_maximum_length_unique_path(std::size_t index) {
  constexpr std::string_view hex_digits = "0123456789abcdef";
  std::string path;
  path.reserve(4095U);
  for (std::size_t component = 0; component < 16U; ++component) {
    if (!path.empty()) {
      path.push_back('/');
    }
    path.append(ManifestLimits::maximum_path_component_bytes, 'p');
  }
  for (std::size_t digit = 0; digit < 3U; ++digit) {
    path[path.size() - digit - 1U] = hex_digits[index & 0x0FU];
    index >>= 4U;
  }
  return make_path(std::move(path));
}

[[nodiscard]] RuntimeFingerprint make_valid_runtime_fingerprint() {
  return RuntimeFingerprint{
      .runtime_name = "podman",
      .runtime_version = "5.4.2",
      .runtime_build_origin = "Debian 13",
      .architecture = "amd64",
      .kernel_release = "6.12.111+deb13-amd64",
      .criu_version = "4.1.1",
      .oci_runtime_name = "crun",
      .oci_runtime_version = "1.21",
      .oci_runtime_commit = "10269840aa07fb7e6b7e1acff6198692d8ff5c88",
      .oci_runtime_features = "+APPARMOR +CAP +CRIU +SECCOMP +SELINUX",
      .cgroup_version = "v2",
      .cgroup_manager = "systemd",
      .cgroup_controllers = "cpu cpuset hugetlb io memory misc pids rdma",
      .privilege_mode = "rootful",
      .user_namespace_mode = "host",
      .capability_profile = "CapEff=000001ffffffffff",
      .selinux_mode = "disabled",
      .apparmor_profile = "host-default",
      .seccomp_profile = "default",
      .command_profile = "podman-criu-rootful-v1",
  };
}

[[nodiscard]] ManifestV1 make_valid_manifest() {
  return ManifestV1{
      .container_id = ContainerId::create("container-01").value(),
      .checkpoint_id = CheckpointId::create("checkpoint-0007").value(),
      .generation = 7,
      .created_at_unix_nanoseconds = 1730000000123456789ULL,
      .source_image_identity = std::string{kSourceImage},
      .runtime_fingerprint = make_valid_runtime_fingerprint(),
      .artifacts =
          {
              ManifestArtifactV1{
                  .path = make_path("memory/pages-1.img"),
                  .size = ManifestLimits::chunk_bytes + 17U,
                  .mode = kRegularFileMode,
                  .chunks =
                      {
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xAB}),
                              .size = ManifestLimits::chunk_bytes,
                          },
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xCD}),
                              .size = 17,
                          },
                      },
              },
              ManifestArtifactV1{
                  .path = make_path("metadata/inventory.json"),
                  .size = 29,
                  .mode = kRegularFileMode,
                  .chunks =
                      {
                          ManifestChunkV1{
                              .hash = make_hash(std::byte{0xEF}),
                              .size = 29,
                          },
                      },
              },
          },
  };
}

[[nodiscard]] EncodedManifest bytes_from_text(std::string_view text) {
  EncodedManifest bytes;
  bytes.reserve(text.size());
  for (const auto character : text) {
    bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return bytes;
}

[[nodiscard]] std::string text_from_bytes(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const auto byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

[[nodiscard]] std::string encode_manifest(const ManifestV1 &manifest) {
  auto encoded = encode_manifest_v1(manifest);
  if (!encoded) {
    throw std::runtime_error(encoded.error().diagnostic);
  }
  return text_from_bytes(*encoded);
}

[[nodiscard]] std::string encode_valid_manifest() {
  return encode_manifest(make_valid_manifest());
}

[[nodiscard]] std::string checksum_from_encoding(std::string_view encoded) {
  constexpr std::string_view prefix = "\"checksum\":\"";
  const auto checksum_begin = encoded.find(prefix);
  if (checksum_begin == std::string_view::npos) {
    throw std::logic_error("encoded manifest has no checksum");
  }
  return std::string(encoded.substr(checksum_begin + prefix.size(),
                                    ChunkHash::digest_size * 2U));
}

[[nodiscard]] std::string replace_once(std::string text,
                                       std::string_view original,
                                       std::string_view replacement) {
  const auto position = text.find(original);
  if (position == std::string::npos) {
    throw std::logic_error("manifest mutation source was not found");
  }
  text.replace(position, original.size(), replacement);
  return text;
}

void expect_decode_error(std::string_view text,
                         ManifestErrorCode expected_code) {
  const auto result = decode_manifest_v1(bytes_from_text(text));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, expected_code);
}

void expect_pre_checksum_error_after_replacement(
    std::string_view encoded, std::string_view original,
    std::string_view replacement, ManifestErrorCode expected_code) {
  expect_decode_error(replace_once(std::string{encoded}, original, replacement),
                      expected_code);
}

TEST(ManifestCodecTest, EncodesDeterministicGoldenCanonicalJson) {
  const auto first = encode_manifest_v1(make_valid_manifest());
  const auto second = encode_manifest_v1(make_valid_manifest());

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first, *second);

  const auto text = text_from_bytes(*first);
  EXPECT_EQ(text, kGoldenEncoding);
  EXPECT_EQ(text.find_first_of("\n\r\t"), std::string::npos);
  EXPECT_EQ(text.find(": "), std::string::npos);
  EXPECT_EQ(text.find(", "), std::string::npos);
}

TEST(ManifestCodecTest, RoundTripsEqualityAndPreservesOrdering) {
  const auto manifest = make_valid_manifest();
  const auto encoded = encode_manifest_v1(manifest);
  ASSERT_TRUE(encoded);

  const auto decoded = decode_manifest_v1(*encoded);

  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, manifest);
  ASSERT_EQ(decoded->artifacts.size(), 2U);
  EXPECT_EQ(decoded->artifacts[0].path.value(), "memory/pages-1.img");
  EXPECT_EQ(decoded->artifacts[1].path.value(), "metadata/inventory.json");
  ASSERT_EQ(decoded->artifacts[0].chunks.size(), 2U);
  EXPECT_EQ(decoded->artifacts[0].chunks[0].hash, make_hash(std::byte{0xAB}));
  EXPECT_EQ(decoded->artifacts[0].chunks[1].hash, make_hash(std::byte{0xCD}));
}

TEST(ManifestCodecTest, EmitsChecksumAsExactly64LowercaseHexCharacters) {
  const auto encoded = encode_valid_manifest();
  constexpr std::string_view prefix = "\"checksum\":\"";
  const auto checksum_begin = encoded.find(prefix);
  ASSERT_NE(checksum_begin, std::string::npos);
  const auto value_begin = checksum_begin + prefix.size();
  const auto checksum = std::string_view{encoded}.substr(
      value_begin, ChunkHash::digest_size * 2U);

  ASSERT_EQ(checksum.size(), 64U);
  EXPECT_TRUE(std::ranges::all_of(checksum, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  }));
  ASSERT_LT(value_begin + checksum.size(), encoded.size());
  EXPECT_EQ(encoded[value_begin + checksum.size()], '"');
}

TEST(ManifestCodecTest, EveryProtectedFieldAndOrderAffectsChecksum) {
  const auto baseline = make_valid_manifest();
  const auto baseline_checksum =
      checksum_from_encoding(encode_manifest(baseline));
  std::vector<ManifestV1> variants;
  const auto add_variant = [&baseline, &variants](auto mutate) {
    auto variant = baseline;
    mutate(variant);
    variants.push_back(std::move(variant));
  };

  add_variant([](ManifestV1 &value) {
    value.container_id = ContainerId::create("container-02").value();
  });
  add_variant([](ManifestV1 &value) {
    value.checkpoint_id = CheckpointId::create("checkpoint-0008").value();
  });
  add_variant([](ManifestV1 &value) { ++value.generation; });
  add_variant([](ManifestV1 &value) { ++value.created_at_unix_nanoseconds; });
  add_variant([](ManifestV1 &value) {
    value.source_image_identity =
        "registry.example/gravesentinel@sha256:1123456789abcdef";
  });
  add_variant([](ManifestV1 &value) {
    value.runtime_fingerprint.command_profile = "podman-criu-rootful-v2";
  });
  add_variant([](ManifestV1 &value) {
    value.artifacts[0].path = make_path("memory/pages-2.img");
  });
  add_variant([](ManifestV1 &value) { value.artifacts[0].mode = 0100600U; });
  add_variant([](ManifestV1 &value) {
    std::swap(value.artifacts[0], value.artifacts[1]);
  });
  add_variant([](ManifestV1 &value) {
    std::swap(value.artifacts[0].chunks[0].hash,
              value.artifacts[0].chunks[1].hash);
  });
  add_variant([](ManifestV1 &value) {
    ++value.artifacts[1].size;
    ++value.artifacts[1].chunks[0].size;
  });

  for (const auto &variant : variants) {
    EXPECT_NE(checksum_from_encoding(encode_manifest(variant)),
              baseline_checksum);
  }
}

TEST(ManifestCodecTest, RejectsAlteredProtectedPayloadFields) {
  const auto encoded = encode_valid_manifest();

  expect_decode_error(
      replace_once(encoded, "\"generation\":7", "\"generation\":8"),
      ManifestErrorCode::checksum_mismatch);
  expect_decode_error(
      replace_once(encoded, kSourceImage,
                   "registry.example/gravesentinel@sha256:1123456789abcdef"),
      ManifestErrorCode::checksum_mismatch);
}

TEST(ManifestCodecTest, RejectsAlteredChecksum) {
  auto encoded = encode_valid_manifest();
  constexpr std::string_view prefix = "\"checksum\":\"";
  const auto checksum_begin = encoded.find(prefix);
  ASSERT_NE(checksum_begin, std::string::npos);
  const auto first_digit = checksum_begin + prefix.size();
  ASSERT_LT(first_digit, encoded.size());
  encoded[first_digit] = encoded[first_digit] == '0' ? '1' : '0';

  expect_decode_error(encoded, ManifestErrorCode::checksum_mismatch);
}

TEST(ManifestCodecTest, RejectsMalformedTruncatedAndTrailingJson) {
  const auto encoded = encode_valid_manifest();
  const std::array malformed_documents{
      std::string{"{not-json}"},
      encoded.substr(0, encoded.size() - 1U),
      encoded + "{}",
  };

  for (const auto &document : malformed_documents) {
    SCOPED_TRACE(document.size());
    expect_decode_error(document, ManifestErrorCode::malformed_json);
  }
}

TEST(ManifestCodecTest, RejectsUnknownAndMissingTopLevelFields) {
  const auto encoded = encode_valid_manifest();

  expect_pre_checksum_error_after_replacement(
      encoded, "{", "{\"unknown\":0,", ManifestErrorCode::schema_violation);
  expect_pre_checksum_error_after_replacement(
      encoded,
      ",\"source_image_identity\":\"registry.example/gravesentinel@sha256:"
      "0123456789abcdef\"",
      "", ManifestErrorCode::schema_violation);
}

TEST(ManifestCodecTest, RejectsUnknownAndMissingNestedFields) {
  const auto encoded = encode_valid_manifest();

  expect_pre_checksum_error_after_replacement(
      encoded, "\"chunks\":[", "\"unknown\":0,\"chunks\":[",
      ManifestErrorCode::schema_violation);
  expect_pre_checksum_error_after_replacement(
      encoded,
      "{\"hash\":"
      "\"abababababababababababababababababababababababababababababababab\","
      "\"size\":4194304}",
      "{\"size\":4194304}", ManifestErrorCode::schema_violation);
  expect_pre_checksum_error_after_replacement(
      encoded, ",\"seccomp_profile\":\"default\"", "",
      ManifestErrorCode::schema_violation);
}

TEST(ManifestCodecTest, RejectsUnsupportedAndMistypedVersions) {
  const auto encoded = encode_valid_manifest();

  for (const auto version : {std::string_view{"0"}, std::string_view{"2"}}) {
    SCOPED_TRACE(version);
    expect_pre_checksum_error_after_replacement(
        encoded, "\"format_version\":1",
        std::string{"\"format_version\":"} + std::string{version},
        ManifestErrorCode::unsupported_version);
  }
  expect_pre_checksum_error_after_replacement(
      encoded, "\"format_version\":1", "\"format_version\":\"1\"",
      ManifestErrorCode::schema_violation);
}

TEST(ManifestCodecTest, RejectsUnsafeAndDuplicateArtifactPaths) {
  const auto encoded = encode_valid_manifest();

  expect_pre_checksum_error_after_replacement(
      encoded, "\"path\":\"memory/pages-1.img\"", "\"path\":\"../pages-1.img\"",
      ManifestErrorCode::invalid_relative_path);
  expect_pre_checksum_error_after_replacement(
      encoded, "\"path\":\"metadata/inventory.json\"",
      "\"path\":\"memory/pages-1.img\"",
      ManifestErrorCode::duplicate_artifact_path);
}

TEST(ManifestCodecTest, RejectsShortLongUppercaseAndNonhexChunkHashes) {
  const auto encoded = encode_valid_manifest();
  auto uppercase_hash = std::string{kFirstHash};
  uppercase_hash.front() = 'A';
  auto nonhex_hash = std::string{kFirstHash};
  nonhex_hash.front() = 'g';
  const std::array invalid_hashes{
      std::string{kFirstHash.substr(0, kFirstHash.size() - 1U)},
      std::string{kFirstHash} + "a",
      uppercase_hash,
      nonhex_hash,
  };

  for (const auto &hash : invalid_hashes) {
    SCOPED_TRACE(hash);
    expect_pre_checksum_error_after_replacement(
        encoded, kFirstHash, hash, ManifestErrorCode::invalid_hash_encoding);
  }
}

TEST(ManifestCodecTest, RejectsNonUnsignedOrOverflowingSizes) {
  const auto encoded = encode_valid_manifest();
  constexpr std::array invalid_literals{
      std::string_view{"-1"},
      std::string_view{"1.5"},
      std::string_view{"1e0"},
      std::string_view{"18446744073709551616"},
  };

  for (const auto literal : invalid_literals) {
    SCOPED_TRACE(literal);
    expect_pre_checksum_error_after_replacement(
        encoded, "\"size\":4194304",
        std::string{"\"size\":"} + std::string{literal},
        ManifestErrorCode::schema_violation);
  }
}

TEST(ManifestCodecTest, RejectsArtifactSizeInconsistentWithChunks) {
  const auto encoded = encode_valid_manifest();

  expect_pre_checksum_error_after_replacement(
      encoded, "\"size\":4194321", "\"size\":4194322",
      ManifestErrorCode::invalid_chunk_layout);
}

TEST(ManifestCodecTest, RejectsNoncanonicalWhitespaceAndKeyOrder) {
  const auto encoded = encode_valid_manifest();
  auto with_whitespace = encoded;
  with_whitespace.insert(1U, 1U, ' ');
  const auto reordered = replace_once(
      encoded,
      "\"runtime_fingerprint\":{\"apparmor_profile\":\"host-default\","
      "\"architecture\":\"amd64\"",
      "\"runtime_fingerprint\":{\"architecture\":\"amd64\","
      "\"apparmor_profile\":\"host-default\"");

  expect_decode_error(with_whitespace,
                      ManifestErrorCode::non_canonical_encoding);
  expect_decode_error(reordered, ManifestErrorCode::non_canonical_encoding);
}

TEST(ManifestCodecTest, RejectsDuplicateJsonKeyDuringPreflight) {
  const auto encoded = encode_valid_manifest();
  const auto duplicate_key = replace_once(encoded, "{", "{\"generation\":7,");

  expect_decode_error(duplicate_key, ManifestErrorCode::schema_violation);
}

TEST(ManifestCodecTest, RejectsJsonBeyondDepthLimit) {
  std::string deeply_nested(ManifestLimits::maximum_json_depth + 2U, '[');
  deeply_nested.push_back('0');
  deeply_nested.append(ManifestLimits::maximum_json_depth + 2U, ']');

  expect_decode_error(deeply_nested, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestCodecTest, PreflightRejectsCountsBeforeBuildingJsonDom) {
  const auto repeated_array = [](std::string_view key, std::size_t count) {
    std::string document = "{\"" + std::string(key) + "\":[";
    for (std::size_t index = 0; index < count; ++index) {
      if (index != 0) {
        document.push_back(',');
      }
      document.push_back('0');
    }
    document.append("]}");
    return document;
  };

  expect_decode_error(
      repeated_array("artifacts", ManifestLimits::maximum_artifacts + 1U),
      ManifestErrorCode::limit_exceeded);
  expect_decode_error(
      repeated_array("chunks", ManifestLimits::maximum_chunks + 1U),
      ManifestErrorCode::limit_exceeded);
}

TEST(ManifestCodecTest, PreflightRejectsLargeStringsAndObjects) {
  const auto oversized_string =
      std::string{"{\"value\":\""} +
      std::string(ManifestLimits::maximum_relative_path_bytes + 1U, 'a') +
      "\"}";
  std::string oversized_object{"{"};
  for (std::size_t index = 0;
       index < ManifestLimits::maximum_json_object_fields + 1U; ++index) {
    if (index != 0) {
      oversized_object.push_back(',');
    }
    oversized_object.append("\"field" + std::to_string(index) + "\":0");
  }
  oversized_object.push_back('}');

  expect_decode_error(oversized_string, ManifestErrorCode::limit_exceeded);
  expect_decode_error(oversized_object, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestCodecTest, EncoderRejectsOutputBeyondByteLimit) {
  auto manifest = make_valid_manifest();
  manifest.artifacts.clear();
  manifest.artifacts.reserve(ManifestLimits::maximum_artifacts);
  for (std::size_t index = 0; index < ManifestLimits::maximum_artifacts;
       ++index) {
    manifest.artifacts.push_back(ManifestArtifactV1{
        .path = make_maximum_length_unique_path(index),
        .size = 0,
        .mode = kRegularFileMode,
        .chunks = {},
    });
  }

  const auto result = encode_manifest_v1(manifest);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ManifestErrorCode::limit_exceeded);
}

TEST(ManifestCodecTest, DecoderRejectsEncodingBeyondByteLimit) {
  const EncodedManifest oversized(ManifestLimits::maximum_encoded_bytes + 1U,
                                  std::byte{0x20});

  const auto result = decode_manifest_v1(oversized);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, ManifestErrorCode::limit_exceeded);
}

} // namespace
} // namespace gravesentinel::ccm
