# GraveSentinel documentation

This directory documents the implemented Checkpoint Cache Manager (CCM) service,
its scheduler IPC client, operations/recovery contract, release gates, the qualified
Podman/CRIU profile, persisted formats, and remaining lifecycle work.

## Current status

The repository contains tested implementations of Manifest v1, filesystem storage,
chunking/reassembly, the Linux process and affinity layer, the Podman adapter,
checkpoint creation, restore/fallback/cold start, retention, chunk garbage
collection, startup storage recovery, the checkpoint service core, and their
production composition in `//src/hypervisor:gravesentinel_hypervisor`.

The composition exposes a versioned, bounded request/response JSON protocol over a
local `AF_UNIX/SOCK_SEQPACKET` socket. The concrete privilege-free scheduler client
verifies the server peer UID (UID 0 by default); the server verifies the connecting
scheduler UID with `SO_PEERCRED` and supports a configured socket GID and mode.
Create and restore are durable asynchronous submit operations followed by
`GetOperation` polling. Register and unregister remain synchronous and are not
transport-retried after an exchange may have reached the server. At the core boundary,
a busy unregister returns a retryable conflict instead of waiting; protocol v2 exposes
all synchronous register/unregister service failures only as `request_failed`, so the
scheduler must reconcile state before deciding whether to retry.

The operation journal is durable and bounded. It derives deterministic operation
IDs from request IDs, deduplicates identical create/restore submissions across
restarts, compacts its JSONL history, marks a crash-interrupted queued operation as
failed before execution, and marks a crash-interrupted running operation as
indeterminate. Its in-memory/recovered registry is also bounded and does not evict
completed history: once full, it rejects new unique submissions while still serving
known duplicates and status queries.

Committed creates apply `keep_last` retention immediately under their create lease.
The service retries incomplete retention and runs coalesced, bounded garbage collection
on the existing per-container workers; startup recovery runs before the service,
operation journal, and readiness. Background maintenance is internal and does not
create public operation-journal records.

The remaining important limits are:

- operation-history eviction and administrative pruning are not implemented, so the
  bounded durable registry can still fill with terminal records;
- the registration-level optional `RuntimeFingerprint` is validated and carried on
  the wire but is not forwarded into create or restore decisions;
- an indeterminate operation requires scheduler/operator reconciliation and must not
  be blindly resubmitted as a new destructive operation;
- the only production runtime profile is `podman-criu-rootful-v1`, with executable
  defaults `/usr/bin/podman` and `/usr/sbin/criu`; its destructive checkpoint and
  restore flow must run only in a qualified rootful environment.

The binary is buildable and its composed components and IPC behavior are covered by
unit tests. On 2026-10-25, the manual
`//tests/integration:hypervisor_scheduler_e2e` regression harness completed the full
process/IPC/Podman path against the current production recovery code and emitted
`RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed`. The complete
non-privileged release gate passed on the same date. The four-container CPU acceptance
qualified twice and crash/reconnect/recovery qualified once on 2026-10-25, so all
three rootful acceptance markers are current for the code being released. This is
dated evidence for the measured host; each deployment still requires its own rootful
qualification.

## Reading guide

| Document | Purpose |
| --- | --- |
| [Architecture](architecture.md) | Current component status, process boundaries, Bazel graph, IPC/journal behavior, data flows, trust boundaries, and remaining work |
| [SchedulerClient](scheduler-client.md) | Public Bazel/API boundary, async operations, bool wrappers, reconnect and protocol policy |
| [Operations](operations.md) | CLI/defaults, ownership, systemd, startup/recovery, upgrade, logging, diagnostics, and unsupported modes |
| [Release gate](release-gate.md) | Non-privileged release gate, rootful suites, success markers, and requalification triggers |
| [Requirements traceability](requirements-traceability.md) | Exact Bazel evidence for requirements 8.1/8.2 and stage-13 process criteria |
| [Manifest v1](manifest-v1.md) | Normative persisted manifest schema, canonical JSON, checksum, validation, and limits |
| [Chunking](chunking.md) | Fixed-size ingest, namespace-local deduplication, immutable tree checks, and verified reassembly |
| [Filesystem storage](filesystem-storage.md) | On-disk layout, locking, staging, pins, maintenance records, durable publication/deletion, reads, recovery, and known limits |
| [Checkpoint creation](checkpoint-create.md) | Capture-to-manifest transaction, generation assignment, commit point, immediate retention, cleanup, and indeterminate publication |
| [Checkpoint restore](checkpoint-restore.md) | Candidate selection, validation, pinning, absence fence, fallback, cold start, and cleanup |
| [Maintenance](maintenance.md) | Strict retention, candidate-based chunk GC, startup recovery, scheduling/retry policy, and safety invariants |
| [Service core](service-core.md) | Registration wire schema, CPU validation, workers, foreground/periodic/maintenance scheduling, IPC exposure, concurrency, shutdown, and API limitations |
| [Testbed qualification](testbed.md) | Dated Podman/CRIU qualification evidence, exact runtime profile, reproduction commands, and test scope |
| [ADR 0001](adr/0001-podman-criu-runtime-profile.md) | Accepted rootful Podman/CRIU profile and opaque-artifact decision |

