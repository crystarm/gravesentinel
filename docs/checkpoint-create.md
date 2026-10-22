# Crash-safe checkpoint creation transaction

## Scope

`//src/core:checkpoint_creator` is the stage-07 synchronous orchestration layer.
It connects `IPodmanAdapter`, `ISnapshotChunker`, Manifest v1, and `IStorage`
without adding periodic scheduling, restore selection, retention, startup recovery,
or garbage collection. It is implemented and unit-tested as a library, but the
current `gravesentinel_hypervisor` binary is still a stub and does not instantiate
this layer or a production service composition. See [Checkpoint service
core](service-core.md) for the current wiring boundary.

A create request carries the already assigned `RequestId` and `OperationId`, the
logical `ContainerId`, the stable runtime reference, and the runtime/checkpoint
CPU assignment. A direct successful `CheckpointCreator::create` result identifies
the immutable `CheckpointId`, its per-container generation, the captured runtime
incarnation, and whether post-commit staging cleanup was confirmed. The
`ICheckpointService::create_checkpoint` facade deliberately returns only
`CheckpointResult<void>` and retains the source incarnation internally; it does
not expose the checkpoint identity, generation, or cleanup-completion flag.

## State machine

Each invocation owns local state; the `CheckpointCreator` object does not store
a shared current state and can therefore be used by a later service core to run
independent containers concurrently when its collaborators are thread-safe.
The transaction transitions are:

```text
IDLE -> CAPTURING -> CHUNKING -> COMMITTING -> COMMITTED
```

A definite pre-commit failure transitions from the current phase to `FAILED`.
The terminal state is `INCOMPLETE` when staging cleanup fails or manifest
publication reports `StorageSideEffect::publication_may_be_visible`. Every
transition is emitted through `ILogger` with container, request, operation,
checkpoint/generation when assigned, and the stable error code and diagnostic on
failure. The creator does not intentionally add artifact contents or secret values
to logs, but it logs collaborator-provided `CheckpointErrorDetail::diagnostic`
text verbatim. Storage, runtime, chunker, ID-generator, and other injected
implementations must therefore sanitize diagnostics and must not include artifact
contents, credentials, or environment values.

## Lease, identity, and generation

Creation begins with
`IStorage::begin_operation(..., StorageOperationKind::create_checkpoint)`. With
production `FilesystemStorage`, the returned move-only operation owns an exclusive
cross-process per-container storage lease until the operation object is destroyed.
Create, restore, retention, and conflicting cleanup through that backend therefore
cannot overlap for one logical container, while different containers have
independent leases. An arbitrary `IStorage` implementation must provide equivalent
semantics for this orchestration guarantee to hold.

Only after acquiring the lease does the creator:

1. request a fresh `CheckpointId` from `IIdGenerator`;
2. list committed manifests;
3. compute `max(committed generation) + 1` with overflow rejection.

The core has no request journal, duplicate-ID table, or idempotency mechanism.
Callers own `RequestId` and `OperationId` uniqueness and must use fresh values for
a retry. `FilesystemStorage` rejects an `OperationId` while an incomplete staging
directory with that ID still exists, rather than resuming it, but neither the
creator nor the service rejects an ID merely because it was used by an earlier
operation whose staging was removed. A fresh retry receives a new checkpoint ID
and performs a new runtime capture.

A definitely uncommitted failed attempt does not reserve its generation because
generation ordering is defined only by manifests returned by committed-manifest
listing. An indeterminate manifest publication is the exception: if a rename
became visible before durability confirmation failed, that manifest can appear in
a later listing and cause the next attempt to select the following generation.

## Capture and chunk publication

`FilesystemStorage` creates an empty inode-bound
`staging/<operation>/runtime-artifact/` directory, separate from storage-owned
`state` and pin metadata. The creator passes only this directory to
`IPodmanAdapter::capture`. The production Podman adapter exports a complete
opaque checkpoint with `--leave-running`, so the source container resumes after
the runtime capture boundary. The returned artifact must be an absolute,
lexically normalized regular file below the dedicated directory. The production storage, adapter, and
chunker additionally bind and re-check directory/file identities; the abstract
`IPodmanAdapter` contract alone does not provide that filesystem containment
guarantee.

