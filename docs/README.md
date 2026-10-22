# GraveSentinel documentation

This directory documents the implemented Checkpoint Cache Manager (CCM) libraries,
the qualified Podman/CRIU profile, persisted formats, and the remaining integration
work.

## Current status

The repository contains tested implementations of Manifest v1, filesystem storage,
chunking/reassembly, the Linux process and affinity layer, the Podman adapter,
checkpoint creation, restore/fallback/cold start, and the synchronous checkpoint
service core.

The project is **not yet an operational end-to-end daemon**:

- `//src/hypervisor:gravesentinel_hypervisor` is still a stub that exits with
  failure;
- the implemented libraries are not composed in that binary;
- the Unix socket server, wire protocol, concrete scheduler client, asynchronous
  operation journal, retention, garbage collection, and startup recovery are not
  implemented;
- `CheckpointConfig::keep_last` and the registration-level optional
  `RuntimeFingerprint` are currently validated but have no operational effect.

Do not interpret a successful library build or unit-test run as proof that a
running scheduler can use CCM through the hypervisor binary.

## Reading guide

| Document | Purpose |
| --- | --- |
| [Architecture](architecture.md) | Current component status, process boundaries, actual Bazel graph, data flows, trust boundaries, and planned work |
| [Manifest v1](manifest-v1.md) | Normative persisted manifest schema, canonical JSON, checksum, validation, and limits |
| [Chunking](chunking.md) | Fixed-size ingest, namespace-local deduplication, immutable tree checks, and verified reassembly |
| [Filesystem storage](filesystem-storage.md) | On-disk layout, locking, staging, pins, durable publication, reads, cleanup, and known limits |
| [Checkpoint creation](checkpoint-create.md) | Capture-to-manifest transaction, generation assignment, commit point, cleanup, and indeterminate publication |
| [Checkpoint restore](checkpoint-restore.md) | Candidate selection, validation, pinning, absence fence, fallback, cold start, and cleanup |
| [Service core](service-core.md) | Registration, CPU validation, workers, periodic scheduling, concurrency, shutdown, and API limitations |
| [Testbed qualification](testbed.md) | Dated Podman/CRIU qualification evidence, exact runtime profile, reproduction commands, and test scope |
| [ADR 0001](adr/0001-podman-criu-runtime-profile.md) | Accepted rootful Podman/CRIU profile and opaque-artifact decision |

A useful implementation-order reading path is:

1. [Architecture](architecture.md)
2. [Manifest v1](manifest-v1.md)
3. [Filesystem storage](filesystem-storage.md)
4. [Chunking](chunking.md)
5. [Checkpoint creation](checkpoint-create.md)
6. [Checkpoint restore](checkpoint-restore.md)
7. [Service core](service-core.md)
8. [Testbed qualification](testbed.md)

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
- **operation** — one create, restore, retention, or GC storage transaction holding
  the per-container lease.
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
   `FilesystemStorage`, `SnapshotChunker`, `PodmanAdapter`, and other concrete
   classes apply to those implementations and their documented composition.
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
not assume the smoke is compatible with a sanitizer configuration.

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
