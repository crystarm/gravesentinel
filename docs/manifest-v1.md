# Checkpoint Manifest v1

## Status and scope

This document is the normative persisted-format contract for Checkpoint Manifest
v1. The implementation lives in `//src/manifest:manifest`; the production BLAKE3
wrapper is `//src/crypto:blake3`, and `//src/core:blake3_hasher` adapts it to
`IHasher`.

Manifest v1 describes an immutable checkpoint as an ordered set of regular-file
artifacts and their ordered fixed-size chunks. It does not publish files, read
chunks, follow filesystem links, invoke Podman, or decide runtime compatibility.
Those operations belong to later storage, chunking, and adapter stages.

## Data model

A `ManifestV1` contains:

| Field | Representation and meaning |
| --- | --- |
| `format_version` | Unsigned integer `1`; encoded by the codec rather than stored as mutable model state |
| `container_id` | Logical scheduler `ContainerId`, never a transient Podman container ID |
| `checkpoint_id` | Unique ID of the immutable checkpoint |
| `generation` | Unsigned per-container ordering value; must be greater than zero |
| `created_at_unix_nanoseconds` | Unsigned nanoseconds since the Unix epoch; diagnostic metadata, not version ordering |
| `source_image_identity` | Non-secret immutable image reference, digest, or runtime image identity |
| `runtime_fingerprint` | Exact runtime/host compatibility inputs listed below |
| `artifacts` | Ordered array of regular-file artifact descriptors |
| `checksum` | Derived BLAKE3-256 checksum of the canonical payload without this field |

Every artifact contains:

- canonical `RelativeSnapshotPath` in `path`;
- unsigned byte size in `size`;
- full POSIX regular-file mode in `mode`, including the `S_IFREG` type bits;
- an ordered `chunks` array.

Every chunk contains a 32-byte BLAKE3 digest encoded as exactly 64 lowercase hex
characters and an unsigned byte `size`.

The array order is semantic. Neither artifacts nor chunks are sorted by the
codec. Generation, not wall-clock time or array position in a directory listing,
defines newest/oldest checkpoint order. Storage must enforce monotonic generation
when publishing successive manifests for one `ContainerId`.

## Runtime fingerprint

Manifest v1 persists these exact non-secret strings:

- `runtime_name`, `runtime_version`, `runtime_build_origin`;
- `architecture`, `kernel_release`, `criu_version`;
- `oci_runtime_name`, `oci_runtime_version`, `oci_runtime_commit`,
  `oci_runtime_features`;
- `cgroup_version`, `cgroup_manager`, `cgroup_controllers`;
- `privilege_mode`, `user_namespace_mode`, `capability_profile`;
- `selinux_mode`, `apparmor_profile`, `seccomp_profile`;
- `command_profile`.

Producers must use stable canonical strings. Unordered feature/controller sets
must be normalized into a documented order before constructing the fingerprint.
A later adapter may define compatibility rules, but an unknown field value or
profile must fail closed rather than attempt an unsafe restore. Source image
identity is deliberately separate from the runtime fingerprint.

No environment value, secret, credential-bearing host path, or arbitrary command
stderr belongs in either field.

## Canonical JSON profile

Manifest v1 uses a narrow project-defined canonical JSON profile, not a claim of
full RFC 8785 conformance:

1. UTF-8 without BOM, comments, control characters, trailing bytes, or trailing
   newline.
2. No insignificant whitespace.
3. Object keys are emitted in lexicographic order by the pinned
   `nlohmann_json` implementation.
4. The root and every nested object contain exactly the fields defined by v1;
   missing, unknown, or duplicate keys are rejected.
5. Versions, timestamps, modes, and sizes are unsigned decimal integer literals.
   Negative, floating-point, exponent, or out-of-range alternatives are rejected.
6. Hashes and checksums contain exactly 64 lowercase hexadecimal characters.
7. Artifact and chunk arrays preserve model order.
8. Strings are valid UTF-8 and are not Unicode-normalized. Byte-distinct strings
   remain distinct identifiers.
9. The decoder re-encodes the validated model and requires byte-for-byte equality
   with the input. Pretty-printed JSON, alternate key order, alternate escapes,
   and otherwise equivalent noncanonical documents are rejected.

Changing this schema, key set, integer representation, checksum algorithm, or
canonicalization rules requires a new manifest version.

## Checksum

Encoding is deterministic:

