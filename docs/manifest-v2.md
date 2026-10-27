# Checkpoint Manifest v2

## Status and scope

This document is the normative persisted-format contract for Checkpoint Manifest
v2. The implementation lives in `//src/manifest:manifest`; the production BLAKE3
wrapper is `//src/crypto:blake3`, and `//src/core:blake3_hasher` adapts it to
`IHasher`.

Manifest v2 describes an immutable checkpoint as an ordered set of regular-file
artifacts and their ordered fixed-size chunks. It does not publish files, read
chunks, follow filesystem links, invoke Podman, or decide runtime compatibility.
Those operations belong to [filesystem storage](filesystem-storage.md),
[fixed-size chunking and reassembly](chunking.md), and adapter stages.

## Data model

A `ManifestV2` contains:

| Field | Representation and meaning |
| --- | --- |
| `format_version` | Unsigned integer `2`; encoded by the codec rather than stored as mutable model state |
| `container_id` | Logical scheduler `ContainerId`, never a transient Podman container ID |
| `checkpoint_id` | Unique ID of the immutable checkpoint |
| `generation` | Unsigned per-container ordering value; must be greater than zero |
| `created_at_unix_nanoseconds` | Unsigned nanoseconds since the Unix epoch; diagnostic metadata, not version ordering |
| `container_cpus` | Nonempty canonical sorted set of unique logical CPU IDs in the inclusive range `0..7`; the exact source container cpuset required for restore |
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

Manifest v2 persists these exact non-secret strings:

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

Manifest v2 uses a narrow project-defined canonical JSON profile, not a claim of
full RFC 8785 conformance. The encoder uses the repository-pinned
`nlohmann_json` implementation with its ordered object representation and strict
UTF-8 serializer. The following rules are part of the persisted v2 format:

1. The document is UTF-8 without BOM, comments, control characters, trailing
   bytes, or trailing newline.
2. There is no insignificant whitespace.
3. Object keys are emitted in the exact sequences specified below. These
   sequences are the lexicographic order produced by the pinned implementation,
   but consumers must treat the explicit sequences, rather than a dependency's
   future ordering behavior, as the v2 contract.
4. The root and every nested object contain exactly the fields defined by v2;
   missing, unknown, or duplicate keys are rejected.
5. Versions, timestamps, modes, and sizes are unsigned decimal integer literals.
   Negative, floating-point, exponent, or out-of-range alternatives are rejected.
6. Hashes and checksums contain exactly 64 lowercase hexadecimal characters.
7. Artifact and chunk arrays preserve model order. The codec never sorts either
   array.
8. Strings are valid UTF-8 and are not Unicode-normalized. Byte-distinct strings
   remain distinct identifiers. The serializer emits non-ASCII UTF-8 directly
   and uses its canonical compact escaping for JSON syntax characters.
9. The decoder re-encodes the validated model and requires byte-for-byte equality
   with the input. Pretty-printed JSON, alternate key order, alternate escapes,
   and otherwise equivalent noncanonical documents are rejected.

The complete top-level object uses this exact key order:

```text
artifacts
checkpoint_id
checksum
container_cpus
container_id
created_at_unix_nanoseconds
format_version
generation
runtime_fingerprint
source_image_identity
```

The checksum payload uses the same order with `checksum` omitted. Each artifact
object uses `chunks`, `mode`, `path`, `size`; each chunk object uses `hash`,
`size`. The runtime fingerprint object uses this exact order:

```text
apparmor_profile
architecture
capability_profile
cgroup_controllers
cgroup_manager
cgroup_version
command_profile
criu_version
kernel_release
oci_runtime_commit
oci_runtime_features
oci_runtime_name
oci_runtime_version
privilege_mode
runtime_build_origin
runtime_name
runtime_version
seccomp_profile
selinux_mode
user_namespace_mode
```

### Decoder acceptance order

`decode_manifest_v2` applies checks in this order:

1. reject an empty encoding or an encoding larger than 16 MiB;
2. run the bounded SAX preflight pass;
3. build the JSON DOM and require one root object;
4. require and parse `format_version`, reject unsupported versions, then require
   the exact v2 root key set;
