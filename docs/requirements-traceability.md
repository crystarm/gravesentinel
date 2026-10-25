# MVP requirements traceability

This matrix maps `.dev/notes/REQIREMENTS.txt` sections 8.1 and 8.2 plus the
additional stage-13 process/IPC criteria to executable Bazel targets. `PASS` means
the non-privileged target has run successfully in the development workspace.
`QUALIFIED` means the destructive rootful marker was observed on the dated testbed.
`PENDING` means the target exists/builds but no rootful success marker has yet been
recorded.

## 8.1 unit criteria

| Requirement | Evidence | Bazel target | Status |
| --- | --- | --- | --- |
| 8.1.1: chunk boundaries at 0, below 4 MiB, exactly 4 MiB, and above 4 MiB | Boundary ingest/chunk-count cases in `snapshot_chunker_test.cxx` | `//tests/unit:snapshot_chunker_test` | PASS |
| 8.1.2: stable BLAKE3 and same-container block reuse | Official vectors/determinism plus namespace-local dedup cases | `//tests/unit:blake3_test`, `//tests/unit:snapshot_chunker_test` | PASS |
| 8.1.3: cross-container blocks need not deduplicate | Separate container namespaces publish independently | `//tests/unit:snapshot_chunker_test` | PASS |
| 8.1.4: store/reassembly is byte-identical | Reassembly verifies bytes and file mode | `//tests/unit:snapshot_chunker_test` | PASS |
| 8.1.5: corrupted chunk detected before runtime restore | Reassembler rejects corruption; restore coordinator skips corrupted candidate before runtime call | `//tests/unit:snapshot_chunker_test`, `//tests/unit:restore_coordinator_test` | PASS |
| 8.1.6: publication failures preserve previous three snapshots | Capture/chunk/serialization/commit and open/write/fsync/rename fault matrices | `//tests/unit:checkpoint_creator_test`, `//tests/unit:filesystem_storage_test` | PASS |
| 8.1.7: GC preserves live/pinned chunks and removes orphans | Restore-pin exclusion, grace/mark/delete, live/re-referenced chunk cases | `//tests/unit:filesystem_storage_test` | PASS |
| 8.1.8: four containers checkpoint concurrently | Four service workers blocked concurrently, then completed | `//tests/unit:checkpoint_service_test` | PASS |
| 8.1.9: four standard CPU pairs and fifth rejection | Registrations `0/1`, `2/3`, `4/5`, `6/7`, worker CPU checks, fifth-slot rejection | `//tests/unit:checkpoint_service_test` | PASS |
| 8.1.10: checkpoint workers do not change Scheduler main-thread affinity | Service control thread remains unbound; Linux affinity tests bind only intended child/thread | `//tests/unit:checkpoint_service_test`, `//tests/unit:linux_runtime_test` | PASS |

All targets above are members of `//:unit_tests` and are run by the default,
ASan, UBSan, and TSan release-gate invocations.

## 8.2 integration criteria

| Requirement | Evidence | Bazel target | Status |
| --- | --- | --- | --- |
| 8.2.1: counter checkpoint, source removal, restore, continued state | Production daemon and `SchedulerClient`; memory/file counter continuity and new runtime incarnation | `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-25 |
| 8.2.2: threads, mutable memory, local files, stdout/stderr | Static fixture exercises worker threads, memory, ordinary files, `/dev/shm`, stdout and stderr | `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-25 |
| 8.2.3: forced stop between checkpoints and restore latest usable | Controlled source stop/removal and multi-generation restore | `//tests/integration:podman_checkpoint_smoke`, `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-21 / 2026-10-25 |
| 8.2.4: kill CCM during block write; prior checkpoints survive | Real `SIGKILL` at controlled chunk-write and manifest publication seams; restart, restore, deduplicated resubmit | `//tests/integration:hypervisor_crash_recovery_e2e` | QUALIFIED rootfully on 2026-10-25 |
| 8.2.5: corrupt newest block and fall back | Newest-only chunk corruption; observed restoration of prior generation | `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-25 |
| 8.2.6: incompatible snapshot cold-starts from configured source | Checksum-valid runtime incompatibility and explicit `ColdStartSpec` | `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-25 |
| 8.2.7: exactly latest three and GC old blocks | Five creates, retained generations 3/4/5, known generation-1-only chunk removed | `//tests/integration:hypervisor_scheduler_e2e` | QUALIFIED 2026-10-25 |
| 8.2.8: four CPU containers, four parallel checkpoints, restore all | Four standard pairs, deterministic checkpoint overlap, controlled removal, four restores and affinity/state checks | `//tests/integration:hypervisor_four_cpu_e2e` | QUALIFIED rootfully twice on 2026-10-25 |

