# Checkpoint retention, garbage collection, and startup recovery

## Scope

Stage 11 maintenance is implemented by `//src/core:checkpoint_maintenance`,
`CheckpointService`, and `FilesystemStorage`. It has three entry paths:

1. post-commit retention runs immediately inside a successful create transaction;
2. registered containers receive background retention retries and garbage collection on
   their existing checkpoint worker;
3. storage startup recovery removes narrowly recognized stale files and runs bounded
   garbage collection before the service, operation journal, and IPC readiness exist.

Immediate retention is part of its journaled create, but its completion flag is not
exposed over IPC. Separate background retention/GC passes and startup recovery are
internal daemon work: they are not submitted through the public IPC API and are not
represented in the public `OperationStore` journal or by `GetOperation`.
Operation-history eviction and administrative pruning remain unimplemented and are not
part of checkpoint retention.

## Lease and orchestration boundary

`CheckpointMaintenance` is separate from create and restore orchestration. It acquires a
lease-only `IStorageMaintenanceOperation` through
`IStorage::begin_maintenance(container, retention|garbage_collection)`. This path does
not allocate an `OperationId`, operation staging, `state`, `runtime-artifact/`, or a
manifest pin.

`FilesystemStorage` uses the same nonblocking exclusive per-container `flock` for
create, restore, retention, garbage collection, and startup recovery. Therefore only
one of those activities can inspect or mutate a container namespace at a time, even
across hypervisor processes. A restore pin shares the operation state and may outlive
the operation owner; while it does, it keeps the lease and maintenance is deferred with
a retryable lock conflict.

## Fail-closed root discovery

Retention and garbage collection start with the same strict manifest-root scan. Before
any manifest or chunk deletion, storage:

- examines every entry under `manifests/`;
- accepts only a canonical `<20-digit-positive-generation>.json` name or the exact
  hidden publication-temporary form `.tmp-<32-lowercase-hex>`;
- reads and canonically decodes every named manifest, including its checksum;
- verifies that the manifest `container_id` matches the namespace identity and that its
  body generation matches its filename;
- builds the complete set of referenced chunks and rejects conflicting sizes for the
  same hash;
- verifies that every referenced chunk exists as a regular file and has the exact size
  recorded by the manifest.

An unknown manifest-root entry, unreadable or corrupt canonical manifest, namespace or
generation mismatch, missing referenced chunk, or wrong-sized referenced chunk fails
the maintenance pass. Because this scan completes before deletion, retention removes
no manifest and garbage collection performs no chunk sweep on such a failure. This is
intentional: an uncertain root set must never be converted into data loss.

## Retention

### Immediate post-commit retention

`CheckpointCreator` passes the registration's validated `keep_last` value (`1..3`) into
the create request. After the new manifest publication is durably confirmed, and while
the create lease is still held, the creator calls retention before staging cleanup.
The strict scan sorts generations newest-to-oldest. Before deleting any root,
retention durably removes candidate records for every currently referenced chunk;
this prevents a marker whose earlier removal had an indeterminate directory `fsync`
from surviving a re-reference and shortening the next unreferenced grace period.
Deletion then proceeds oldest-first until only `keep_last` manifests remain. Each
manifest unlink is followed by an `fsync` of `manifests/` before the deletion is
counted as successful.

The manifest publication is the create commit point. A retention error or exception
cannot revoke that commit and does not turn the create into failure. The rich
`CreatedCheckpoint` result instead returns `retention_complete = false`; staging cleanup
is still attempted. `CheckpointService` treats the create as successful and schedules
a separate retention pass when that flag is false.

### Separate retention passes

A retry uses `CheckpointMaintenance::apply_retention`, acquires a lease-only retention
operation, and reruns the complete strict scan. The current filesystem backend normally
reports either a complete pass or an error; it retains the `RetentionReport::complete`
contract so an incomplete pass can also be retried safely.

Removing a manifest does not directly remove chunks. A chunk referenced by several
manifests remains live until retention has removed the final referencing root and a
later garbage-collection grace period has elapsed.

## Chunk mark-and-sweep

Garbage collection is conservative two-phase mark-and-sweep scoped to one container.
After strict root discovery it scans canonical chunk paths and maintains durable marker
files under:

```text
containers/<container-key>/gc-candidates/<64-lowercase-hex-chunk-hash>
```

A candidate record is newline-terminated and has this exact field order:

```text
format_version=1
chunk=<64-lowercase-hex-chunk-hash>
first_observed_unix_nanoseconds=<signed-decimal>
device=<unsigned-decimal>
inode=<unsigned-decimal>
size=<unsigned-decimal>
mtime_seconds=<signed-decimal>
mtime_nanoseconds=<unsigned-decimal>
ctime_seconds=<signed-decimal>
ctime_nanoseconds=<unsigned-decimal>
```