5. parse the exact nested schemas and unsigned/string field types;
6. construct the model and run all Manifest v2 semantic validation;
7. parse the checksum, reconstruct the canonical payload, and compare BLAKE3-256;
8. encode the complete canonical document and require byte-for-byte equality with
   the original input.

Consequently, a schema or semantic error can be reported before checksum
validation. A semantically valid document with a matching checksum but
noncanonical whitespace, key order, or escapes reaches the final comparison and
is rejected as `non_canonical_encoding`. A checksum mismatch is rejected before
such a document could be accepted, but checksum checking is not the first decode
step.

Changing this schema, key set or order, integer representation, checksum
algorithm, or canonicalization rules requires a new manifest version. Manifest v1
snapshots are intentionally incompatible with the current decoder and command profile;
a migration must be added explicitly rather than inferred during restore.

## Checksum

Encoding is deterministic:

1. Construct the canonical JSON object containing every field except
   `checksum`.
2. Serialize that payload using the canonical profile.
3. Compute unkeyed BLAKE3-256 over the exact serialized payload bytes.
4. Insert the lowercase hex digest as `checksum`.
5. Serialize the complete object canonically.

The checksum protects every semantic field, including `container_cpus`, and both
array orders. The decoder
reconstructs the payload, recomputes BLAKE3, and compares the digest before
accepting the document. Restore additionally requires `container_cpus` to match the
requested runtime cpuset exactly and rejects a mismatch before runtime mutation. This
is corruption detection, not authentication or a MAC; authenticity must come from the
trusted storage/process boundary.

## Validation invariants

The model and decoder enforce all of the following:

- `generation > 0`;
- `container_cpus` is nonempty, sorted, duplicate-free, and contains only logical CPU IDs `0..7`; the `CpuSet` value type canonicalizes valid caller input before it reaches the manifest model;
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

The v2 parser applies these limits before accepting untrusted input:

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

The encoded-size limit is applied before parsing. A SAX preflight pass then
enforces nesting, node, object-field, array, duplicate-key, artifact-count, and
aggregate chunk-count limits before the DOM parser allocates the full JSON tree.
For scalar JSON string values, the preflight applies the 4096-byte relative-path
ceiling as a general early bound; the typed parse later applies the smaller
field-specific limits from the table.

The current SAX implementation does not apply that 4096-byte per-string bound to
object keys. A key is still bounded by the 16 MiB encoded-document limit and the
32-fields-per-object limit, and every accepted object must later match an exact
small v2 key set. This is an implementation resource-bound limitation, not a
permission for extensions or oversized keys in an accepted manifest.

The typed parse enforces the exact schema, numeric ranges, aggregate chunk count,
total sizes, implied directory count, and path component depth before
constructing the accepted model.

## Path safety boundary

`RelativeSnapshotPath` accepts only already-canonical relative POSIX paths. It
rejects empty values, absolute paths, Windows drive prefixes and backslashes,
NUL/control bytes, invalid UTF-8, empty components, `.`/`..`, repeated or trailing
separators, and overlong components/paths. It never normalizes an unsafe value
into a different accepted value.

`join_within_root` performs a lexical join beneath an absolute root and returns a
normalized path. That is not proof of physical filesystem containment: a path
component can later be replaced with a symlink. Filesystem users must open
relative to trusted directory descriptors using Linux no-follow semantics and
must verify actual file types with `fstat`; `weakly_canonical` or a
check-then-open path comparison is insufficient because of TOCTOU races.

The concrete boundaries are documented separately. The
[chunker and reassembler](chunking.md#filesystem-and-mount-boundary) additionally
require Linux mount IDs and reject mount crossings. The
[FilesystemStorage descriptor boundary](filesystem-storage.md#directory-descriptor-boundary)
uses no-follow descriptor-relative access but does not currently enforce mount-ID
containment inside the administrator-trusted storage root.

Manifest-declared mode validation and filesystem no-follow validation are both
required: the former rejects malicious metadata, while the latter rejects a
host filesystem that changed after parsing.

## API and errors

- `validate_manifest_v2` validates an in-memory model without serializing it.
- `encode_manifest_v2` validates and returns canonical bytes with a derived
  checksum.
- `decode_manifest_v2` treats bytes as untrusted input and returns only a fully
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
