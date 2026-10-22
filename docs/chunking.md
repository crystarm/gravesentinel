# Fixed-size snapshot chunking and reassembly

## Scope

`//src/chunking:snapshot_chunker` implements the stage-05 transformation between
an immutable snapshot tree and the artifact/chunk section of
[Manifest v1](manifest-v1.md). It uses a lock-bound operation supplied by
[FilesystemStorage](filesystem-storage.md), but does not capture a runtime
checkpoint, commit a manifest, select a restore candidate, or invoke the runtime
adapter.

The supported tree consists only of regular files. File bytes, relative paths,
and full regular-file POSIX modes are preserved. Directories are structural only:
empty directories, directory modes, ownership, ACLs, xattrs, capabilities, and
timestamps are not represented by Manifest v1. Reassembly requests mode `0700`
when creating its output root and intermediate directories. The POSIX backend
uses `mkdirat`, so the process `umask` may remove permission bits; the
implementation does not apply a compensating directory `fchmod` and therefore
does not guarantee an exact resulting `0700` mode under an arbitrary `umask`.
The runtime adapter must expose an opaque export using this supported profile;
the usual MVP form is one regular archive artifact.

## Ingest contract

`ISnapshotChunker` is the orchestration seam used for fault injection.
`SnapshotChunker` implements it and retains only its `IHasher` and file-operations
dependencies. Each `ingest(IStorageOperation&, snapshot_root)` call receives an
operation of kind `create_checkpoint`; the same chunker instance can therefore be
used across operations without retaining operation state. The operation supplies
the `ContainerId`, preventing reads from one container namespace and publication
into another. `ingest()` returns an ordered
`std::vector<ManifestArtifactV1>` and never publishes a manifest. When the source
is the operation's dedicated `runtime-artifact` path, its opened device/inode must
match the identity captured by storage; replacing that directory between capture
and ingest fails before any source bytes are accepted.

The producer must keep the source tree immutable for the entire call. Descriptor
and metadata checks detect ordinary contract violations, but they are not a
replacement for a filesystem snapshot against a privileged malicious writer.
Published chunks from an ingest that later fails are harmless orphan candidates;
without a published manifest they do not make a committed checkpoint.

### Filesystem and mount boundary

Traversal is deterministic:

1. open the absolute root with Linux `openat2` and
   `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`, then open every descendant
   relative to an already-open directory descriptor without following it;
2. require Linux `statx` mount IDs and reject a descendant on another mount,
   including bind mounts;
3. sort each directory's entries by byte order and visit depth-first;
4. validate every entry as a canonical `RelativeSnapshotPath`;
5. sort the resulting artifact descriptors globally by relative path.

