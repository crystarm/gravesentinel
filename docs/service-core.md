# Checkpoint service core: registration, scheduling, and CPU budget

## Scope and API boundary

`//src/core:checkpoint_service` is the stage-09 owner of registrations and the
synchronous create/restore orchestration implemented by
[`CheckpointCreator`](checkpoint-create.md) and
[`RestoreCoordinator`](checkpoint-restore.md). It is implemented and unit-tested
as a library, but it is not yet wired into a running production service: the
current `gravesentinel_hypervisor` binary is still a stub and does not instantiate
`CheckpointService`, storage, chunking, hashing, affinity, or the Podman adapter.
This component therefore describes the available in-process behavior, not a
currently reachable IPC endpoint.

The core does not contain Unix sockets, wire JSON, request journaling, externally
visible asynchronous operation state, retention, or garbage collection.

`ICheckpointService` exposes synchronous `register_container`,
`unregister_container`, `create_checkpoint`, `restore_or_restart`, and `shutdown`
operations. Manual create/restore calls supply their `RequestId` and `OperationId`
and return `CheckpointResult<void>` only after the operation reaches a terminal
result. IDs are caller-owned: the service does not enforce global uniqueness,
deduplicate submissions, journal completion, or provide idempotent replay. A retry
must use fresh IDs and is a new operation.

The direct creator and coordinator return rich `CreatedCheckpoint` and
`RestoredContainer` results. The service intentionally consumes those results:
create retains only the source incarnation, restore retains only the new
incarnation, and the caller does not receive checkpoint identity/generation,
checkpoint-versus-cold-start outcome, or pin/staging cleanup-completion flags. A
future IPC layer must define its own asynchronous submit/poll and result model
rather than assuming `OperationStatus` is maintained by this core.

`CheckpointOperations` is the production adapter from the service to the existing
synchronous creator and restore coordinator. Tests replace its
`ICheckpointOperations` seam without constructing storage or Podman.

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
is not released while unregister is waiting for an active operation, so a fifth
registration cannot reuse its CPUs prematurely.

Two accepted registration fields are currently inert after validation:

- `CheckpointConfig::keep_last` must be in `1..3`, but this stage has no retention
  pass and does not use the value when creating or scheduling checkpoints;
- optional `ContainerRegistration::runtime_fingerprint` must be complete when
  present, but it is not forwarded to create or restore. Creation records the
  fingerprint returned by the runtime adapter, and restore validates the
  fingerprint stored in the selected manifest.

## Worker and concurrency model

Each registered container owns one persistent worker and a mailbox of capacity one.
The worker binds its own thread to the registration's `checkpoint_cpu` before the
registration succeeds and attempts to restore its original affinity on exit. An
affinity-restore error is returned by unregister/shutdown (and cached by shutdown);
the destructor invokes shutdown but cannot report that error. The calling control
thread is never rebound. Capture, chunking, hashing, reassembly, and runtime adapter
calls all execute inside that bound worker context. Runtime subprocesses also
receive the same checkpoint CPU through the existing adapter/process-runner
contract.

One worker per slot provides both limits without a global unbounded queue:

- no more than four container operations can execute simultaneously;
- create and restore for one `ContainerId` are serialized;
- operations for different IDs can execute concurrently.

A second synchronous caller for the same ID waits for the active operation to
finish before occupying the mailbox. Waiting callers are not queued in FIFO order,
and no fairness guarantee exists between multiple manual callers or between a
manual caller and a due periodic item; whichever participant acquires the slot
mutex while it is idle may be admitted first. A closing registration wakes such
callers and rejects work that had not yet been accepted. Reentrant synchronous
service calls from the registration's own worker are rejected as conflicts rather
than self-deadlocking.

Accepted operations are not cancellable through this API and receive no service
stop token or service-level deadline. Subprocess adapters have their own timeouts,
but a blocked storage/runtime/custom collaborator can block its caller and can make
unregister or shutdown wait without a core-imposed bound.

After a successful create, the returned source `RuntimeIncarnation` is retained in
memory and supplied to the next restore as the exact source fence. A successful
restore replaces it with the restored incarnation. A failed restore clears this
hint, forcing the next attempt through the coordinator's conservative absence
proof. The service itself neither persists cold-start environment values nor adds
them to log fields; the collaborator-diagnostic constraint below still applies.

## Periodic scheduling and missed ticks

The scheduler uses `IClock::monotonic_now()` and an injectable generation-based
`ITimer`. `SteadyTimer` is the production implementation. The generation parameter
prevents a registration, shutdown, or test-clock advance from being lost between
computing the earliest deadline and entering the timer wait.

The first periodic deadline is registration time plus `interval`. At a due deadline
the next deadline is set to `now + interval`. A periodic create is placed only when
the slot is idle and its mailbox is empty. If a manual or periodic operation is
pending/running, that tick is skipped. Multiple elapsed intervals therefore never
create a catch-up burst or an unbounded backlog.

An admitted periodic operation receives a locally constructed request ID of the
form `periodic-<container-id>-<slot-sequence>` and an `OperationId` from
`IIdGenerator`. The sequence advances only for admitted periodic work and resets
when a container is unregistered and registered again, so the periodic request ID
is not globally unique; the generated operation ID must provide operation
uniqueness. Periodic completion has no caller-visible status and failures are sent
only to the injected `ILogger`. Until production wiring exists, this is not yet a
hypervisor diagnostic log in an operational sense.

An exception escaping the scheduler loop is caught and logged, but it terminates
the scheduler thread permanently. Manual operations and registrations can remain
available, and there is currently no health/status API that reports the loss of
periodic scheduling.

## Unregister and shutdown

`unregister_container` atomically marks its slot closing, cancels a pending periodic
item, rejects new work, and waits for an accepted manual or active operation to
finish. An already active periodic operation also runs to completion. The worker
then attempts to restore its affinity and exits before the registry slot and CPU
assignment are released. Register, unregister, and shutdown calls are globally
serialized by the lifecycle mutex, while manual create/restore admission proceeds
through the per-slot locks.

`shutdown` is idempotent. It marks every slot closing, cancels pending periodic
items, stops and joins the scheduler, drains accepted/active manual operations and
active periodic operations, joins all workers, and finally clears the registry.
It has no timeout or forced-cancellation path. If worker affinity restoration
fails, shutdown stores that error and returns it again on later calls. The service
destructor invokes the same path but discards the result. Injected dependencies
must outlive the service.

Exceptions from creator/restore implementations do not escape a worker. Allocation
failures become `resource_exhausted`; other exceptions become `internal`. Because an
exception can occur after an opaque side effect, create exceptions conservatively
report `publication_may_be_visible` and restore exceptions report
`incarnation_may_exist`. The worker remains usable after an individual operation
exception.

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
serialization, worker CPU assignments, unchanged control-thread affinity, missed
periodic tick coalescing, unregister/shutdown races, exception containment, and
source-incarnation forwarding. Linux affinity mask restoration itself remains
covered by `//tests/unit:linux_runtime_test`. These tests use injected fakes and do
not provide production binary wiring or an end-to-end service composition.
