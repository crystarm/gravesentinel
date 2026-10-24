# Checkpoint service core: registration, scheduling, and CPU budget

## Scope and API boundary

`//src/core:checkpoint_service` owns registrations and dispatches synchronous
create/restore orchestration plus internal maintenance implemented by
[`CheckpointCreator`](checkpoint-create.md),
[`RestoreCoordinator`](checkpoint-restore.md), and
[`CheckpointMaintenance`](maintenance.md). The production
`gravesentinel_hypervisor` composition instantiates the service together with
filesystem storage, chunking, hashing, affinity, the Podman adapter, the durable
operation store, RPC handler, and Unix socket server.

`ICheckpointService` itself remains an in-process synchronous interface with
`register_container`, `unregister_container`, `create_checkpoint`,
`restore_or_restart`, and `shutdown`. Direct create/restore calls supply both
`RequestId` and `OperationId` and return `CheckpointResult<void>` only after a
terminal result. The core does not enforce global ID uniqueness or provide replay;
a direct retry with new IDs is a new operation.

The production RPC boundary deliberately adds different semantics around this core:

- `Register` and `Unregister` call the service synchronously. They are not journaled,
  and `SchedulerClient` does not transport-retry either mutation after an exchange
  may already have reached the server. A false/ambiguous result requires scheduler
  reconciliation of desired registration state.
- `SubmitCreateCheckpoint` and `SubmitRestoreOrRestart` durably enqueue work and
  return a deterministic `OperationId`; `GetOperation` polls `queued`, `running`,
  `succeeded`, `failed`, or `indeterminate`.
- the high-level synchronous client methods for create/restore are implemented as a
  bounded submit-and-poll loop and return `true` only after confirmed success.

The operation journal is outside `CheckpointService`. It deduplicates equal
create/restore submissions by `RequestId` and canonical method/container payload,
preserves results across restart, and passes its deterministic operation ID into the
synchronous service call. This distinction is important: calling the core directly
does not acquire IPC idempotency merely because the production daemon has it.

The direct creator and coordinator return rich `CreatedCheckpoint` and
`RestoredContainer` results. The service intentionally consumes those results: create
retains the source incarnation and uses `retention_complete` to schedule a retry;
restore retains the new incarnation. Neither the core facade nor wire status exposes
checkpoint identity/generation, checkpoint-versus-cold-start outcome, or
retention/pin/staging cleanup-completion flags. Background maintenance is not submitted
to or recorded by the public `OperationStore` journal.

`CheckpointOperations` is the production adapter from the service to the existing
synchronous creator, restore coordinator, and maintenance orchestrator. Tests replace
its `ICheckpointOperations` seam without constructing storage or Podman.

## `ContainerRegistration` wire schema

In protocol v2, the `Register` method carries the complete registration as its `params`
object. The schema is exact: all keys shown below are required, no extra keys are
accepted, and optional C++ values are encoded as `null` or a complete object.

```text
{
  "container_id": string,
  "runtime_container": string,
  "checkpoint": {
    "interval_seconds": integer,
    "keep_last": integer,
    "container_cpu": integer,
    "checkpoint_cpu": integer
  },
  "cold_start": null | {
    "image_reference": string,
    "argv": [string, ...],
    "environment": [
      {"name": string, "value": string}, ...
    ],
    "mounts": [
      {"source": string, "target": string, "read_only": boolean}, ...
    ],
    "runtime_options": [string, ...]
  },
  "runtime_fingerprint": null | {
    "runtime_name": string,
    "runtime_version": string,
    "runtime_build_origin": string,
    "architecture": string,
    "kernel_release": string,
    "criu_version": string,
    "oci_runtime_name": string,
    "oci_runtime_version": string,
    "oci_runtime_commit": string,
    "oci_runtime_features": string,
    "cgroup_version": string,
    "cgroup_manager": string,
    "cgroup_controllers": string,
    "privilege_mode": string,
    "user_namespace_mode": string,
    "capability_profile": string,
    "selinux_mode": string,
    "apparmor_profile": string,
    "seccomp_profile": string,
    "command_profile": string
  }
}
```

Wire identifiers are 1–128 bytes and use only ASCII letters, digits, `_`, `-`, `.`,
or `:`. `interval_seconds` is `1..31536000`, `keep_last` is `1..3`, and the CPU
values must form one of the standard pairs listed below. Cold-start arrays, strings,
environment names, and aggregate content are bounded by the codec; environment names
must be unique. A non-null runtime fingerprint must contain all 20 non-empty string
fields. Although `mounts` and `runtime_options` are part of the complete wire schema,
`podman-criu-rootful-v1` rejects non-empty values because those command forms are not
qualified.

## Registration and CPU validation

The registry contains at most four slots. Registration performs all of these checks
before it becomes visible:

1. the complete `ContainerRegistration` passes value/configuration validation;
2. the `ContainerId` is not already registered;
3. neither CPU is assigned to another live or retiring registration;
4. both `container_cpu` and `checkpoint_cpu` are online and present in the current
   allowed affinity mask according to `IAffinityController`;
