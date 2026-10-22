# Crash-safe FilesystemStorage

## Scope and support profile

`//src/storage:filesystem_storage` is the stage-04 durable backend for CCM. It
implements storage transactions, immutable file publication, bounded reads,
per-container locks, operation staging, and manifest pins. Chunking, chunk hash
verification, create/restore orchestration, retention, and garbage collection
remain separate stages.

The supported MVP profile is a local Linux filesystem that provides:

- atomic same-filesystem `renameat2(RENAME_NOREPLACE)` or hard-link publication;
- working file and directory `fsync`;
- `flock` locks shared by all hypervisor processes using the storage root;
- accessible Linux `/proc/self/fd` for reopening an already type-checked inode;
- normal POSIX behavior for `openat`, `mkdirat`, `unlinkat`, and regular files.

NFS, FUSE, overlay mounts, or filesystems that reject directory `fsync` are not
silently treated as durable. A directory-sync failure is an operation failure.
The configured root must already exist, be absolute, and be trusted by the
hypervisor administrator. Its final component is opened with `O_NOFOLLOW`.

## Layout and path mapping

The on-disk layout is internal and versioned by its metadata rather than exposed
to Scheduler:

```text
root/
  containers/
    <container-key>/
      identity
      manifests/
        <20-digit-generation>.json
      chunks/
        <two-hex-prefix>/<64-hex-blake3>
      staging/
        <operation-key>/
          state
          pin-<20-digit-generation>
  locks/
    <container-key>.lock
  tmp/
```

`container-key` and `operation-key` are lowercase hex BLAKE3 values over
separate domain tags and the original identifier bytes. Raw identifiers are
never path components, so values containing `/`, `.` or `..` cannot escape the
root. The immutable `identity` file binds a container key back to the logical
`ContainerId` and makes a key mismatch fail closed. Identifiers used by storage
are limited to 256 bytes.

Manifest names contain only their fixed-width decimal generation. This permits
newest-to-oldest listing even when a manifest body is corrupted and must be
rejected later by the strict Manifest v1 decoder. A successful publication
requires a canonical, checksum-valid Manifest v1 whose container and checkpoint
identities match the operation. A generation must be greater than every
previously committed generation. Reads additionally bind the body container and
generation back to the requested storage reference. A corrupt old body remains
a listed fallback candidate and does not block publication of a newer
generation; valid old bodies are still checked for namespace binding and
CheckpointId reuse. Published manifests are never replaced or removed by this
stage.

## Directory-descriptor boundary

After opening the trusted root, storage operations are relative to already-open
directory descriptors. Components are opened with `O_NOFOLLOW`. A final object
is first opened as `O_PATH`, checked as a regular file with `fstat`, then reopened
through its stable `/proc/self/fd` handle and checked again for the same inode.
A FIFO, device, socket, directory, or symlink is therefore rejected before it is
opened for data I/O. Reads use recorded-size limits, retry `EINTR`, handle short
reads, reject early EOF, and perform an extra EOF read to detect concurrent
growth.

Storage-generated names are single safe components. The implementation does not
use `weakly_canonical`, an `exists`-then-open check, or raw identifier path
concatenation. The absolute staging path exposed by `IStorageOperation` is for
the trusted runtime adapter; all storage-owned metadata remains accessed through
the descriptor boundary.

## Durable immutable publication

Chunks, manifests, container identity, operation state, and pin metadata use the
same publication primitive:

1. generate a 128-bit `getrandom` nonce and create the corresponding unique
   hidden file in the destination directory with
   `O_CREAT | O_EXCL | O_NOFOLLOW` and mode `0600`;
2. write all bytes, retrying `EINTR` and continuing after short writes;
3. verify regular-file type and exact size with `fstat`;
4. `fsync` the temporary file;
5. publish without replacement using `renameat2(RENAME_NOREPLACE)`, with a
   same-filesystem `linkat` fallback when that syscall/flag is unavailable;
6. `fsync` the destination directory;
7. return success only after step 6.

Before step 5, directory listing ignores the hidden temporary file. After step
5, readers can observe only the complete file. If step 6 fails, the call returns
an error with
`StorageSideEffect::publication_may_be_visible`: the new immutable file may
already be visible, but durability was not confirmed. It is not unlinked as a
rollback because readers may already have observed it and rollback itself could
fail. A later orchestration layer must classify this outcome as indeterminate.
Read/list/reuse paths and an idempotent publication retry re-`fsync` the relevant
directory before reporting the visible object as durable, providing an explicit
reconciliation path.

Creating every new directory is followed by `fsync` of its parent. Existing
directories are also re-synced before use, so retry after a failed parent sync
cannot convert a merely visible directory entry into false durable success. The
random temporary namespace does not repeat after process restart; stale hidden
temporary files remain ignored until conservative GC.

## Locks, staging, and pins

`begin_operation` obtains a nonblocking exclusive `flock` on the permanent
per-container lock file. The lock file is never removed, avoiding split-brain
locks on different inodes. Create, restore, retention, and conflicting cleanup
are therefore serialized across both threads and hypervisor processes. A lock
is held for the lifetime of the move-only `IStorageOperation` and any pin that
outlives its operation owner.

Each operation must use a fresh `OperationId`. It creates a unique staging
directory and durably publishes non-secret `state` metadata. Reusing an ID whose
staging directory survived a crash returns `conflict`; mutable snapshots are
never resumed implicitly.

`pin_manifest` first proves that the selected generation exists, then durably
publishes pin metadata. The returned RAII pin removes and syncs that metadata on
release. Explicit operation cleanup is rejected while pins remain. Normal
cleanup recursively removes staging without following symlinks and syncs the
staging parent. If cleanup fails or the process crashes, the staging directory
remains incomplete and is ignored by committed-manifest listing. Conservative
stale cleanup and GC policy are intentionally deferred to stage 11.

## Internal API and errors

`IStorage` owns read-only namespace operations and starts an exclusive
`IStorageOperation`. Publication is available only through that operation, so a
caller cannot bypass the required lease:

- `begin_operation(container, operation, kind)`;
- `contains_chunk` and bounded `read_chunk`;
- `list_manifests`, returning generations in descending order;
- bounded `read_manifest(container, generation)`.

`IStorageOperation` provides the staging path, immutable chunk publication,
final manifest publication, manifest pinning, and cleanup. Once manifest
publication succeeds—or reaches the rename-but-directory-fsync indeterminate
state—the operation rejects further chunk or manifest publication.

Filesystem errors are mapped to stable internal codes: space exhaustion,
permission denial, not found, conflict, or generic storage failure. Diagnostics
include the failed phase and `errno` text but no payload, environment value, or
cold-start secret.

## Verification boundary

Unit tests use a fault-injecting `IFileOperations` decorator over real POSIX
temporary directories. They cover open/create, partial and interrupted
read/write, file `fsync`, no-replace rename, directory `fsync`, `ENOSPC`,
permission errors, re-confirmation after failed directory sync, stale temporary
files and staging after reopen, opaque mapping of traversal-like IDs, symlink and
FIFO substitution, missing mandatory namespace entries, generation/body
binding, tolerance of an older corrupt manifest, immutable chunks, and RAII
pins. These tests require neither root, Podman, nor CRIU.