A useful implementation-order reading path is:

1. [Architecture](architecture.md)
2. [SchedulerClient](scheduler-client.md)
3. [Operations](operations.md)
4. [Manifest v1](manifest-v1.md)
5. [Filesystem storage](filesystem-storage.md)
6. [Chunking](chunking.md)
7. [Checkpoint creation](checkpoint-create.md)
8. [Checkpoint restore](checkpoint-restore.md)
9. [Maintenance](maintenance.md)
10. [Service core](service-core.md)
11. [Requirements traceability](requirements-traceability.md)
12. [Release gate](release-gate.md)
13. [Testbed qualification](testbed.md)

## Terminology

- **CCM** — Checkpoint Cache Manager.
- **logical container** — scheduler-owned `ContainerId`; it is not a Podman ID.
- **runtime reference** — stable configured `RuntimeContainerRef`, normally the
  Podman name managed by CCM.
- **runtime incarnation** — the pair of runtime reference and immutable runtime
  container ID observed for one concrete process/container lifetime.
- **checkpoint** — a committed canonical manifest plus every immutable chunk it
  references.
- **generation** — positive per-container ordering number. It, not a timestamp,
  determines newest-to-oldest order.
- **operation** — a create or restore request tracked by the durable public IPC
  operation journal and, when admitted by the service, driving a storage transaction
  under the per-container lease. Retention and GC use internal lease-only maintenance
  operations and are not public `OperationStore` records.
- **candidate** — one committed generation considered during restore.
- **indeterminate side effect** — an error for which publication may already be
  visible or a runtime incarnation may still exist.

## Contract layers

The documentation deliberately separates four kinds of statements:

1. **Format contracts** — Manifest v1 and the storage layout describe bytes and
   durability rules implemented by the current code. A persisted-format change
   requires an explicit versioning decision.
2. **Interface contracts** — types in `src/core/contracts.hxx` define method shapes,
   but do not by themselves prove locking, durability, sanitization, or thread
   safety for arbitrary implementations.
3. **Production implementation behavior** — guarantees attributed to
   `FilesystemStorage`, `SnapshotChunker`, `PodmanAdapter`, `OperationStore`, the
   IPC server/client, and other concrete classes apply to those implementations and
   their documented composition.
4. **Qualification evidence** — `testbed.md` records dated observations on one
   exact host/runtime profile. It is not a timeless portability claim and must be
   rerun after a relevant host or runtime change.

When documentation and executable behavior disagree, treat source and tests as the
current behavioral evidence, then update the document. For compatibility-sensitive
persisted data, treat the discrepancy as a defect rather than silently changing
one side.

## Build and validation

The repository pins Bazel `8.8.2`, uses Bzlmod, and compiles project C++ targets as
C++23 with warnings treated as errors.

Fast project checks:

```text
bazel test //:checks
```

Focused maintenance, lifecycle, and fixture validation targets include:

```text
bazel test //tests/unit:checkpoint_maintenance_test
bazel test //tests/unit:filesystem_storage_test
bazel test //tests/unit:checkpoint_service_test
bazel test //tests/unit:checkpoint_creator_test
bazel test //tests/integration/fixture:counter_fixture_test
bazel test //tests/integration:hypervisor_lifecycle_test
```

Broader repository commands:

```text
bazel build //...
bazel test //...
bazel test //scripts:clang_format_test
```

Unlike `//:checks`, `bazel test //...` also discovers non-manual tests outside the
unit suite, including the integration fixture's non-privileged unit test.

`bazel test //...` excludes the Podman/CRIU integration tests because they are
marked `manual`. The opt-in, destructive workflow and required rootful execution
context are documented in [testbed.md](testbed.md). Do not use `sudo bazel` for the
normal local workflow.

Sanitizer configurations are available independently:

```text
bazel test --config=asan //:unit_tests
bazel test --config=ubsan //:unit_tests
bazel test --config=tsan //:unit_tests
```

The integration fixture is expected to be statically linked for the destructive
smoke, while sanitizer builds intentionally disable that static-link profile; do
not assume the smoke is compatible with a sanitizer configuration. Run the complete
non-privileged sequence with `sh scripts/release_gate.sh`; rootful commands and
markers are documented separately in [release-gate.md](release-gate.md).

## Documentation maintenance checklist

When changing behavior, update the closest subsystem document and check whether the
change also affects:

- the status and dependency graph in [architecture.md](architecture.md);
- Manifest or storage compatibility/versioning;
- stable error codes or side-effect flags;
- resource limits and timeouts;
- security and secret-handling claims;
- the qualified runtime fingerprint or command profile;
- test commands, target names, or the distinction between unit and manual tests.

Keep historical ADR decisions intact. Add an implementation note or a superseding
ADR when later code changes refine an accepted decision.
