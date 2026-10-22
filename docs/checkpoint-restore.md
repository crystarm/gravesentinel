# Safe checkpoint restore, fallback, and cold start

## Scope

`//src/core:restore_coordinator` is the stage-08 synchronous restore orchestration
layer. It selects committed Manifest v1 checkpoints newest-to-oldest, validates and
pins each candidate, reassembles verified bytes, fences the original runtime
incarnation, invokes `IPodmanAdapter`, and performs a cold start only after all
checkpoint candidates are unusable or have failed with confirmed cleanup.

Periodic scheduling and registration ownership are deferred to the service core.
Retention and garbage collection are separate later stages. The coordinator holds
an exclusive storage operation for the complete request, so create, restore,
retention, and conflicting cleanup for the same `ContainerId` cannot overlap.

## Request and result

A `RestoreRequest` contains the request/operation IDs, logical container ID, stable
runtime reference, CPU assignment, optional `ColdStartSpec`, and an optional
trusted `RuntimeIncarnation` for the source container. If a source incarnation is
provided, its runtime reference must equal the registered target reference.

A successful `RestoredContainer` records whether the result came from a checkpoint
or cold start, the new runtime incarnation, and the selected checkpoint ID and
generation when applicable. Pin-release and staging-cleanup completion are reported
separately. Once a runtime start succeeds, a later metadata cleanup failure does
not turn that start into a false failure or trigger another start; the failure is
logged and conservative stale-staging cleanup remains future recovery work.

`ColdStartSpec` environment values are used only for the synchronous adapter call.
They are never persisted in manifests, storage operation metadata, or logs.

## Candidate order and validation

The coordinator obtains the committed generation list while holding the storage
lease, validates that generations are positive and unique, and sorts them in
strictly descending order even if an alternative storage implementation returns a
different order. For each generation it:

1. durably pins the published manifest;
2. reads and strictly decodes canonical Manifest v1, including schema, checksum,
   container identity, and generation checks;
3. asks the runtime adapter to validate the exact fingerprint and immutable source
   image identity;
4. requires the qualified MVP runtime shape of exactly one opaque regular artifact;
5. reassembles into a fresh `snapshot-<generation>` child of this operation's
   staging directory;
6. verifies every chunk size and BLAKE3 digest before the artifact path can reach
   the adapter.

A corrupt/missing candidate or `incompatible_runtime` candidate is released and
skipped. Storage/control-plane failures are terminal rather than being mistaken for
a bad checkpoint. Per-generation output directories allow multiple fallback
attempts under one uninterrupted storage lease; partial output from one rejected
candidate is removed only when the whole operation staging tree is cleaned.

## Absence fence and runtime attempts

Reassembly and compatibility checks are completed before stopping a known source,
minimizing avoidable downtime. Immediately before the first restore or cold start:

- with a trusted source incarnation, `stop_and_remove` verifies the immutable
  runtime ID, ownership, and cpuset before mutation, then proves both normal and
  external Podman records absent;
- without a trusted incarnation, `confirm_absence` performs only the two read-only
  Podman existence probes. It never calls inspect, stop, or remove. Any presence or
  uncertain probe result is terminal.

Storage locking serializes CCM operations, while the stable runtime reference and
ownership policy exclude out-of-band replacement as a supported operation during
restore. The read-only probe deliberately does not delete an unexpected live
container.

The manifest pin remains held through the adapter restore call. If restore fails
with `RuntimeSideEffect::incarnation_may_exist`, the coordinator invokes
`cleanup_failed_restore`; this destructive cleanup is allowed only because the
coordinator proved absence immediately before its own restore attempt under the
same lease. If restore reports no possible side effect, the coordinator performs a
new read-only absence proof instead. Fallback to the next generation is permitted
only after one of those proofs succeeds.

Any cleanup or absence-proof failure terminates the request with
`incarnation_may_exist`; no older restore and no cold start is attempted. A pin is
released only after the adapter call and any required runtime cleanup finish.

## Cold start

After every candidate is rejected as corrupt/incompatible or fails restore with
confirmed cleanup, cold start is considered exactly once. It requires an explicit
`ColdStartSpec` and a currently confirmed absence fence. A failed cold start is
never retried by this coordinator. If it may have created an incarnation,
`cleanup_failed_restore` is attempted before returning failure; failure to prove
cleanup remains indeterminate and prohibits another start.

Without a `ColdStartSpec`, exhaustion of candidates is a structured failure. The
coordinator never invents an image, argv, environment, mount, or runtime option.

## State machine and cleanup

The operation starts in `VALIDATING` and uses these transitions, with safe fallback
loops back to validation:

```text
VALIDATING -> REASSEMBLING -> RESTORING -> RESTORED
     ^              |              |
     |              +--------------+  candidate rejected or restore cleaned
     |
     +-- next older generation

VALIDATING / REASSEMBLING / RESTORING -> FAILED
```

Cold start uses `VALIDATING -> RESTORING -> RESTORED|FAILED`. Every transition and
candidate rejection is logged with container, request, operation, stable error
code, and checkpoint/generation when known. Runtime stderr, artifact contents, and
cold-start environment values are not logged.

On every ordinary failure path the active pin is explicitly released before the
operation staging cleanup is attempted. The operation object retains the
cross-process per-container lease until destruction. RAII remains a final safety
net for abrupt stack unwinding, but is not used as a substitute for observable
ordinary-path cleanup.

## Verification

`//tests/unit:restore_coordinator_test` uses fake storage/runtime/logger seams with
real BLAKE3 and production reassembly. It covers strict generation order, manifest
and chunk corruption before runtime restore, incompatibility, runtime side effects,
pin lifetime, cleanup-gated fallback, cold start, source fencing, unique candidate
staging, and same-container lease exclusion. `//tests/unit:podman_adapter_test`
separately verifies the production read-only absence probe and confirms that
presence never causes stop/remove commands.
