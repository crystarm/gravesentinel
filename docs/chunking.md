# Fixed-size snapshot chunking and reassembly

## Scope

`//src/chunking:snapshot_chunker` implements the stage-05 transformation between
an immutable snapshot tree and the artifact/chunk section of Manifest v1. It does
not capture a runtime checkpoint, commit a manifest, select a restore candidate,
or invoke the runtime adapter.

The supported tree consists only of regular files. File bytes, relative paths,
and full regular-file POSIX modes are preserved. Directories are structural only:
they are recreated with mode `0700`, while empty directories, directory modes,
ownership, ACLs, xattrs, capabilities, and timestamps are not represented by
Manifest v1. The runtime adapter must expose an opaque export using this supported
profile; the usual MVP form is one regular archive artifact.

## Ingest contract

`SnapshotChunker` is bound to an `IStorageOperation` of kind
`create_checkpoint`. The operation supplies the `ContainerId`, preventing reads
from one container namespace and publication into another. `ingest()` returns an
ordered `std::vector<ManifestArtifactV1>` and never publishes a manifest.

The producer must keep the source tree immutable for the entire call. Descriptor
and metadata checks detect ordinary contract violations, but they are not a
replacement for a filesystem snapshot against a privileged malicious writer.
Published chunks from an ingest that later fails are harmless orphan candidates;
without a published manifest they do not make a committed checkpoint.

Traversal is deterministic:

1. open the absolute root with Linux `openat2` and
   `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`, then open every descendant
   relative to an already-open directory descriptor without following it;
2. require Linux `statx` mount IDs and reject a descendant on another mount,
   including bind mounts;
3. sort each directory's entries by byte order and visit depth-first;
4. validate every entry as a canonical `RelativeSnapshotPath`;
5. sort the resulting artifact descriptors globally by relative path.

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

For each source block, the chunker computes production BLAKE3-256 and calls
`IStorageOperation::read_chunk`. Read and publication therefore use one
operation-bound backend, namespace, and per-container lock capability; a caller
cannot accidentally read from one storage root and publish to another. An existing chunk
is reusable only when all of these match the source block:

- exact byte size;
- BLAKE3 digest;
- byte-for-byte contents.

A missing chunk is atomically published through `IStorageOperation::publish_chunk`.
A concurrent no-replace race is reconciled by the storage implementation. A
corrupted object under the expected hash is never considered deduplicated and is
not overwritten: ingest fails with `corrupted_snapshot`. Replacing such an
immutable object could silently alter every committed manifest that references
it, so repair/quarantine requires a separate future storage operation under the
per-container lock.

Deduplication is deliberately limited to one `ContainerId`. Identical blocks in
different container namespaces are independently published and may occupy
separate physical files.

## Reassembly contract

`SnapshotReassembler` is bound to an `IStorageOperation` of kind `restore`. It
first validates the complete artifact vector, including count/size limits, chunk
layout, duplicate paths, modes, and file-vs-parent conflicts such as `a` and
`a/b`. Invalid metadata causes no output filesystem side effect.

After validation it reopens the operation staging path, requires its device/inode
to match the identity captured by storage, and creates a fresh exclusive `snapshot`
directory inside it. The storage-owned `state` and pin metadata therefore
never appear in the runtime artifact tree. Intermediate directories and files are
created relative to checked directory descriptors; unexpected existing entries,
symlinks, special files, hardlinks, and mount crossings are rejected.

Every referenced chunk is read in the operation's container namespace. Its exact
manifest size and BLAKE3 are checked before any of its bytes are written. Output
handles partial writes and `EINTR`; each completed file is checked for exact size,
regular-file type, single link, expected mode, and mount containment, then
`fsync`ed. A partial current file is unlinked on failure. Before return, staging
and `snapshot` are rebound by pathname and must still identify the checked
inodes. The caller receives the `.../staging/<operation>/snapshot` path only after
every artifact succeeds, so a
runtime adapter cannot be handed a partially validated tree through this API.

## Resource bounds and errors

Payload buffering is independent of artifact or snapshot size. Ingest holds one
source chunk and, while validating deduplication, at most one stored chunk;
reassembly holds one stored chunk. Artifact and chunk descriptor memory is bounded
by Manifest v1 limits. A directory listing refuses more than the combined
artifact/directory budget before sorting. Both ingest and manifest validation cap
the whole tree at 4096 directories including the root and 256 path components.

Errors use `CheckpointResult<T>`. Unsafe or changing source trees report
`capture_failed`; malformed stored layouts and missing/hash-mismatched chunks
report `corrupted_snapshot`; permission, capacity, storage, conflict, and
availability failures retain their stable internal codes. Diagnostics contain
phases and safe relative context, never artifact payload or cold-start secrets.