The manual aggregate `//tests/integration:stage13_acceptance` contains the stage-12
suite and both stage-13 suites. All three constituent wrappers produced current-code
rootful qualification markers on 2026-10-25.

## Additional process, recovery, and release criteria

| Criterion | Evidence and target | Status / limitation |
| --- | --- | --- |
| Real daemon crash during chunk write and before/after manifest rename | `//tests/integration:hypervisor_crash_recovery_e2e` gates each seam, sends `SIGKILL`, restarts on the same storage/journal, verifies prior generations and the same `OperationId` for duplicate `RequestId` | QUALIFIED rootfully on 2026-10-25 |
| Interrupted restore before runtime side effect, after successful restore, and with conflict | `//tests/unit:operation_store_test`; `//tests/integration:hypervisor_crash_recovery_e2e` verifies no blind replay and at most one owned runtime | QUALIFIED rootfully on 2026-10-25. Recovered `running` remains conservatively `indeterminate`; phase-aware automatic success classification is not implemented. |
| SchedulerClient disconnect/reconnect and at-most-once request execution | `//tests/unit:ipc_server_client_test`, `//tests/unit:operation_store_test`, `//tests/integration:hypervisor_lifecycle_test`, `//tests/integration:hypervisor_crash_recovery_e2e` | QUALIFIED destructive path on 2026-10-25. A disconnect does not cancel work; reconciliation uses the same request/operation IDs. |
| Four simultaneous CPU containers/checkpoint workers; fifth rejected | `//tests/unit:checkpoint_service_test`; `//tests/integration:hypervisor_four_cpu_e2e` checks real container tasks, Podman cpuset, effective cgroup cpuset, daemon worker TIDs, gated Podman child, daemon baseline tasks, and client process | QUALIFIED rootfully twice on 2026-10-25. The suite does not claim absolute observation of every later Podman/CRIU helper descendant. |
| ENOSPC, permission, and interrupted write without loss of published versions | `//tests/unit:filesystem_storage_test`; `//tests/integration:hypervisor_crash_recovery_e2e` modes `chunk-enospc`, `chunk-eacces`, `chunk-eintr-once` | QUALIFIED rootfully on 2026-10-25 |
| Current/unsupported protocol, malformed traffic, second daemon, stale socket, bounded journal cleanup | `//tests/integration:hypervisor_lifecycle_test`, `//tests/unit:ipc_codec_test`, `//tests/unit:ipc_server_client_test`, `//tests/unit:operation_store_test` | PASS |
| Repeated reconnect/create/restore/retention/GC and graceful SIGTERM | `//tests/integration:hypervisor_crash_recovery_e2e`; component coverage in `//tests/unit:checkpoint_service_test` and `//tests/unit:filesystem_storage_test` | QUALIFIED rootfully on 2026-10-25 |
| Public external SchedulerClient only; core/storage/Podman hidden | `//scripts:check_scheduler_client_boundary` builds `examples/scheduler_client_consumer` and expects negative visibility targets to fail | PASS when invoked by release gate; this is a `sh_binary`, not part of `bazel test //...` |
| External consumer communicates with separate daemon | `//scripts:check_scheduler_client_consumer_runtime` | PASS when invoked by release gate; non-privileged real process/socket boundary |
| Format, release build, default tests, sanitizers, external consumer, placeholder scan | `//scripts:release_gate`; format subtest `//scripts:clang_format_test` | QUALIFIED non-privileged on 2026-10-25 |
| Exact rootful full integration suite | `//tests/integration:stage13_acceptance` | QUALIFIED by all three current-code constituent wrapper markers on 2026-10-25 |

## Documented, intentionally unsupported scope

No test claims support for these out-of-scope modes:

- snapshot/rollback of critically mutable shared volumes;
- preservation of live TCP sessions or a network RPC transport;
- GPU or arbitrary device checkpoint/restore;
- remote/rootless Podman;
- inter-host migration or shared multi-daemon storage;
- CPU layouts other than the four standard logical pairs.

These limits are part of the accepted MVP contract, not missing acceptance tests.
See [`testbed.md`](testbed.md),
[`adr/0001-podman-criu-runtime-profile.md`](adr/0001-podman-criu-runtime-profile.md),
and [`operations.md`](operations.md).

## Completion rule

Both new stage-13 rootful targets and the stage-12 process-boundary regression
qualified against the current production recovery code on 2026-10-25. The current
non-privileged release marker was recorded on the same date. Stage 13 and the MVP are
complete for this dated testbed evidence. Only successful functional assertions plus
final qualification markers count as evidence.