After capture, the creator records the wall-clock timestamp for diagnostics and
passes the complete dedicated directory to `ISnapshotChunker`. The production
chunker reads the immutable artifact tree, computes fallible BLAKE3 hashes,
validates existing chunks before reuse, and publishes missing chunks through the
same lock-bound storage operation. A hash, source read, existing-chunk read, or
chunk publication error ends this attempt; already published chunks remain
valid orphan candidates that a later attempt may reuse. Even a
`publication_may_be_visible` chunk error is a definite create failure rather than
an indeterminate checkpoint: without a manifest, no checkpoint was committed.

The artifact is never re-captured or continued within a failed operation.

## Manifest-last commit

After every artifact has been chunked, the creator builds canonical Manifest v1
with:

- logical container and fresh checkpoint identities;
- monotonic generation and capture timestamp;
- source image identity and strict runtime fingerprint returned by the adapter;
- ordered artifact metadata and chunk references returned by the chunker.

The canonical manifest is serialized before any manifest publication call. The
creator itself makes a single `IStorageOperation::publish_manifest` call;
that call is the commit point and is strictly the last publication made by the
creator. The production `SnapshotChunker` publishes only chunks, and
`FilesystemStorage` rejects further chunk publication after manifest publication
succeeds or reaches an indeterminate rename-completed outcome. A custom
`ISnapshotChunker` receives the full `IStorageOperation` interface, so it must obey
the same manifest-last rule; the type system does not restrict it to chunk-only
methods.

Only confirmed manifest publication transitions to `COMMITTED`. Previous
manifests are not removed or modified by this stage. Retention is deferred to
stage 11.

## Failures, cleanup, and crash behavior

On every ordinary path after `begin_operation`, the creator explicitly calls
`IStorageOperation::cleanup` while the lease is still owned. A primary capture,
chunking, serialization, or commit error is preserved; a cleanup error is added
to the diagnostic and changes the terminal transaction state to `INCOMPLETE`.
The operation object is then destroyed and the lease is released.

After confirmed manifest commit, cleanup failure cannot turn the create result
into failure: the checkpoint is already immutable and visible. The creator
returns success with `staging_cleanup_complete = false` and logs the cleanup
reason, allowing conservative startup recovery/GC to handle the stale staging
in stage 11. Returning failure here would invite a caller to create an
unnecessary duplicate checkpoint.

If the process crashes before manifest publication, operation staging may remain
but is never returned by committed-manifest listing and is never used for
restore. A retry uses a fresh operation/checkpoint ID and artifact, while any
fully published chunks can be deduplicated.

Filesystem publication has one unavoidable indeterminate boundary. If the
manifest rename completed but the manifests directory `fsync` failed, storage
returns `publication_may_be_visible`: the file may already be observable, but
its crash durability was not confirmed. The creator reports failure and
`INCOMPLETE`; it does not claim that no manifest is visible and does not unlink a
possibly observed immutable manifest. Existing committed checkpoints remain
valid. A later recovery layer must reconcile this outcome.

`StorageSideEffect::publication_may_be_visible` describes a storage side effect,
not by itself a committed-checkpoint outcome. The same flag can accompany a chunk
publication failure; in that case the creator reports definite checkpoint failure
and logs `FAILED`, because no manifest was committed, while preserving the flag to
report that an orphan chunk may be visible.

## Verification

`//tests/unit:checkpoint_creator_test` uses fake runtime/storage/clock/ID/logger
collaborators and deterministic chunk data. It verifies the exact state machine,
lease-bound generation selection, metadata, manifest-last order, retry and chunk
reuse, independent containers, and faults in capture, source read, hash, chunk
publication, manifest serialization/commit, and cleanup. Every failed attempt is
checked against three pre-existing canonical checkpoints whose referenced chunks
are reassembled byte-for-byte with the production reassembler.

The lower-level production storage, chunker, Manifest v1 codec, and Podman adapter
retain their own unit and opt-in rootful integration tests. These tests do not
constitute production service wiring: there is currently no end-to-end binary
composition of `CheckpointService`, `CheckpointCreator`, `FilesystemStorage`, and
`PodmanAdapter`. The creator tests also do not cover every possible collaborator
contract violation, such as a foreign pre-populated correlation ID or a mismatched
source-incarnation reference.
