#include "src/manifest/codec.hxx"

#include "src/crypto/blake3.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gravesentinel::ccm {
namespace {

using Json = nlohmann::json;

[[nodiscard]] ManifestError
make_error(ManifestErrorCode code, std::string field, std::string diagnostic) {
  return ManifestError{
      .code = code,
      .field = std::move(field),
      .diagnostic = std::move(diagnostic),
  };
}

class ManifestPreflightSax final : public nlohmann::json_sax<Json> {
public:
  bool null() override { return begin_value(); }
  bool boolean(bool) override { return begin_value(); }
  bool number_integer(number_integer_t) override { return begin_value(); }
  bool number_unsigned(number_unsigned_t) override { return begin_value(); }
  bool number_float(number_float_t, const string_t &) override {
    return begin_value();
  }

  bool string(string_t &value) override {
    if (value.size() > ManifestLimits::maximum_relative_path_bytes) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "JSON string exceeds the manifest byte limit");
    }
    return begin_value();
  }

  bool binary(binary_t &) override {
    return fail(ManifestErrorCode::schema_violation,
                "binary JSON values are not supported");
  }

  bool start_object(std::size_t) override {
    if (!begin_value()) {
      return false;
    }
    return push_frame(FrameKind::object);
  }

  bool key(string_t &value) override {
    if (frames_.empty() || frames_.back().kind != FrameKind::object) {
      return fail(ManifestErrorCode::malformed_json,
                  "JSON key appears outside an object");
    }
    auto &frame = frames_.back();
    ++frame.entries;
    if (frame.entries > ManifestLimits::maximum_json_object_fields) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "JSON object contains too many fields");
    }
    if (!frame.keys.insert(value).second) {
      return fail(ManifestErrorCode::schema_violation,
                  "JSON object contains a duplicate key");
    }
    frame.pending_key = value;
    frame.has_pending_key = true;
    return true;
  }

  bool end_object() override {
    if (frames_.empty() || frames_.back().kind != FrameKind::object ||
        frames_.back().has_pending_key) {
      return fail(ManifestErrorCode::malformed_json,
                  "JSON object ended in an invalid state");
    }
    frames_.pop_back();
    return true;
  }

  bool start_array(std::size_t) override {
    auto kind = FrameKind::generic_array;
    if (!frames_.empty() && frames_.back().kind == FrameKind::object &&
        frames_.back().has_pending_key) {
      if (frames_.back().pending_key == "artifacts") {
        kind = FrameKind::artifacts_array;
      } else if (frames_.back().pending_key == "chunks") {
        kind = FrameKind::chunks_array;
      }
    }
    if (!begin_value()) {
      return false;
    }
    return push_frame(kind);
  }

  bool end_array() override {
    if (frames_.empty() || frames_.back().kind == FrameKind::object) {
      return fail(ManifestErrorCode::malformed_json,
                  "JSON array ended in an invalid state");
    }
    frames_.pop_back();
    return true;
  }

  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &error) override {
    if (!error_.has_value()) {
      error_ = make_error(ManifestErrorCode::malformed_json, "manifest",
                          std::string("manifest is not valid JSON: ") +
                              error.what());
    }
    return false;
  }

  [[nodiscard]] ManifestError error() const {
    if (error_.has_value()) {
      return *error_;
    }
    return make_error(ManifestErrorCode::malformed_json, "manifest",
                      "manifest is not valid JSON");
  }

