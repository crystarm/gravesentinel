#pragma once

#include "src/manifest/types.hxx"

#include <cstddef>
#include <span>
#include <vector>

namespace gravesentinel::ccm {

using EncodedManifest = std::vector<std::byte>;

[[nodiscard]] ManifestResult<EncodedManifest>
encode_manifest_v1(const ManifestV1 &manifest);

[[nodiscard]] ManifestResult<ManifestV1>
decode_manifest_v1(std::span<const std::byte> encoded);

} // namespace gravesentinel::ccm
