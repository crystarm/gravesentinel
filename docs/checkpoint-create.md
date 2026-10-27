# Crash-safe checkpoint creation transaction

## Scope

`//src/core:checkpoint_creator` is the synchronous create orchestration layer. It
connects `IPodmanAdapter`, `ISnapshotChunker`, Manifest v2, and `IStorage`. It owns
post-commit immediate retention under the create lease, but not periodic scheduling,
restore selection, separate maintenance retries, operation-journal recovery, or
garbage collection. The production
`gravesentinel_hypervisor` composition instantiates this layer through
`CheckpointOperations` and `CheckpointService`, with asynchronous IPC submission
provided by the outer durable operation store. See [Checkpoint service
core](service-core.md) for that boundary.

A create request carries the already assigned `RequestId` and `OperationId`, the
logical `ContainerId`, the stable runtime reference, and the runtime/checkpoint
CPU assignment. In the production IPC path, `SubmitCreateCheckpoint` supplies the
request ID and the operation store deterministically derives the operation ID before
the synchronous service call. A direct successful `CheckpointCreator::create`
result identifies the immutable `CheckpointId`, its per-container generation, the
captured runtime incarnation, whether post-commit retention completed, and whether
staging cleanup was confirmed. The `ICheckpointService::create_checkpoint` facade
deliberately returns only
`CheckpointResult<void>` and retains the source incarnation internally; the IPC
operation status likewise does not expose checkpoint identity, generation, or the
retention/staging-completion flags.

## State machine

Each invocation owns local state; the `CheckpointCreator` object does not store
a shared current state and can therefore be used by `CheckpointService` to run
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
Create, restore, retention, garbage collection, and startup cleanup through that
backend therefore cannot overlap for one logical container, while different containers have
independent leases. An arbitrary `IStorage` implementation must provide equivalent
semantics for this orchestration guarantee to hold.

Only after acquiring the lease does the creator:

1. request a fresh `CheckpointId` from `IIdGenerator`;
2. list committed manifests;
3. compute `max(committed generation) + 1` with overflow rejection.

The creator and synchronous service do not themselves own a request journal or
idempotency table. A direct in-process caller owns `RequestId` and `OperationId`
uniqueness. `FilesystemStorage` rejects an `OperationId` while an incomplete staging
directory with that ID still exists, rather than resuming it, but neither the creator
nor the service rejects an ID merely because it was used by an earlier operation
whose staging was removed.

The production IPC boundary is stronger: its operation store durably deduplicates a
journaled `RequestId` with the same create/container payload and reuses the same
operation ID across restart. A submission with a fresh request ID is intentionally a
new capture and receives a fresh checkpoint ID.

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
lexically normalized regular file below the dedicated directory. The production
storage, adapter, and chunker additionally bind and re-check directory/file identities;
the abstract
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

After every artifact has been chunked, the creator builds canonical Manifest v2
with:

- logical container and fresh checkpoint identities;
- monotonic generation and capture timestamp;
- the exact canonical `container_cpus` assigned to the captured runtime;
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

Only confirmed manifest publication transitions to `COMMITTED`. The creator then
calls `apply_retention(request.keep_last)` on the still-active create operation before
staging cleanup. `keep_last` is validated as `1..3`; storage strictly validates all
manifest roots and referenced chunk sizes, then removes excess manifests oldest-first.
The create lease remains held for publication, retention, and cleanup, so restore and
separate maintenance cannot overlap this sequence.

The manifest publication remains the commit point. A retention failure, incomplete
report, or exception cannot cancel the committed create. The rich result returns
`retention_complete = false`, and the service schedules a lease-only retention retry;
the IPC operation itself still succeeds because the checkpoint commit succeeded.

## Failures, cleanup, and crash behavior

On every ordinary path after `begin_operation`, the creator explicitly calls
`IStorageOperation::cleanup` while the lease is still owned. A primary capture,
chunking, serialization, or commit error is preserved; a cleanup error is added
to the diagnostic and changes the terminal transaction state to `INCOMPLETE`.
The operation object is then destroyed and the lease is released.