private:
  enum class FrameKind : std::uint8_t {
    object,
    generic_array,
    artifacts_array,
    chunks_array,
  };

  struct Frame {
    FrameKind kind;
    std::size_t entries{0};
    std::unordered_set<std::string> keys;
    std::string pending_key;
    bool has_pending_key{false};
  };

  [[nodiscard]] bool begin_value() {
    ++nodes_;
    if (nodes_ > ManifestLimits::maximum_json_nodes) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "JSON contains too many values");
    }
    if (frames_.empty()) {
      if (root_seen_) {
        return fail(ManifestErrorCode::malformed_json,
                    "JSON contains more than one root value");
      }
      root_seen_ = true;
      return true;
    }

    auto &parent = frames_.back();
    if (parent.kind == FrameKind::object) {
      if (!parent.has_pending_key) {
        return fail(ManifestErrorCode::malformed_json,
                    "JSON object value has no key");
      }
      parent.pending_key.clear();
      parent.has_pending_key = false;
      return true;
    }

    ++parent.entries;
    if (parent.kind == FrameKind::artifacts_array &&
        parent.entries > ManifestLimits::maximum_artifacts) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "manifest contains too many artifacts");
    }
    if (parent.kind == FrameKind::chunks_array) {
      ++total_chunks_;
      if (total_chunks_ > ManifestLimits::maximum_chunks) {
        return fail(ManifestErrorCode::limit_exceeded,
                    "manifest contains too many chunks");
      }
    } else if (parent.kind == FrameKind::generic_array &&
               parent.entries > ManifestLimits::maximum_chunks) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "JSON array contains too many values");
    }
    return true;
  }

  [[nodiscard]] bool push_frame(FrameKind kind) {
    if (frames_.size() >= ManifestLimits::maximum_json_depth) {
      return fail(ManifestErrorCode::limit_exceeded,
                  "JSON nesting exceeds the manifest depth limit");
    }
    frames_.push_back(Frame{
        .kind = kind,
        .entries = 0,
        .keys = {},
        .pending_key = {},
        .has_pending_key = false,
    });
    return true;
  }

  [[nodiscard]] bool fail(ManifestErrorCode code, std::string diagnostic) {
    if (!error_.has_value()) {
      error_ = make_error(code, "manifest", std::move(diagnostic));
    }
    return false;
  }

  std::vector<Frame> frames_;
  std::optional<ManifestError> error_;
  std::size_t nodes_{0};
  std::size_t total_chunks_{0};
  bool root_seen_{false};
};

[[nodiscard]] std::string hash_to_hex(const ChunkHash &hash) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(ChunkHash::digest_size * 2);
  for (const auto byte : hash.bytes()) {
    const auto value = std::to_integer<unsigned int>(byte);
    encoded.push_back(digits[(value >> 4U) & 0x0FU]);
    encoded.push_back(digits[value & 0x0FU]);
  }
  return encoded;
}