Mount-ID containment is an algorithm-layer guarantee of `SnapshotChunker` and
`SnapshotReassembler`. The shared POSIX file-operations adapter records an
optional mount ID, and these classes fail with `unavailable` when it is absent.
`FilesystemStorage` itself uses the same descriptor primitives but does not
require mount IDs or reject nested mount crossings inside its trusted root; see
its [directory-descriptor boundary](filesystem-storage.md#directory-descriptor-boundary).

Only regular files with `st_nlink == 1` are accepted. Symlinks, hardlinks,
devices, sockets, FIFOs, repeated directory identities, mount crossings, unsafe
names, excessive depth, and excessive directory counts fail closed. For every
file and directory, stable metadata is checked before and after traversal. A file
name is reopened relative to its original parent and must still resolve to the
same device, mount, inode, size, mode, link count, mtime, and ctime. After all
chunks are produced, a second descriptor-relative traversal compares the complete
set of entries and metadata again, then the root pathname is rebound to the same
identity. Reads retry `EINTR`, handle short reads, reject early EOF, and perform an
extra EOF probe to detect growth.

Each nonempty file is read through one reusable buffer of at most exactly 4 MiB.
The final block may be shorter; an empty file has no chunks. Manifest limits are
checked before reading a file so an oversized artifact cannot cause unbounded
orphan publication.

## Deduplication and immutable chunks

For each source block, the chunker requests a BLAKE3-256 digest from `IHasher`
and calls `IStorageOperation::read_chunk`. Production `Blake3Hasher` completes
this fallible contract with a successful result and remains `noexcept`. Any
injected hasher error is returned unchanged, and no publication follows that
failure. Read and publication therefore use one call-bound backend,
namespace, and per-container lock capability; a caller cannot accidentally read
from one storage root and publish to another. An existing chunk is reusable only
when all of these match the source block:

- exact byte size;
- BLAKE3 digest;
- byte-for-byte contents.

A missing chunk is atomically published through
`IStorageOperation::publish_chunk`. Before publication, an object already visible
under the expected hash is accepted only after size, BLAKE3, and byte equality;
a mismatch is not overwritten and ingest fails with `corrupted_snapshot`.
Replacing such an immutable object could silently alter every committed manifest
that references it, so repair or quarantine requires a separate future storage
operation under the per-container lock.

The current `FilesystemStorage` no-replace race handling is narrower. If final
publication reports `already_exists`, storage rereads the winning object. Equal
bytes are accepted only after a successful directory `fsync`. If the raced object
differs, or that reread fails, the current implementation returns the original
publication failure, normally `already_exists`, instead of remapping it to
`corrupted_snapshot` or the reread error. The object is still never overwritten.
The per-container lease prevents this race between conforming storage operations,
but callers must not rely on `corrupted_snapshot` specifically for interference
outside that lease boundary. See
[durable immutable publication](filesystem-storage.md#durable-immutable-publication).

Deduplication is deliberately limited to one `ContainerId`. Identical blocks in
different container namespaces are independently published and may occupy
separate physical files.

## Reassembly contract

`SnapshotReassembler` is bound to an `IStorageOperation` of kind `restore`. It
first validates the complete artifact vector, including count/size limits, chunk
layout, duplicate paths, modes, and file-vs-parent conflicts such as `a` and
`a/b`. Invalid metadata causes no output filesystem side effect.

After validation it reopens the operation staging path and requires its
device/inode to match the identity captured by storage. The default overload
creates a fresh exclusive `snapshot` directory; restore orchestration may supply a
strict single-component `[A-Za-z0-9_-]+` name such as `snapshot-9`, allowing one
unique output tree per fallback candidate under the same uninterrupted operation
lease. These output directories are siblings of the storage-owned `state`, pin
metadata, and `runtime-artifact/` directory in the
[operation staging layout](filesystem-storage.md#layout-and-path-mapping); none of
that metadata appears in the returned artifact tree. Intermediate directories
and files are created relative to checked directory descriptors; unexpected
existing entries, symlinks, special files, hardlinks, and mount crossings are
rejected. Directory creation requests mode `0700`, subject to the `umask`
limitation described in [Scope](#scope).

Every referenced chunk is read in the operation's container namespace. Its exact
manifest size and BLAKE3 are checked before any of its bytes are written. A
hasher failure is returned unchanged. Output handles partial writes and `EINTR`;
each completed file is checked for exact size, regular-file type, single link,
expected mode, and mount containment, then `fsync`ed.

After any failure that occurs after the current output file was created, the
implementation closes that file and attempts to unlink it. This unlink is
best-effort: `EINTR` is retried, but a final unlink error is not substituted for
the primary assembly error. A partial current file may therefore remain in the
private operation staging tree after an unlink failure. Previously completed
files and newly created directories are also retained; operation cleanup is the
outer reclamation boundary.

Before successful return, staging and the selected output directory are rebound
by pathname and must still identify the checked inodes. The caller receives the
output root path only after every artifact succeeds, so a runtime adapter cannot
be handed a partially validated tree through this API.

## Resource bounds and errors

Payload buffering is independent of artifact or snapshot size. Ingest holds one
source chunk and, while validating deduplication, at most one stored chunk;
reassembly holds one stored chunk. Artifact and chunk descriptor memory is bounded
by Manifest v1 limits. A directory listing refuses more than the combined
artifact/directory budget before sorting. Both ingest and manifest validation cap
the whole tree at 4096 directories including the root and 256 path components.

Errors use `CheckpointResult<T>`. Unsafe or changing source trees report
`capture_failed`; malformed manifest layouts and chunks found before publication
with missing, oversized, size-mismatched, or hash-mismatched content report
`corrupted_snapshot`; injected hasher errors are propagated without remapping.
Permission, capacity, storage, conflict, and availability failures retain their
stable internal codes. The no-replace race limitation described under
[Deduplication and immutable chunks](#deduplication-and-immutable-chunks) may
surface `already_exists` rather than `corrupted_snapshot`. Diagnostics contain
phases and safe relative context, never artifact payload or cold-start secrets.
