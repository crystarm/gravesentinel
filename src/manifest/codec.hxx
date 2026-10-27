#pragma once

#include "src/manifest/types.hxx"

#include <cstddef>
#include <span>
#include <vector>

namespace gravesentinel::ccm {

using EncodedManifest = std::vector<std::byte>;

[[nodiscard]] ManifestResult<EncodedManifest>
encode_manifest_v2(const ManifestV2 &manifest);

[[nodiscard]] ManifestResult<ManifestV2>
decode_manifest_v2(std::span<const std::byte> encoded);

} // namespace gravesentinel::ccm