[[nodiscard]] int hex_nibble(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

[[nodiscard]] ManifestResult<ChunkHash> hash_from_hex(std::string_view encoded,
                                                      std::string field) {
  if (encoded.size() != ChunkHash::digest_size * 2) {
    return std::unexpected(make_error(
        ManifestErrorCode::invalid_hash_encoding, std::move(field),
        "BLAKE3 digest must contain exactly 64 lowercase hex characters"));
  }

  ChunkHash::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto high = hex_nibble(encoded[index * 2]);
    const auto low = hex_nibble(encoded[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::unexpected(make_error(
          ManifestErrorCode::invalid_hash_encoding, std::move(field),
          "BLAKE3 digest must contain only lowercase hex characters"));
    }
    const auto value = static_cast<unsigned int>((high << 4) | low);
    bytes[index] = static_cast<std::byte>(value);
  }
  return ChunkHash(bytes);
}

[[nodiscard]] std::span<const std::byte>
as_bytes(std::string_view value) noexcept {
  return std::as_bytes(std::span(value.data(), value.size()));
}

[[nodiscard]] Json manifest_payload_json(const ManifestV2 &manifest) {
  Json artifacts = Json::array();
  for (const auto &artifact : manifest.artifacts) {
    Json chunks = Json::array();
    for (const auto &chunk : artifact.chunks) {
      chunks.push_back(Json{
          {"hash", hash_to_hex(chunk.hash)},
          {"size", static_cast<Json::number_unsigned_t>(chunk.size)},
      });
    }
    artifacts.push_back(Json{
        {"chunks", std::move(chunks)},
        {"mode", static_cast<Json::number_unsigned_t>(artifact.mode)},
        {"path", artifact.path.value()},
        {"size", static_cast<Json::number_unsigned_t>(artifact.size)},
    });
  }

  const auto &runtime = manifest.runtime_fingerprint;
  return Json{
      {"artifacts", std::move(artifacts)},
      {"checkpoint_id", manifest.checkpoint_id.value()},
      {"container_cpus", manifest.container_cpus.values()},
      {"container_id", manifest.container_id.value()},
      {"created_at_unix_nanoseconds",
       static_cast<Json::number_unsigned_t>(
           manifest.created_at_unix_nanoseconds)},
      {"format_version",
       static_cast<Json::number_unsigned_t>(ManifestV2::format_version)},
      {"generation", static_cast<Json::number_unsigned_t>(manifest.generation)},
      {"runtime_fingerprint",
       Json{
           {"apparmor_profile", runtime.apparmor_profile},
           {"architecture", runtime.architecture},
           {"capability_profile", runtime.capability_profile},
           {"cgroup_controllers", runtime.cgroup_controllers},
           {"cgroup_manager", runtime.cgroup_manager},
           {"cgroup_version", runtime.cgroup_version},
           {"command_profile", runtime.command_profile},
           {"criu_version", runtime.criu_version},
           {"kernel_release", runtime.kernel_release},
           {"oci_runtime_commit", runtime.oci_runtime_commit},
           {"oci_runtime_features", runtime.oci_runtime_features},
           {"oci_runtime_name", runtime.oci_runtime_name},
           {"oci_runtime_version", runtime.oci_runtime_version},
           {"privilege_mode", runtime.privilege_mode},
           {"runtime_build_origin", runtime.runtime_build_origin},
           {"runtime_name", runtime.runtime_name},
           {"runtime_version", runtime.runtime_version},
           {"seccomp_profile", runtime.seccomp_profile},
           {"selinux_mode", runtime.selinux_mode},
           {"user_namespace_mode", runtime.user_namespace_mode},
       }},
      {"source_image_identity", manifest.source_image_identity},
  };
}

[[nodiscard]] ManifestResult<std::string>
dump_canonical_json(const Json &document) {
  try {
    return document.dump(-1, ' ', false, Json::error_handler_t::strict);
  } catch (const Json::exception &error) {
    return std::unexpected(make_error(
        ManifestErrorCode::invalid_text, "manifest",
        std::string("cannot encode canonical JSON: ") + error.what()));
  }
}

[[nodiscard]] ManifestResult<std::string>
encode_manifest_text(const ManifestV2 &manifest) {
  if (auto validation = validate_manifest_v2(manifest); !validation) {
    return std::unexpected(std::move(validation.error()));
  }

  auto payload = manifest_payload_json(manifest);
  auto payload_text = dump_canonical_json(payload);
  if (!payload_text) {
    return std::unexpected(std::move(payload_text.error()));
  }
  const auto checksum = blake3_hash(as_bytes(*payload_text));
  payload["checksum"] = hash_to_hex(checksum);

  auto encoded = dump_canonical_json(payload);
  if (!encoded) {
    return encoded;
  }
  if (encoded->size() > ManifestLimits::maximum_encoded_bytes) {
    return std::unexpected(
        make_error(ManifestErrorCode::limit_exceeded, "manifest",
                   "encoded manifest exceeds the byte limit"));
  }
  return encoded;
}

[[nodiscard]] bool
has_exact_keys(const Json &object,
               std::initializer_list<const char *> expected_keys) {
  if (!object.is_object() || object.size() != expected_keys.size()) {
    return false;
  }
  return std::ranges::all_of(expected_keys, [&object](const char *key) {
    return object.contains(key);
  });
}

[[nodiscard]] ManifestResult<std::uint64_t> read_unsigned(const Json &value,
                                                          std::string field) {
  if (!value.is_number_unsigned()) {
    return std::unexpected(
        make_error(ManifestErrorCode::schema_violation, std::move(field),
                   "field must be an unsigned integer literal"));
  }
  return value.get<std::uint64_t>();
}

[[nodiscard]] ManifestResult<CpuSet> read_cpu_set(const Json &value) {
  if (!value.is_array() || value.empty() ||
      value.size() > CpuSet::maximum_size) {
    return std::unexpected(
        make_error(ManifestErrorCode::schema_violation, "container_cpus",
                   "container_cpus must be a nonempty bounded array"));
  }
  std::vector<std::uint32_t> cpus;
  cpus.reserve(value.size());
  for (const auto &encoded_cpu : value) {
    auto cpu = read_unsigned(encoded_cpu, "container_cpus");
    if (!cpu || *cpu > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(
          make_error(ManifestErrorCode::schema_violation, "container_cpus",
                     "container_cpus entries must be uint32 values"));
    }
    cpus.push_back(static_cast<std::uint32_t>(*cpu));
  }
  auto result = CpuSet::create(std::move(cpus));
  if (!result) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "container_cpus",
                                      result.error().diagnostic));
  }
  return std::move(*result);
}

[[nodiscard]] ManifestResult<std::string> read_string(const Json &value,
                                                      std::string field) {
  if (!value.is_string()) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      std::move(field),
                                      "field must be a string"));
  }
  return value.get<std::string>();
}