After confirmed manifest commit, neither retention nor cleanup failure can turn the
create result into failure: the checkpoint is already immutable and visible. Retention
failure or exception is logged and leaves `retention_complete = false`; cleanup is still
attempted. Cleanup failure leaves `staging_cleanup_complete = false` and stale staging
for the conservative startup recovery path. Returning failure for either post-commit
maintenance action would invite a caller to create an unnecessary duplicate checkpoint.

If the process crashes before manifest publication, operation staging may remain
but is never returned by committed-manifest listing and is never used for restore.
The creator has no resume path; fully published chunks can be deduplicated by a later
attempt. A direct caller would use a new operation/checkpoint ID. In the production
IPC path, however, a crash after the durable `running` transition is first classified
as `indeterminate` and must be reconciled before any new attempt.

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

## IPC submission and crash recovery

Create is exposed as a bounded asynchronous operation. The server durably appends and
`fsync`s a `queued` journal record before acknowledging `SubmitCreateCheckpoint`, and
a worker durably records `running` before entering
`ICheckpointService::create_checkpoint`. `GetOperation` then exposes the terminal
`succeeded`, `failed`, or `indeterminate` state. `SchedulerClient` also provides a
synchronous `bool` wrapper that performs submit plus polling up to its operation
timeout; a transport disconnect does not cancel accepted work.

The operation ID is `op-` plus the BLAKE3 digest of the request ID. Repeating the same
request ID with the same canonical method/container payload returns the original
operation without another capture, including after restart. Reusing it with a
different payload is rejected. Journal compaction durably keeps one current record
per operation, preserving this deduplication history.

Crash recovery never silently reruns accepted work. A record still `queued` becomes a
terminal failed-before-execution operation. A record still `running` becomes
`indeterminate`, because capture or manifest publication may already have happened.
The executor also maps a core error carrying any storage/runtime side-effect flag to
`indeterminate`; this is deliberately conservative even when the only possible
publication was an orphan chunk rather than a manifest. Scheduler/operator
reconciliation must inspect committed manifests and relevant runtime state before a
new destructive request.

The operation registry is bounded and has no eviction policy. When its record limit is
reached, new unique create/restore submissions are rejected even if retained records
are terminal; known duplicate submissions and status polling continue to work.
Operation-history pruning remains unimplemented. Checkpoint retention, garbage
collection, and startup stale-staging cleanup are implemented as internal work and do
not add public operation-journal records.

## Verification

`//tests/unit:checkpoint_creator_test` uses fake runtime/storage/clock/ID/logger
collaborators and deterministic chunk data. It verifies the exact state machine,
lease-bound generation selection, metadata, manifest-last order, immediate retention,
post-commit retention failure/exception handling, retry and chunk reuse, independent
containers, and faults in capture, source read, hash, chunk publication, manifest
serialization/commit, and cleanup. Every failed attempt is
checked against three pre-existing canonical checkpoints whose referenced chunks
are reassembled byte-for-byte with the production reassembler.

The lower-level production storage, maintenance layer, chunker, Manifest v2 codec, and
Podman adapter retain their own unit and opt-in rootful integration tests. Focused
maintenance coverage includes `//tests/unit:checkpoint_maintenance_test`,
`//tests/unit:filesystem_storage_test`, and `//tests/unit:checkpoint_service_test`.
`CheckpointCreator` tests cover the synchronous creator separately. Operation-store, RPC-handler, and
server/client tests cover asynchronous submission, deduplication, and polling with
fake executors or services; no test wires the real `CheckpointCreator` through the
operation store and IPC stack. The Podman adapter capture test does obtain its artifact
directory from a real `FilesystemStorage` operation, but this still does not establish
that the complete production daemon-to-Podman/CRIU flow was executed end to end. The
destructive runtime path still requires the qualified rootful environment. Creator tests also do not cover every possible collaborator
contract violation, such as a foreign pre-populated correlation ID or a mismatched
source-incarnation reference.