The first pass that observes an unreferenced chunk only publishes its candidate record;
it does not delete that chunk. The persisted first-observed timestamp starts the grace
period, and the device, inode, size, modification time, and status-change time bind the
marker to the observed file identity. `CheckpointMaintenance` defaults to a one-hour
grace period and at most 128 chunk deletions per pass. The production daemon exposes
that grace as `--gc-grace-seconds N` (default `3600`); it is an operator-controlled
startup setting, not a scheduler request field. The stage-12 manual E2E uses `1` so it
can exercise both candidate marking and a later deletion without waiting an hour.

Every pass first `fsync`s `gc-candidates/` before trusting its visible marker set,
so an indeterminate marker publication/removal cannot be silently treated as
confirmed within the running filesystem state. On a later pass:

- a marker is durably removed if its chunk is live again or the chunk no longer exists;
- an identity change removes the old marker and starts a new grace period with a fresh
  marker;
- once grace has elapsed, storage reruns the complete strict manifest-root scan
  immediately before considering that chunk for unlink;
- storage then reopens the chunk and requires device, inode, size, `mtime`, and `ctime`
  to equal the candidate record;
- only then is the chunk unlinked and its parent prefix directory `fsync`ed; the marker
  is subsequently removed durably from `gc-candidates/`.

If the deletion limit is reached while eligible chunks remain, the report sets
`more_work = true`. The service coalesces another GC pass. These rules ensure that a
shared chunk remains live until the final manifest root disappears and that a replaced
or modified file is never deleted using stale candidate evidence.

## Startup recovery

The production process acquires `ServerLock`, creates/opens checkpoint storage, and
then calls `FilesystemStorage::recover`. Recovery must complete before
`CheckpointService`, `OperationStore`, the RPC handler, and the readiness/listening
message are created. Consequently, no registered work can race startup recovery in the
normal composition.

Production startup recovery uses the same configured `--gc-grace-seconds` value
(default one hour) and a maximum of 128 chunk deletions per container per pass. Recovery
takes the same per-container lease and:

- removes only canonical operation-key staging directories whose modification time is
  stale;
- removes only exact `.tmp-<32-lowercase-hex>` regular publication temporaries whose
  modification time is stale, in the recognized container, manifest, candidate,
  staging, and chunk-prefix locations;
- runs the same strict, bounded chunk GC used by maintenance;
- repeats bounded passes while `more_work` is reported, up to the process startup bound
  of 1024 passes.

A live lease defers the namespace; the production startup treats any such deferred
container as startup failure rather than opening the service concurrently. There is no
startup retention pass: registrations have not been loaded, so their `keep_last`
values are unavailable.

Recovery deliberately leaves uncertain names alone. A canonical container-key
directory without an `identity` file is treated as a partial namespace and is
untouched. An identity entry that exists but is unreadable or invalid fails recovery. Noncanonical container, staging, chunk, candidate, and temporary names are
also untouched. This tolerance does not weaken the strict manifest-root rule for a
complete identified namespace: an unknown entry in that `manifests/` directory makes
its GC pass and startup recovery fail closed.

## Service scheduling

Maintenance reuses the existing maximum of four per-registration workers. It executes
on the same worker thread already bound to the registration's `checkpoint_cpu`; there
is no fifth CPU-heavy worker or separate maintenance pool.

Within a slot, selection priority is:

1. manual foreground create or restore;
2. periodic create;
3. retention work/retry;
4. garbage collection.

Maintenance uses coalesced pending flags rather than an unbounded queue. A successful
create requests GC and, when immediate retention was incomplete, retention. Periodic GC
is due every five minutes. Retryable maintenance errors, all maintenance exceptions,
and an incomplete retention report schedule a retry after 30 seconds. A non-retryable
failure is logged but is not timer-retried; it waits for a relevant new trigger: a
later create for retention, or a later create/periodic five-minute tick for GC. `more_work` schedules another coalesced GC pass.

Foreground items and waiters suppress selection/execution of periodic and maintenance
work. A due periodic create may already be coalesced as pending, but it cannot run ahead
of foreground work. Under a continuous foreground backlog, maintenance may therefore
wait until the slot becomes quiescent; no fairness deadline overrides foreground
priority.

An active maintenance pass makes `unregister_container` return a retryable conflict.
When unregister succeeds, it discards queued periodic and maintenance flags before
joining the idle worker. Shutdown similarly drops queued maintenance and periodic work,
while an already active bounded pass is allowed to finish before the worker is joined.

## Validation and known limits

The focused maintenance and integration-with-storage/service targets are:

```text
bazel test //tests/unit:checkpoint_maintenance_test
bazel test //tests/unit:filesystem_storage_test
bazel test //tests/unit:checkpoint_service_test
bazel test //tests/unit:checkpoint_creator_test
```

The aggregate commands remain `bazel test //:checks` and `bazel test //...`.
Sanitizer configurations are available as documented in the [index](README.md), but no
sanitizer execution is claimed here.

Maintenance does not solve operation-history eviction/pruning. The registration-level
runtime fingerprint is still not forwarded into create or restore decisions. Unit and
component coverage also does not establish that the complete production
daemon-to-Podman/CRIU flow or its maintenance behavior has been exercised end to end in
a qualified rootful environment.