[[nodiscard]] ManifestResult<void> read_string_into(const Json &object,
                                                    const char *key,
                                                    std::string &destination) {
  auto value =
      read_string(object.at(key), std::string("runtime_fingerprint.") + key);
  if (!value) {
    return std::unexpected(std::move(value.error()));
  }
  destination = std::move(*value);
  return {};
}

[[nodiscard]] ManifestResult<RuntimeFingerprint>
read_runtime_fingerprint(const Json &value) {
  if (!has_exact_keys(value, {"apparmor_profile",     "architecture",
                              "capability_profile",   "cgroup_controllers",
                              "cgroup_manager",       "cgroup_version",
                              "command_profile",      "criu_version",
                              "kernel_release",       "oci_runtime_commit",
                              "oci_runtime_features", "oci_runtime_name",
                              "oci_runtime_version",  "privilege_mode",
                              "runtime_build_origin", "runtime_name",
                              "runtime_version",      "seccomp_profile",
                              "selinux_mode",         "user_namespace_mode"})) {
    return std::unexpected(
        make_error(ManifestErrorCode::schema_violation, "runtime_fingerprint",
                   "runtime fingerprint must contain exactly the v1 fields"));
  }

  RuntimeFingerprint fingerprint;
  const std::pair<const char *, std::string *> fields[]{
      {"runtime_name", &fingerprint.runtime_name},
      {"runtime_version", &fingerprint.runtime_version},
      {"runtime_build_origin", &fingerprint.runtime_build_origin},
      {"architecture", &fingerprint.architecture},
      {"kernel_release", &fingerprint.kernel_release},
      {"criu_version", &fingerprint.criu_version},
      {"oci_runtime_name", &fingerprint.oci_runtime_name},
      {"oci_runtime_version", &fingerprint.oci_runtime_version},
      {"oci_runtime_commit", &fingerprint.oci_runtime_commit},
      {"oci_runtime_features", &fingerprint.oci_runtime_features},
      {"cgroup_version", &fingerprint.cgroup_version},
      {"cgroup_manager", &fingerprint.cgroup_manager},
      {"cgroup_controllers", &fingerprint.cgroup_controllers},
      {"privilege_mode", &fingerprint.privilege_mode},
      {"user_namespace_mode", &fingerprint.user_namespace_mode},
      {"capability_profile", &fingerprint.capability_profile},
      {"selinux_mode", &fingerprint.selinux_mode},
      {"apparmor_profile", &fingerprint.apparmor_profile},
      {"seccomp_profile", &fingerprint.seccomp_profile},
      {"command_profile", &fingerprint.command_profile},
  };
  for (const auto &[key, destination] : fields) {
    if (auto result = read_string_into(value, key, *destination); !result) {
      return std::unexpected(std::move(result.error()));
    }
  }
  return fingerprint;
}