5. the registration interval has a representable monotonic deadline;
6. the worker starts and successfully binds itself to `checkpoint_cpu`.

The standard accepted pairs remain `(0,1)`, `(2,3)`, `(4,5)`, and `(6,7)`. A slot
with active work rejects unregister with a retryable conflict and remains registered,
so a fifth registration cannot reuse its CPUs prematurely.

`CheckpointConfig::keep_last` is operational: its validated `1..3` value is passed to
every manual or periodic create for immediate post-commit retention and to separate
retention retries. Optional `ContainerRegistration::runtime_fingerprint` remains inert
after validation: it must be complete when present, but is not forwarded to create or
restore. Creation records the fingerprint returned by the runtime adapter, and restore
validates the fingerprint stored in the selected manifest.

The service schedules retention and bounded garbage collection for registered
containers. Conservative stale staging/publication cleanup occurs earlier in startup
storage recovery, before registrations exist.

## IPC durability, bounds, and recovery

The production daemon communicates with the privilege-free scheduler client using
one bounded JSON request and one response per `AF_UNIX/SOCK_SEQPACKET` exchange. Both
ends check `SO_PEERCRED`: the client trusts server UID 0 by default, while the server
accepts the configured scheduler UID (or its own effective UID when none is
configured). The socket GID is optional and its mode defaults to `0660`; filesystem
access and the exact peer-UID check both have to pass.

For create/restore, the default operation-store limits are 64 in-flight operations,
4096 retained records, at most 8192 append-log entries before the entry limit forces
compaction, and a 64 MiB journal. The store persists and `fsync`s acceptance as `queued` before
returning an operation ID, persists `running` before execution, and persists the
terminal state. The ID is `op-` plus the BLAKE3 digest of the request ID, making it
deterministic and restart-safe while its journal record is retained. Repeating the
same request ID and canonical payload returns the retained operation; a different
payload is rejected.

Compaction retains one current record per operation and durably replaces the journal;
it bounds transition history but does not evict deduplication records. Consequently,
once the 4096-record registry is full, new unique operations are rejected even if all
existing operations are terminal. Existing duplicates and status queries remain
available.

Before this journal is opened, the production process holds `ServerLock`, opens
`FilesystemStorage`, and completes bounded storage recovery. `CheckpointService` and
its registrations do not yet exist, so startup performs no retention because no
`keep_last` values are available.

At operation-journal startup, a record left `queued` by a crash becomes `failed` with an
interrupted-before-execution classification and is not run. A record left `running`
becomes `indeterminate`, because its create/restore side effects cannot be inferred
from the journal. `indeterminate` requires scheduler/operator reconciliation of
manifest publication and runtime presence; it is not permission to submit another
destructive attempt blindly.

## Worker and concurrency model

Each registered container owns one persistent worker, a foreground mailbox of
capacity one, a periodic-create slot, and coalesced retention/GC pending flags.
The worker binds its own thread to the registration's `checkpoint_cpu` before the
registration succeeds and attempts to restore its original affinity on exit. An
affinity-restore error is returned by unregister/shutdown (and cached by shutdown);
the destructor invokes shutdown but cannot report that error. The calling control
thread is never rebound. Capture, chunking, hashing, reassembly, runtime adapter calls,
retention, and garbage collection all execute inside that bound worker context.
Runtime subprocesses also receive the same checkpoint CPU through the existing adapter/process-runner
contract.

One worker per slot provides both limits without a global unbounded queue:

- no more than four container operations can execute simultaneously;
- create, restore, retention, and GC for one `ContainerId` are serialized;
- operations for different IDs can execute concurrently;
- maintenance reuses the same `checkpoint_cpu` affinity; there is no fifth CPU-heavy
  worker or separate maintenance pool.

A second synchronous caller for the same ID waits for the active operation to finish
before occupying the mailbox. Waiting callers are not queued in FIFO order. Once work
is selectable, priority is manual foreground, periodic create, retention, then GC;
foreground waiters suppress lower-priority admission. There is no fairness guarantee
among multiple manual callers. A closing registration wakes such
callers and rejects work that had not yet been accepted. Reentrant synchronous
service calls from the registration's own worker are rejected as conflicts rather
than self-deadlocking.

Accepted operations are not cancellable through this API and receive no service
stop token or service-level deadline. Subprocess adapters have their own timeouts,
but a blocked storage/runtime/custom collaborator can block its caller and shutdown
without a core-imposed bound. Unregister does not wait for active work: it returns a
retryable conflict so the scheduler can retry after the operation finishes.

After a successful create, the returned source `RuntimeIncarnation` is retained in
memory and supplied to the next restore as the exact source fence. A successful
restore replaces it with the restored incarnation. A failed restore clears this
hint, forcing the next attempt through the coordinator's conservative absence
proof. The service itself neither persists cold-start environment values nor adds
them to log fields; the collaborator-diagnostic constraint below still applies.

## Periodic creation and maintenance scheduling

