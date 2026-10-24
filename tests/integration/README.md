# Podman/CRIU integration tests

These targets are destructive, host-dependent, and opt-in. They are excluded from
`bazel test //...` by the `manual` tag and also carry `requires-podman`,
`exclusive`, and `local`.

The qualified MVP profile is local **rootful** Podman. Rootless Podman 5.4.2 is
unsupported because it rejects checkpoint creation even when the CRIU and cpuset
prerequisites pass.

Build the static fixture and production adapter smoke driver as the normal
repository user:

```text
bazel build \
  //src/hypervisor:gravesentinel_hypervisor \
  //tests/integration/fixture:counter_fixture \
  //tests/integration:podman_adapter_smoke_driver \
  //tests/integration:hypervisor_e2e_driver
```

Check prerequisites without creating a container:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  scripts/check_testbed.sh
```

Run the complete qualification smoke only on a disposable, dedicated Linux host:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/podman_checkpoint_smoke.sh \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh
```

Run the stage-06 production adapter smoke with the same fixture and safety checks:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/podman_checkpoint_smoke.sh \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh \
  --adapter-helper bazel-bin/tests/integration/podman_adapter_smoke_driver
```

This mode executes cold start, checkpoint, expected-incarnation stop/remove,
failed-restore cleanup and valid restore through the production `PodmanAdapter`,
`PosixProcessRunner`, and `LinuxAffinityController`. It proves both ordinary and
external Podman-object absence; the surrounding fixture checks cgroup removal and
state continuity. Secret workload environment values are passed through a
transient mode-`0600` env-file, never through Podman's own process environment.

Do not use `sudo bazel` for the normal local workflow. It can create root-owned
workspace/cache files and may select a different Bazel installation. The manual
Bazel targets below are intended for CI or another process already running in the
qualified rootful environment:

```text
bazel test //tests/integration:testbed_check --test_output=streamed
bazel test //tests/integration:podman_checkpoint_smoke \
  --test_output=streamed
bazel test //tests/integration:podman_adapter_smoke \
  --test_output=streamed
bazel test //tests/integration:hypervisor_scheduler_e2e \
  --test_output=streamed
```

Both smoke targets build a `scratch` image without a network pull. It creates only
uniquely named resources with the `gravesentinel-ccm-smoke-` prefix and a per-run
ownership label. On ordinary completion and failure it removes its containers,
image, archive, and observed Podman-managed cgroups. `--keep-artifacts` exists in
the underlying script only for deliberate qualification diagnostics.

A passing prerequisite check is not qualification by itself. The complete smoke
must finish with:

```text
RESULT: QUALIFIED Podman/CRIU checkpoint smoke passed
```

The accepted versions, exact argv, observed artifact entries, security limits,
and two successful stage-02 qualification runs are recorded in `docs/testbed.md`
and `docs/adr/0001-podman-criu-runtime-profile.md`. The adapter target has a
separate success marker:

```text
RESULT: QUALIFIED production PodmanAdapter smoke passed
```

This marker was obtained on the qualified rootful profile on 2026-10-22; rerun
the gate and complete smoke whenever the runtime or host profile changes.

## Hypervisor + SchedulerClient process-boundary E2E

`//tests/integration:hypervisor_scheduler_e2e` is the stage-12 harness. Unlike the
stage-02 direct Podman smoke and stage-06 adapter helper, it starts the real
`gravesentinel_hypervisor` subprocess with unique socket and storage roots. Every
functional lifecycle request uses `SchedulerClient`: handshake, health, register,
async create/restore submission, `GetOperation` polling, and unregister. Direct
Podman access is limited to building the fixture image, read-only observations, fault
oracles, and final ownership-checked cleanup after the daemon has stopped.

The harness covers:

- cold start from an explicit registered image/argv spec;
- checkpoint with `--leave-running`, source progress, controlled replacement, and
  restored memory, `/dev/shm` file history, stdout/stderr, threads, and CPU affinity;
- corruption of a newest-generation-only chunk and observed fallback to generation 1;
- a checksum-valid but runtime-incompatible manifest and a real cold start;
- five checkpoints, `keep_last=3`, candidate grace, and deletion of a known
  generation-1-only chunk;
- exactly one owned runtime instance after every restore/cold start.

Build as the normal repository user, then run the wrapper in the qualified rootful
context; do not run Bazel itself through `sudo`:

```text
bazel build \
  //src/hypervisor:gravesentinel_hypervisor \
  //tests/integration:hypervisor_e2e_driver \
  //tests/integration/fixture:counter_fixture

sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/hypervisor_scheduler_e2e.sh \
  --hypervisor bazel-bin/src/hypervisor/gravesentinel_hypervisor \
  --driver bazel-bin/tests/integration/hypervisor_e2e_driver \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh
```

A CI runner that is already rootful can invoke the explicit manual Bazel target shown
above. Expected duration is approximately 3–10 minutes, depending on CRIU and storage
speed. The wrapper uses `--gc-grace-seconds 1`; the production default remains 3600
seconds. It creates only a uniquely labelled fixture image, four uniquely named
containers, one daemon process, and a short `/tmp/gravesentinel-e2e.*` socket/storage
root. The daemon runs in its own session; setup/cleanup Podman commands are bounded.
On success, failure, or signal the wrapper stops and reaps the daemon, terminates any
remaining process in its session, validates ownership before removing containers/image,
checks ordinary and external Podman stores, mounts, and observed cgroups, and removes
temporary storage. Functional failures retain the temporary root and print
its path; runtime objects are still removed. Cleanup failure makes the suite fail.

Only a run that finishes cleanup and prints this final marker is stage-12 evidence:

```text
RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed
```

On 2026-10-24 the implementation/build/unit checks passed in the development
workspace, but the available Podman invocation was rootless and the prerequisite gate
ended with `RESULT: BLOCKED (1 failed prerequisite checks)`; non-interactive rootful
access was unavailable. Therefore this repository does **not** claim a successful
stage-12 privileged run yet.