[[nodiscard]] ManifestResult<std::vector<ManifestArtifactV1>>
read_artifacts(const Json &value) {
  if (!value.is_array()) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "artifacts",
                                      "artifacts must be an array"));
  }
  if (value.size() > ManifestLimits::maximum_artifacts) {
    return std::unexpected(make_error(ManifestErrorCode::limit_exceeded,
                                      "artifacts",
                                      "manifest contains too many artifacts"));
  }

  std::vector<ManifestArtifactV1> artifacts;
  artifacts.reserve(value.size());
  std::size_t total_chunks = 0;

  for (std::size_t artifact_index = 0; artifact_index < value.size();
       ++artifact_index) {
    const auto &encoded_artifact = value[artifact_index];
    const auto field_prefix =
        "artifacts[" + std::to_string(artifact_index) + "]";
    if (!has_exact_keys(encoded_artifact, {"chunks", "mode", "path", "size"})) {
      return std::unexpected(
          make_error(ManifestErrorCode::schema_violation, field_prefix,
                     "artifact must contain exactly the v1 fields"));
    }

    auto path_text =
        read_string(encoded_artifact.at("path"), field_prefix + ".path");
    if (!path_text) {
      return std::unexpected(std::move(path_text.error()));
    }
    auto path = RelativeSnapshotPath::create(std::move(*path_text));
    if (!path) {
      auto error = std::move(path.error());
      error.field = field_prefix + ".path";
      return std::unexpected(std::move(error));
    }

    auto size =
        read_unsigned(encoded_artifact.at("size"), field_prefix + ".size");
    if (!size) {
      return std::unexpected(std::move(size.error()));
    }
    auto mode =
        read_unsigned(encoded_artifact.at("mode"), field_prefix + ".mode");
    if (!mode) {
      return std::unexpected(std::move(mode.error()));
    }
    if (*mode > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                        field_prefix + ".mode",
                                        "file mode exceeds uint32"));
    }

    const auto &encoded_chunks = encoded_artifact.at("chunks");
    if (!encoded_chunks.is_array()) {
      return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                        field_prefix + ".chunks",
                                        "chunks must be an array"));
    }
    if (encoded_chunks.size() > ManifestLimits::maximum_chunks - total_chunks) {
      return std::unexpected(make_error(ManifestErrorCode::limit_exceeded,
                                        field_prefix + ".chunks",
                                        "manifest contains too many chunks"));
    }
    total_chunks += encoded_chunks.size();

    std::vector<ManifestChunkV1> chunks;
    chunks.reserve(encoded_chunks.size());
    for (std::size_t chunk_index = 0; chunk_index < encoded_chunks.size();
         ++chunk_index) {
      const auto &encoded_chunk = encoded_chunks[chunk_index];
      const auto chunk_prefix =
          field_prefix + ".chunks[" + std::to_string(chunk_index) + "]";
      if (!has_exact_keys(encoded_chunk, {"hash", "size"})) {
        return std::unexpected(
            make_error(ManifestErrorCode::schema_violation, chunk_prefix,
                       "chunk must contain exactly hash and size"));
      }

      auto hash_text =
          read_string(encoded_chunk.at("hash"), chunk_prefix + ".hash");
      if (!hash_text) {
        return std::unexpected(std::move(hash_text.error()));
      }
      auto hash = hash_from_hex(*hash_text, chunk_prefix + ".hash");
      if (!hash) {
        return std::unexpected(std::move(hash.error()));
      }
      auto chunk_size =
          read_unsigned(encoded_chunk.at("size"), chunk_prefix + ".size");
      if (!chunk_size) {
        return std::unexpected(std::move(chunk_size.error()));
      }
      chunks.push_back(ManifestChunkV1{
          .hash = std::move(*hash),
          .size = *chunk_size,
      });
    }

    artifacts.push_back(ManifestArtifactV1{
        .path = std::move(*path),
        .size = *size,
        .mode = static_cast<std::uint32_t>(*mode),
        .chunks = std::move(chunks),
    });
  }
  return artifacts;
}

