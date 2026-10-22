# Checkpoint service core: registration, scheduling, and CPU budget

## Scope and API boundary

`//src/core:checkpoint_service` is the stage-09 owner of registrations and the
synchronous create/restore orchestration implemented by `CheckpointCreator` and
`RestoreCoordinator`. It does not contain Unix sockets, wire JSON, request
journaling, externally visible asynchronous operation state, retention, or garbage
collection.

`ICheckpointService` exposes synchronous `register_container`,
`unregister_container`, `create_checkpoint`, `restore_or_restart`, and `shutdown`
operations. Manual create/restore calls supply their `RequestId` and `OperationId`
and return `CheckpointResult<void>` only after the operation reaches a terminal
result. The stage-10 IPC layer will own asynchronous submit/poll state and call this
synchronous core from its bounded operation workers.

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

## Worker and concurrency model

Each registered container owns one persistent worker and a mailbox of capacity one.
The worker binds its own thread to the registration's `checkpoint_cpu` before the
registration succeeds and restores its original affinity on exit. The calling
control thread is never rebound. Capture, chunking, hashing, reassembly, and runtime
adapter calls all execute inside that bound worker context. Runtime subprocesses
also receive the same checkpoint CPU through the existing adapter/process-runner
contract.

One worker per slot provides both limits without a global unbounded queue:

- no more than four container operations can execute simultaneously;
- create and restore for one `ContainerId` are serialized;
- operations for different IDs can execute concurrently.

A second synchronous caller for the same ID waits for the active operation to
finish before occupying the mailbox. A closing registration wakes such callers and
rejects work that had not yet been accepted. Reentrant synchronous service calls
from the registration's own worker are rejected as conflicts rather than
self-deadlocking.

After a successful create, the returned source `RuntimeIncarnation` is retained in
memory and supplied to the next restore as the exact source fence. A successful
restore replaces it with the restored incarnation. A failed restore clears this
hint, forcing the next attempt through the coordinator's conservative absence
proof. No cold-start environment value is logged or persisted.

## Periodic scheduling and missed ticks

The scheduler uses `IClock::monotonic_now()` and an injectable generation-based
`ITimer`. `SteadyTimer` is the production implementation. The generation parameter
prevents a registration, shutdown, or test-clock advance from being lost between
computing the earliest deadline and entering the timer wait.

The first periodic deadline is registration time plus `interval`. At a due deadline
the next deadline is set to `now + interval`. A periodic create is placed only when
the slot is idle and its mailbox is empty. If a manual or periodic operation is
pending/running, that tick is skipped. Multiple elapsed intervals therefore never
create a catch-up burst or an unbounded backlog. Periodic operations receive an
internal request ID and an `OperationId` from `IIdGenerator`; failures remain in the
hypervisor diagnostic log.

## Unregister and shutdown

`unregister_container` atomically marks its slot closing, cancels a pending periodic
item, rejects new work, and waits for an accepted manual or active operation to
finish. The worker then restores its affinity and exits before the registry slot and
CPU assignment are released.

`shutdown` is idempotent. It marks every slot closing, cancels pending periodic
items, stops and joins the scheduler, drains accepted/active manual operations,
joins all workers, and finally clears the registry. The service destructor invokes
the same path. Injected dependencies must outlive the service.

Exceptions from creator/restore implementations do not escape a worker. Allocation
failures become `resource_exhausted`; other exceptions become `internal`. Because an
exception can occur after an opaque side effect, create exceptions conservatively
report `publication_may_be_visible` and restore exceptions report
`incarnation_may_exist`. The worker remains usable after an individual operation
exception.

Implementations injected behind `ICheckpointOperations`, `IClock`, `IIdGenerator`,
`IAffinityController`, and `ILogger` must support the documented concurrent calls
from different registration workers. `ILogger::log` and timer interruption retain
their `noexcept` contract.

## Verification

`//tests/unit:checkpoint_service_test` uses a fake generation timer, barriers,
thread-safe operation counters, fake affinity bindings, and no sleeps. It verifies
registration validation, the four-slot limit, four-way parallelism, same-ID
serialization, worker CPU assignments, unchanged control-thread affinity, missed
periodic tick coalescing, unregister/shutdown races, exception containment, and
source-incarnation forwarding. Linux affinity mask restoration itself remains
covered by `//tests/unit:linux_runtime_test`.