The scheduler uses `IClock::monotonic_now()` and an injectable generation-based
`ITimer`. `SteadyTimer` is the production implementation. The generation parameter
prevents a registration, shutdown, or test-clock advance from being lost between
computing the earliest deadline and entering the timer wait.

The first periodic-create deadline is registration time plus `interval`. At a due
deadline the next deadline is set to `now + interval`. Deadline promotion and worker
selection share the slot mutex: before selecting maintenance, the worker also promotes
any deadline that is already due, so scheduler-thread timing cannot invert priority.
A periodic create is admitted only when no operation is already running and no create is
pending. A foreground item or waiter still executes first, while the admitted periodic
create remains coalesced behind it. A tick observed while an operation is already
running is skipped; multiple elapsed intervals never create a catch-up burst.

A successful create always coalesces a GC request. If immediate post-commit retention
returns `retention_complete = false`, it also coalesces a retention request.
Independently, GC is due every five minutes. Maintenance flags do not count occurrences
and therefore cannot form an unbounded backlog. `GarbageCollectionReport::more_work` coalesces another
pass.

An admitted periodic operation receives a locally constructed request ID of the
form `periodic-<container-id>-<slot-sequence>` and an `OperationId` from
`IIdGenerator`. The sequence advances only for admitted periodic work and resets
when a container is unregistered and registered again, so the periodic request ID
is not globally unique; the generated operation ID must provide operation
uniqueness. Periodic-create and maintenance completion have no caller-visible
operation-journal status; failures are sent only to the injected `ILogger`. In the
current production composition that logger writes to stderr. Neither periodic work nor background
maintenance is represented by a scheduler-submitted `GetOperation` record.

A retryable maintenance failure, any maintenance exception, or an incomplete retention
report arms a 30-second retry deadline. A non-retryable failure is logged without a
timer retry and waits for a relevant new trigger: a later create for retention, or a
later create/periodic five-minute tick for GC. Under continuous foreground backlog, pending maintenance can wait until the slot becomes quiescent.

An exception escaping the scheduler loop is caught and logged, but it terminates the
scheduler thread permanently. Manual operations and registrations can remain
available, and the current health/status API does not report the loss of periodic
scheduling.

## Unregister and shutdown

`unregister_container` first examines the slot under its mutex. If foreground work is
pending or any create/restore/maintenance item is active, it returns a retryable
`conflict` without closing or removing the registration. Otherwise it atomically marks
the slot closing, drops pending periodic-create and coalesced maintenance work, clears
retry deadlines, rejects new work, and joins the idle worker.
The worker restores its affinity and exits before the registry slot and CPU assignment
are released. Register, unregister, and shutdown calls are globally serialized by the
lifecycle mutex, while manual create/restore admission proceeds through per-slot locks.

`shutdown` is idempotent. It marks every slot closing, drops queued periodic and
maintenance work, clears retry deadlines, stops and joins the scheduler, drains an
accepted/active foreground operation and any already active bounded maintenance pass,
joins all workers, and finally clears the registry.
It has no timeout or forced-cancellation path. If worker affinity restoration
fails, shutdown stores that error and returns it again on later calls. The service
destructor invokes the same path but discards the result. Injected dependencies
must outlive the service.

Exceptions from operation implementations do not escape a worker. Allocation
failures become `resource_exhausted`; other exceptions become `internal`. Because an
exception can occur after an opaque side effect, create exceptions conservatively
report `publication_may_be_visible` and restore exceptions report
`incarnation_may_exist`. Maintenance exceptions are logged as retryable and arm the
30-second retry. The worker remains usable after an individual operation exception.

Implementations injected behind `ICheckpointOperations`, `IClock`, `IIdGenerator`,
`IAffinityController`, and `ILogger` must support the documented concurrent calls
from different registration workers. `ILogger::log` and timer interruption retain
their `noexcept` contract. The service and lower orchestration layers log
collaborator-provided `CheckpointErrorDetail::diagnostic` text without redaction,
so every injected implementation must sanitize diagnostics and must not include
credentials, cold-start environment values, runtime stderr, or artifact contents.

## Verification

`//tests/unit:checkpoint_service_test` uses a fake generation timer, barriers,
thread-safe operation counters, fake affinity bindings, and no sleeps. It verifies
registration validation, the four-slot limit, four-way parallelism, same-ID
serialization, shared worker CPU assignments, exact foreground/create/retention/GC
priority, maintenance coalescing and retries, bounded-pass continuation,
unregister/shutdown races, exception containment, and source-incarnation forwarding.
`//tests/unit:checkpoint_maintenance_test` covers lease-only orchestration defaults and
error propagation; `//tests/unit:filesystem_storage_test` covers durable
retention/GC/recovery algorithms. Linux affinity mask restoration itself remains
covered by `//tests/unit:linux_runtime_test`. IPC codec, server/client, RPC handler,
and operation-store tests cover the production-facing wrapper and journal semantics.
These are still unit-level/component tests: they do not establish that the complete
production daemon-to-Podman/CRIU flow was executed end to end in a qualified rootful
environment.
