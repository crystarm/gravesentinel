# Crash-safe FilesystemStorage

## Scope and support profile

`//src/storage:filesystem_storage` is the durable filesystem backend for CCM. It
implements storage transactions, immutable file publication, bounded reads,
per-container locks, operation staging, runtime artifact directories, and
manifest pins. [Manifest encoding and validation](manifest-v1.md),
[chunking, chunk hash verification, and reassembly](chunking.md), and the
implemented create/restore orchestration remain separate layers. The backend also
implements strict retention, candidate-based chunk garbage collection, and conservative
startup recovery; scheduling policy belongs to `CheckpointService` and
[`CheckpointMaintenance`](maintenance.md).

The supported MVP profile is a local Linux filesystem that provides:

- atomic same-filesystem `renameat2(RENAME_NOREPLACE)` or hard-link publication;
- working file and directory `fsync`;
- `flock` locks shared by all hypervisor processes using the storage root;
- Linux `openat2` with `RESOLVE_NO_SYMLINKS`/`RESOLVE_NO_MAGICLINKS`;
- accessible Linux `/proc/self/fd` for reopening an already type-checked inode;
- for callers using the same file-operations adapter for snapshot ingest or
  reassembly, Linux `statx` mount IDs as required by the
  [chunking-layer mount boundary](chunking.md#filesystem-and-mount-boundary);
- normal POSIX behavior for `openat`, `mkdirat`, `unlinkat`, and regular files.

NFS, FUSE, overlay mounts, or filesystems that reject directory `fsync` are not
silently treated as durable. A directory-sync failure is an operation failure.
The configured root must already exist, be absolute, and be trusted by the
hypervisor administrator. The production backend opens the absolute path with
`openat2(RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS)`, which rejects symlinks in
any component and is stronger than checking only the final component. The public
`FilesystemStorage::open` validation does not currently reject an embedded NUL in
the C++ `filesystem::path`; callers must not supply one, because a POSIX syscall
would interpret only the prefix before that byte while the retained path object
would still contain the suffix.

`FilesystemStorage` itself does not require or compare mount IDs for directories
beneath that root and therefore does not reject a nested bind mount or another
mount crossing in `containers/`, `locks/`, or their descendants. Mount-ID
containment is enforced by `SnapshotChunker` and `SnapshotReassembler`, not by the
storage namespace implementation. The administrator must keep the trusted storage
layout free of unintended nested mounts. This is an implementation boundary, not
a storage guarantee.

## Layout and path mapping

The on-disk layout is internal and versioned by its metadata rather than exposed
to Scheduler. The complete current layout includes storage-owned entries,
algorithm-owned reassembly output, and possible hidden publication temporaries:

```text
root/
  containers/
    <container-key>/
      identity
      .tmp-<32-lowercase-hex>                 # transient or stale identity temp
      manifests/
        <20-digit-generation>.json
        .tmp-<32-lowercase-hex>               # transient or stale manifest temp
      gc-candidates/
        <64-lowercase-hex-blake3>             # persisted GC candidate record
        .tmp-<32-lowercase-hex>               # transient or stale candidate temp
      chunks/
        <two-hex-prefix>/
          <64-lowercase-hex-blake3>
          .tmp-<32-lowercase-hex>             # transient or stale chunk temp
      staging/
        <operation-key>/
          state
          pin-<20-digit-generation>
          runtime-artifact/
            ... runtime-produced entries ...
          snapshot/                            # default reassembly output
            ... reassembled regular files ...
          <caller-selected-output>/            # e.g. snapshot-9
            ... reassembled regular files ...
          .tmp-<32-lowercase-hex>              # transient state or pin temp
  locks/
    <container-key>.lock
  tmp/                                         # reserved; currently unused
```

Only `state`, pin files, and hidden publication temporaries are storage metadata
inside an operation directory. `runtime-artifact/` is the dedicated create-side
runtime export directory. `snapshot/` and a strict caller-selected
`[A-Za-z0-9_-]+` alternative are created by the
[reassembler](chunking.md#reassembly-contract) as siblings, not children, of
`runtime-artifact/`. Failed operations may leave partial contents in staging;
committed-manifest listing never traverses staging.

The root-level `tmp/` directory is created, opened, and retained by
`FilesystemStorage::open`, but the current implementation does not use it.
Immutable publication creates its hidden temporary in the final destination
directory so the no-replace operation is necessarily same-filesystem. GC state lives
under each container's `gc-candidates/`; consumers must not infer a scratch-file
contract from the reserved root `tmp/` entry.

### Derived keys

Keys are unkeyed BLAKE3-256 digests encoded as exactly 64 lowercase hexadecimal
characters. The byte derivations are exact:

```text
container-key = hex(BLAKE3("container" || 0x00 || ContainerId bytes))
operation-key = hex(BLAKE3("operation" || 0x00 || OperationId bytes))
```

The domain strings contain no terminator other than the single explicit `0x00`
separator shown above. Identifiers must contain 1 through 256 bytes and must not
contain NUL. Storage does not require identifier bytes to be UTF-8; the stricter
[Manifest v1 text rules](manifest-v1.md#validation-invariants) apply before an ID
can be persisted in a manifest. Raw identifiers are never path components, so
values containing `/`, `.` or `..` cannot escape the root.

The immutable `identity` file contains exactly the original `ContainerId` bytes,
with no header or newline. It binds a container key back to the logical identity
and makes a key collision or mismatch fail closed with `conflict`. During initial
container creation, `manifests/`, `chunks/`, and `staging/` are made durable before
`identity` is published last as the namespace completion marker. A crash before
that publication leaves a partial namespace that startup recovery preserves and a
later operation can safely complete. Once `identity` exists, every required
subdirectory must already exist and is opened rather than recreated, so lost
metadata fails closed instead of being mistaken for a new empty namespace. There
is no corresponding operation identity file: a surviving directory under the
derived operation key makes reuse fail closed with `conflict`.

### Metadata files

A newly initialized operation publishes `state` with this exact newline-terminated
format:

```text
format_version=1
operation_key=<64-lowercase-hex-operation-key>
kind=<create_checkpoint|restore>
state=active
```

A manifest pin is named `pin-<20-digit-generation>` and contains:

```text
format_version=1
generation=<unpadded-positive-decimal-generation>
```

The pin body is newline-terminated. Maintenance uses no staging or `state` file;
`begin_maintenance` returns only a lease-bound maintenance object.

A GC candidate is named by the exact 64-lowercase-hex chunk hash and contains the
newline-terminated version, chunk name, first-observed-unreferenced Unix nanoseconds,
and the observed device, inode, size, `mtime` seconds/nanoseconds, and `ctime`
seconds/nanoseconds. The normative field order is documented in
[maintenance.md](maintenance.md#chunk-mark-and-sweep).

The lock file is an empty regular file named
`<container-key>.lock`. Storage-created regular files request mode `0600`, and
storage-created directories request mode `0700`; POSIX `openat`/`mkdirat` apply
the process `umask`, which may remove bits. The implementation does not promise
exact generated modes under an arbitrary `umask`.

### Manifests

Manifest names contain only their zero-padded 20-digit decimal generation and the
`.json` suffix. Ordinary restore/create listing ignores other names and leaves body
validation to the caller, which permits newest-to-oldest fallback even when an older
body is corrupt. Retention and GC use a stricter root scan: every entry must be a
canonical positive-generation manifest or the exact `.tmp-<32-lowercase-hex>`
publication temporary; every manifest is decoded and checked against the namespace and
filename generation before any deletion.
A regular file with a valid manifest name but a size greater than 16 MiB causes
listing to fail as corrupted storage rather than being silently skipped.

A successful publication requires a canonical, checksum-valid Manifest v1 whose
container and checkpoint identities match the operation. A generation must be
greater than every previously committed generation name, including a generation
whose body is no longer decodable. Reads additionally bind the body container and
generation back to the requested storage reference. A bounded regular old body
that is malformed, noncanonical, or checksum-invalid remains a listed fallback
candidate and does not block publication of a strictly newer generation; valid
old bodies are still checked for namespace binding and `CheckpointId` reuse.
Published manifests are never replaced. Retention durably removes excess manifests
oldest-first only after the complete strict root and referenced-chunk scan succeeds.

## Directory-descriptor boundary

After opening the trusted root, storage operations are relative to already-open
directory descriptors. Generated child names are single components, and child
directories are opened with `O_NOFOLLOW`. A final object is first opened as
`O_PATH`, checked as a regular file with `fstat`, then reopened through its stable
`/proc/self/fd` handle and checked again for the same device and inode. A FIFO,
device, socket, directory, or symlink is therefore rejected before it is opened
for data I/O. Reads use recorded-size limits, retry `EINTR`, handle short reads,
reject early EOF, and perform an extra EOF read to detect concurrent growth.

These checks prevent symlink traversal and final-object type substitution, but
they do not establish mount containment. Storage directory status may contain an
optional `statx` mount ID, yet `FilesystemStorage` does not compare it when
opening the container layout. This differs from the explicit mount-ID checks in
the [chunker and reassembler](chunking.md#filesystem-and-mount-boundary).

Storage-generated names are single safe components. The implementation does not
use `weakly_canonical`, an `exists`-then-open check, or raw identifier path
concatenation. The absolute staging and `runtime-artifact` paths exposed by
`IStorageOperation` are for the trusted runtime adapter; all storage-owned
metadata remains accessed through the descriptor boundary. Storage retains open
directory descriptors for both paths and captures each path's device/inode
identity before returning the operation.

## Durable immutable publication

Chunks, manifests, GC candidate records, container identity, operation state, and pin
metadata use the same publication primitive:

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

Chunk publication first reads an existing final name. If it is absent, storage
uses the no-replace primitive. When that primitive loses with `already_exists`,
storage rereads the winning object: equal bytes are accepted only after directory
`fsync`. If the bytes differ, or the reread fails, the current implementation
returns the original publication failure, normally `already_exists`; it does not
remap that race outcome to `corrupted_snapshot` or to the reread error. The
winning object is never overwritten. A differing object found by the initial
pre-publication read does produce `corrupted_snapshot`. The per-container lock
prevents a no-replace race between conforming operations, so this limitation is
principally relevant to external interference or nonconforming writers.

Every directory created or reopened through the storage `ensure_directory`
primitive is followed by `fsync` of its parent, so retry after a failed parent
sync cannot convert a merely visible directory entry into false durable success.
Public read/list paths and chunk reuse additionally sync the relevant data
directory before reporting durable success. This is not a guarantee that every
existing regular-file entry is re-synced on every open: in particular, reopening
an existing permanent lock file does not re-`fsync` the lock file or `locks/`.
The random temporary namespace does not repeat after process restart; exact stale
hidden publication temporaries become eligible for conservative startup recovery after
the configured grace period.

## Locks, staging, and pins

`begin_operation` and `begin_maintenance` obtain the same nonblocking exclusive
`flock` on the permanent per-container lock file; startup recovery takes that lock
directly. The lock file is never removed, avoiding split-brain locks on different
inodes. Create, restore, retention, GC, and recovery are therefore serialized across
both threads and hypervisor processes. A lock is held for the lifetime of the move-only
operation and any pin that outlives its operation owner. Such an outliving restore pin
keeps the shared operation state and lease alive, so maintenance is deferred.

Each create/restore operation must use a fresh `OperationId`. It creates a unique
staging directory and a durable, initially empty `runtime-artifact/` child dedicated to
runtime-produced snapshot data. Non-secret `state` metadata and pin files remain
in the operation staging root, outside `runtime-artifact/`. The runtime artifact
path and its captured device/inode identity are exposed separately so adapters
can verify that the directory they use is the one storage created. Reusing an ID
whose staging directory survived a crash returns `conflict`; mutable snapshots
are never resumed implicitly. Ordinary initialization failures after staging
creation attempt to close descriptors and remove the new tree without replacing
the primary error if cleanup also fails.

`pin_manifest` first proves that the selected generation exists, then durably
publishes pin metadata. The returned RAII pin removes and syncs that metadata on
release. Explicit operation cleanup is rejected while pins remain.

Normal cleanup closes the runtime artifact descriptor, recursively removes
operation contents without following symlinks, `fsync`s the operation directory,
closes its descriptor, removes the operation directory, and then `fsync`s the
staging parent. The per-container lease remains held until the operation object
is destroyed, or until an outliving pin releases its final shared operation
state.

Cleanup is not a transactional rollback. If recursive removal fails early, some
staging entries may remain. If removal of the operation directory fails after its
contents were removed, the now-empty directory may remain. If the
operation-directory unlink succeeds but the final parent `fsync` fails, the
pathname may already be absent even though deletion durability was not confirmed.
In the latter two post-close cases, `cleaned` remains false while the
operation-directory descriptor has already been closed; the current
implementation does not guarantee that a second `cleanup()` call can reconcile
the failure. Therefore callers must retain the cleanup error as the authoritative
outcome and must not assume either that staging remains present or that its
deletion is durable.

A process crash or any surviving staging entry is ignored by committed-manifest
listing. Startup recovery removes only canonical stale operation-key directories and
exact stale publication temporaries after its grace period while no live lease exists;
unknown names and a partial container namespace without `identity` remain untouched.

## Internal API and errors

`IStorage` owns read-only namespace operations and starts exclusive create/restore and
lease-only maintenance operations. Concrete `FilesystemStorage` additionally exposes
startup recovery. Publication is available only through a create/restore operation, so
a caller cannot bypass the required lease:

- `begin_operation(container, operation, create_checkpoint|restore)`, with staging;
- `begin_maintenance(container, retention|garbage_collection)`, without staging;
- `recover(options)` on concrete `FilesystemStorage`, before service startup;
- `contains_chunk` and bounded `read_chunk`;
- `list_manifests`, returning generations in descending order;
- bounded `read_manifest(container, generation)`.

`IStorageOperation` exposes its bound container identity and operation kind, the
staging path and its captured device/inode identity, and the dedicated runtime
artifact path and identity through `runtime_artifact_directory()` and
`runtime_artifact_directory_identity()`. It also provides lock-bound chunk
reads, immutable chunk publication, final manifest publication, manifest
pinning, and cleanup. Algorithm layers use the bound container identity rather
than a second caller-supplied `ContainerId`, preventing cross-namespace
read/publish mismatches. Once manifest publication succeeds—or reaches the
rename-but-directory-fsync indeterminate state—the operation rejects further
chunk or manifest publication. A confirmed create operation may then invoke immediate
retention under the same lease. `IStorageMaintenanceOperation` exposes only retention
or GC for its declared maintenance kind.

Filesystem errors are mapped to stable internal codes: `ENOSPC` and `EDQUOT` to
`resource_exhausted`; `EACCES`, `EPERM`, and `EROFS` to `permission_denied`;
`ENOENT` to `not_found`; `EEXIST` to `already_exists`; and `EAGAIN` or
`EWOULDBLOCK` to `conflict`. Other filesystem errors become `storage_error`.
Failures caused by `EINTR`, `EAGAIN`, `EBUSY`, or `EIO` are marked retryable when
they escape the local retry loops. Diagnostics include the failed phase and
`errno` text but no payload, environment value, or cold-start secret.

## Verification boundary

`//tests/unit:filesystem_storage_test` uses a fault-injecting `IFileOperations`
decorator over real POSIX temporary directories. It covers open/create, partial and
interrupted read/write, file `fsync`, no-replace rename, directory `fsync`, `ENOSPC`,
permission errors, re-confirmation after failed directory sync, canonical startup
cleanup, partial/unknown namespace preservation, strict zero-deletion failure,
oldest-first retention, lease-only maintenance exclusion, two-pass candidate GC,
identity/root revalidation, deletion bounds, shared chunks, opaque mapping of
traversal-like IDs, symlink and FIFO substitution, immutable chunks, dedicated runtime
artifact layout and identity, initialization rollback, ordinary cleanup, lease
lifetime, and RAII pins. These tests require neither root, Podman, nor CRIU.

The current unit suite does not establish mount-ID containment for storage,
post-unlink cleanup retry behavior, a differing winner in the chunk no-replace
race, exact generated directory modes under an arbitrary `umask`, embedded-NUL
root rejection, or execution of the `linkat` publication fallback. Those are
implementation limitations or untested paths described above, not guarantees
inferred from the covered cases.