1. Construct the canonical JSON object containing every field except
   `checksum`.
2. Serialize that payload using the canonical profile.
3. Compute unkeyed BLAKE3-256 over the exact serialized payload bytes.
4. Insert the lowercase hex digest as `checksum`.
5. Serialize the complete object canonically.

The checksum protects every semantic field and both array orders. The decoder
reconstructs the payload, recomputes BLAKE3, and compares the digest before
accepting the document. This is corruption detection, not authentication or a
MAC; authenticity must come from the trusted storage/process boundary.

## Validation invariants

The model and decoder enforce all of the following:

- `generation > 0`;
- artifact paths are unique, and no file path may be the parent prefix of another
  file path (for example, `a` and `a/b` conflict);
- only regular-file modes are accepted; symlink, directory, device, socket, and
  FIFO mode declarations are rejected;
- an empty artifact has zero chunks;
- a nonempty artifact has at least one chunk;
- each non-final chunk is exactly 4 MiB;
- the final chunk is in `1..4 MiB`;
- the checked sum of chunk sizes equals the artifact size;
- checked sums do not overflow and total artifact bytes stay within the snapshot
  limit;
- all metadata strings are nonempty, bounded, valid UTF-8, and contain no control
  characters;
- unknown or malformed format versions are rejected;
- checksum mismatch is rejected before a noncanonical document can be accepted.

## Resource limits

The v1 parser applies these limits before accepting untrusted input:

| Resource | Limit |
| --- | ---: |
| Encoded manifest | 16 MiB |
| JSON nesting | 8 container levels |
| JSON object fields | 32 |
| JSON values/nodes | 550000 |
| Artifacts | 4096 |
| Directories implied by artifact paths, including root | 4096 |
| Components in one artifact path | 256 |
| Chunks across the manifest | 131072 |
| Chunk size | 4 MiB |
| Total artifact bytes | 1 TiB |
| Container/checkpoint ID | 256 UTF-8 bytes |
| Source image identity | 2048 UTF-8 bytes |
| Each runtime fingerprint field | 2048 UTF-8 bytes |
| Relative artifact path | 4096 UTF-8 bytes |
| One path component | 255 UTF-8 bytes |

A SAX preflight pass enforces nesting, node, object, array, duplicate-key, and
string limits before the DOM parser allocates the full JSON tree. The typed parse
then enforces the exact schema, numeric ranges, aggregate chunk count, and total
sizes, implied directory count, and path component depth before constructing the
accepted model.

## Path safety boundary

`RelativeSnapshotPath` accepts only already-canonical relative POSIX paths. It
rejects empty values, absolute paths, Windows drive prefixes and backslashes,
NUL/control bytes, invalid UTF-8, empty components, `.`/`..`, repeated or trailing
separators, and overlong components/paths. It never normalizes an unsafe value
into a different accepted value.

`join_within_root` performs a lexical join beneath an absolute root and returns a
normalized path. That is not proof of physical filesystem containment: a path
component can later be replaced with a symlink. Storage and chunking code must
open relative to a trusted directory descriptor using Linux no-follow/beneath
semantics, such as `openat2` with `RESOLVE_BENEATH`, `RESOLVE_NO_SYMLINKS`, and
`RESOLVE_NO_MAGICLINKS`, or an equivalent component-by-component `openat` design.
It must also verify the actual file type with `fstat`. `weakly_canonical` or a
check-then-open path comparison is insufficient because of TOCTOU races.

Manifest-declared mode validation and filesystem no-follow validation are both
required: the former rejects malicious metadata, while the latter rejects a
host filesystem that changed after parsing.

## API and errors

- `validate_manifest_v1` validates an in-memory model without serializing it.
- `encode_manifest_v1` validates and returns canonical bytes with a derived
  checksum.
- `decode_manifest_v1` treats bytes as untrusted input and returns only a fully
  validated canonical model.
- `ManifestResult<T>` is `std::expected<T, ManifestError>` with stable structured
  `ManifestErrorCode`, field path, and internal diagnostic.
- `blake3_hash` computes the production 32-byte digest; `Blake3Hasher` exposes the
  same implementation as a successful `CheckpointResult<ChunkHash>` through the
  fallible `IHasher` contract.

Codec errors are converted to the public `StableErrorCode` only at a higher-level
core boundary. A malformed stored manifest normally becomes
`corrupted_snapshot`; allocation or process-level failure is not disguised as a
recoverable format error.