[[nodiscard]] EncodedManifest string_to_bytes(std::string_view encoded) {
  EncodedManifest bytes;
  bytes.reserve(encoded.size());
  for (const auto character : encoded) {
    bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return bytes;
}

[[nodiscard]] std::string bytes_to_string(std::span<const std::byte> encoded) {
  std::string text;
  text.reserve(encoded.size());
  for (const auto byte : encoded) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

} // namespace

ManifestResult<EncodedManifest> encode_manifest_v2(const ManifestV2 &manifest) {
  auto encoded = encode_manifest_text(manifest);
  if (!encoded) {
    return std::unexpected(std::move(encoded.error()));
  }
  return string_to_bytes(*encoded);
}

ManifestResult<ManifestV2>
decode_manifest_v2(std::span<const std::byte> encoded) {
  if (encoded.empty()) {
    return std::unexpected(make_error(ManifestErrorCode::malformed_json,
                                      "manifest",
                                      "manifest must not be empty"));
  }
  if (encoded.size() > ManifestLimits::maximum_encoded_bytes) {
    return std::unexpected(
        make_error(ManifestErrorCode::limit_exceeded, "manifest",
                   "encoded manifest exceeds the byte limit"));
  }

  const auto text = bytes_to_string(encoded);
  ManifestPreflightSax preflight;
  if (!Json::sax_parse(text, &preflight, Json::input_format_t::json, true,
                       false)) {
    return std::unexpected(preflight.error());
  }

  const auto document = Json::parse(text, nullptr, false, false);
  if (document.is_discarded()) {
    return std::unexpected(make_error(ManifestErrorCode::malformed_json,
                                      "manifest",
                                      "manifest is not valid JSON"));
  }
  if (!document.is_object()) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "manifest",
                                      "manifest root must be an object"));
  }
  if (!document.contains("format_version")) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "format_version",
                                      "manifest format version is required"));
  }
  auto version = read_unsigned(document.at("format_version"), "format_version");
  if (!version) {
    return std::unexpected(std::move(version.error()));
  }
  if (*version != ManifestV2::format_version) {
    return std::unexpected(
        make_error(ManifestErrorCode::unsupported_version, "format_version",
                   "manifest format version is not supported"));
  }
  if (!has_exact_keys(
          document,
          {"artifacts", "checkpoint_id", "checksum", "container_cpus",
           "container_id", "created_at_unix_nanoseconds", "format_version",
           "generation", "runtime_fingerprint", "source_image_identity"})) {
    return std::unexpected(
        make_error(ManifestErrorCode::schema_violation, "manifest",
                   "manifest must contain exactly the v2 fields"));
  }

  auto container_text =
      read_string(document.at("container_id"), "container_id");
  if (!container_text) {
    return std::unexpected(std::move(container_text.error()));
  }
  auto container_id = ContainerId::create(std::move(*container_text));
  if (!container_id) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "container_id",
                                      container_id.error().diagnostic));
  }

  auto checkpoint_text =
      read_string(document.at("checkpoint_id"), "checkpoint_id");
  if (!checkpoint_text) {
    return std::unexpected(std::move(checkpoint_text.error()));
  }
  auto checkpoint_id = CheckpointId::create(std::move(*checkpoint_text));
  if (!checkpoint_id) {
    return std::unexpected(make_error(ManifestErrorCode::schema_violation,
                                      "checkpoint_id",
                                      checkpoint_id.error().diagnostic));
  }

  auto generation = read_unsigned(document.at("generation"), "generation");
  if (!generation) {
    return std::unexpected(std::move(generation.error()));
  }
  auto created_at = read_unsigned(document.at("created_at_unix_nanoseconds"),
                                  "created_at_unix_nanoseconds");
  if (!created_at) {
    return std::unexpected(std::move(created_at.error()));
  }
  auto container_cpus = read_cpu_set(document.at("container_cpus"));
  if (!container_cpus) {
    return std::unexpected(std::move(container_cpus.error()));
  }
  auto source_image = read_string(document.at("source_image_identity"),
                                  "source_image_identity");
  if (!source_image) {
    return std::unexpected(std::move(source_image.error()));
  }
  auto runtime = read_runtime_fingerprint(document.at("runtime_fingerprint"));
  if (!runtime) {
    return std::unexpected(std::move(runtime.error()));
  }
  auto artifacts = read_artifacts(document.at("artifacts"));
  if (!artifacts) {
    return std::unexpected(std::move(artifacts.error()));
  }

  ManifestV2 manifest{
      .container_id = std::move(*container_id),
      .checkpoint_id = std::move(*checkpoint_id),
      .generation = *generation,
      .created_at_unix_nanoseconds = *created_at,
      .container_cpus = std::move(*container_cpus),
      .source_image_identity = std::move(*source_image),
      .runtime_fingerprint = std::move(*runtime),
      .artifacts = std::move(*artifacts),
  };
  if (auto validation = validate_manifest_v2(manifest); !validation) {
    return std::unexpected(std::move(validation.error()));
  }

  auto checksum_text = read_string(document.at("checksum"), "checksum");
  if (!checksum_text) {
    return std::unexpected(std::move(checksum_text.error()));
  }
  auto checksum = hash_from_hex(*checksum_text, "checksum");
  if (!checksum) {
    return std::unexpected(std::move(checksum.error()));
  }

  auto payload_text = dump_canonical_json(manifest_payload_json(manifest));
  if (!payload_text) {
    return std::unexpected(std::move(payload_text.error()));
  }
  const auto expected_checksum = blake3_hash(as_bytes(*payload_text));
  if (*checksum != expected_checksum) {
    return std::unexpected(
        make_error(ManifestErrorCode::checksum_mismatch, "checksum",
                   "manifest checksum does not match its canonical payload"));
  }

  auto canonical_text = encode_manifest_text(manifest);
  if (!canonical_text) {
    return std::unexpected(std::move(canonical_text.error()));
  }
  if (*canonical_text != text) {
    return std::unexpected(
        make_error(ManifestErrorCode::non_canonical_encoding, "manifest",
                   "manifest JSON is valid but not in canonical v2 form"));
  }

  return manifest;
}

} // namespace gravesentinel::ccm
