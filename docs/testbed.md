# Podman + CRIU testbed qualification

## Qualification status

### Dated recorded evidence

**QUALIFIED on 2026-10-21.** The complete rootful checkpoint/export/restore
smoke passed twice on the target host. Both runs proved `--leave-running`
progress, removal of the original container before restore, in-memory state
continuity after import, CPU affinity, negative-restore cleanup, cgroup cleanup,
and idempotent resource cleanup.

**QUALIFIED for stage 12 on 2026-10-24.** The production hypervisor subprocess
and `SchedulerClient` completed the full process/IPC/Podman suite with
`RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed`.

These are historical qualification records for the measured host on those dates,
not statements that the current host or current runtime installation has been
rechecked. Qualification applies only to the exact runtime profile and command
boundary in this document. A `+CRIU` feature in `crun --version` means that the
OCI runtime was compiled with CRIU integration; the successful smoke, not that
feature flag, is the evidence that the complete workflow works.

### Current verification boundary

A deployment is currently verified only after the prerequisite gate and the
appropriate complete destructive smoke have been rerun in that deployment's
rootful execution context. This repository retains the dated observations and
success markers below, but it does not retain a raw qualification log, a signed
or checksummed host report, checkpoint hashes, or a CI evidence bundle from the
recorded runs.

## Accepted runtime profile

| Property | Qualified value |
| --- | --- |
| Distribution | Debian 13 (trixie) |
| Kernel | `6.12.111+deb13-amd64` |
| Architecture | host `x86_64`; Podman `amd64` |
| CPUs available to the process | 12, `Cpus_allowed_list=0-11` |
| MVP CPUs | all logical CPUs `0..7` available |
| Cgroups | unified cgroup v2 managed by systemd |
| Available controllers | `cpuset cpu io memory hugetlb pids rdma misc` |
| Effective cpuset | `0-11` |
| Podman | local rootful Podman `5.4.2`, Debian build origin, uid 0 |
| OCI runtime | `crun 1.21`, commit `10269840aa07fb7e6b7e1acff6198692d8ff5c88` |
| OCI runtime features | includes `+APPARMOR`, `+CAP`, `+CRIU`, `+SECCOMP`, `+SELINUX` |
| CRIU | `4.1.1` at `/usr/sbin/criu` |
| Qualification process capabilities | `CapEff=000001ffffffffff`, `NoNewPrivs=0` |
| Podman security | AppArmor enabled, SELinux disabled, seccomp enabled with `/usr/share/containers/seccomp.json` |
| Host LSM list | `lockdown,capability,landlock,yama,apparmor,tomoyo,bpf,ipe,ima,evm` |
| Workload | static C++23 CPU-only fixture in a `scratch` image |

The production `PodmanAdapter` enforces a stricter machine-readable profile than
the prerequisite script. It requires all of the following observations:

- Podman client version `5.4.2`, `OsArch=linux/amd64`, `BuildOrigin=Debian`, and
  Podman info version `5.4.2`;
- Podman host distribution `debian`, version `13`, codename `trixie`, architecture
  `amd64`, and kernel `6.12.111+deb13-amd64`;
- a local, non-rootless service, null uid/gid mappings, cgroup v2 with the
  `systemd` manager, and exactly the controller set
  `cpu cpuset hugetlb io memory misc pids rdma`;
- AppArmor enabled, SELinux disabled, seccomp enabled with
  `/usr/share/containers/seccomp.json`, and the qualified Podman capability set;
- `crun` with first version line `crun version 1.21`, commit
  `10269840aa07fb7e6b7e1acff6198692d8ff5c88`, and the required `+APPARMOR`,
  `+CAP`, `+CRIU`, `+SECCOMP`, and `+SELINUX` feature tokens;
- CRIU with first version line `Version: 4.1.1` and command profile
  `podman-criu-rootful-v1`.

The host must make `/usr/sbin` available in `PATH`, and `criu check` must pass in
the same rootful execution context that will invoke Podman. The adapter itself
checks `criu --version` but does not run `criu check`; the gate must therefore be
run separately. The adapter checks AppArmor/SELinux/seccomp state and the seccomp
path, but it does not fingerprint the contents of AppArmor policy or the seccomp
file. The smoke container itself runs as uid/gid `65532`, drops every capability,
enables `no-new-privileges`, has no network, and has a read-only root filesystem.

## Implementation and gate boundaries

The runtime libraries are built as `//src/hypervisor:linux_runtime` and
`//src/hypervisor:podman_adapter`; the production
`//src/hypervisor:gravesentinel_hypervisor` composes them with storage, chunking,
create/restore, maintenance, the operation journal, and IPC. The dated stage-06
adapter helper still tests the runtime layer directly. The opt-in
`//tests/integration:hypervisor_scheduler_e2e` target instead starts the production
binary and drives functional operations through `SchedulerClient`.

The process-boundary target completed successfully in the qualified rootful context
on 2026-10-24 and emitted
`RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed`. This is the stage-12
production-daemon E2E evidence; the older direct and adapter qualification records
remain evidence for their narrower boundaries.

`scripts/check_testbed.sh` is a prerequisite screen, not an exact implementation
of the adapter fingerprint. It checks Linux/x86-64, the exact kernel, CPU
availability, cgroup v2/systemd and cpuset, Podman 5.4.2/amd64/local environment,
rootful uid 0, a `crun 1.21` version prefix with `+CRIU`, and a successful
`criu check`. It does not enforce the exact Debian release/codename or Podman
build origin, CRIU 4.1.1, the crun commit and full required feature set, the exact
controller set, or the adapter's AppArmor/SELinux/seccomp/capability profile.
Consequently, the direct smoke can pass outside the exact adapter profile; the
adapter-mode smoke closes this gap by executing the adapter's strict profile
validation.

## Reproducing the qualification

Build the fixture and process-boundary E2E binaries as the normal repository user;
do not run Bazel as root:

```text
bazel build \
  //src/hypervisor:gravesentinel_hypervisor \
  //tests/integration:hypervisor_e2e_driver \
  //tests/integration/fixture:counter_fixture
```

Run the read-only prerequisite gate in the intended rootful context:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  scripts/check_testbed.sh
```

Run the destructive smoke directly, reusing the user-owned Bazel output:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/podman_checkpoint_smoke.sh \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh
```

Run the complete production process/IPC E2E separately (expected duration 3–10
minutes):

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/hypervisor_scheduler_e2e.sh \
  --hypervisor bazel-bin/src/hypervisor/gravesentinel_hypervisor \
  --driver bazel-bin/tests/integration/hypervisor_e2e_driver \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh
```

The manual Bazel targets are available for CI or another process that is already
running in the correct rootful environment:

```text
bazel test //tests/integration:testbed_check --test_output=streamed
bazel test //tests/integration:podman_checkpoint_smoke \
  --test_output=streamed
bazel test //tests/integration:podman_adapter_smoke \
  --test_output=streamed
bazel test //tests/integration:hypervisor_scheduler_e2e \
  --test_output=streamed
```

They carry the `manual`, `requires-podman`, `exclusive`, and `local` tags and are
excluded from `bazel test //...`. Running `sudo bazel` is not the canonical local
workflow because it creates root-owned Bazel state and may select a different
Bazel installation or cache.

## Qualified command boundary

The smoke creates the source container with the following security and resource
profile; names and the image reference are unique per run:

```text
podman run --detach --name ORIGINAL_NAME --label OWNERSHIP_LABEL \
  --network=none --read-only --cap-drop=all --passwd=false \
  --security-opt=no-new-privileges --cpuset-cpus=0 \
  FIXTURE_IMAGE --threads 2 --interval-ms 250
```

`--passwd=false` is mandatory for this `scratch` image with `USER 65532:65532`.
Without it, Podman 5.4.2 injects a bind mount from the source container's
`userdata/passwd`. Once the original container is removed as required, import
restore fails because that source path no longer exists. Disabling passwd
injection makes the exported checkpoint self-contained for this profile.

The exact qualified checkpoint and restore argv forms are:

```text
podman container checkpoint --compress=none --leave-running \
  --export CHECKPOINT_TAR ORIGINAL_NAME
podman container restore --import CHECKPOINT_TAR --name RESTORED_NAME
```

The production adapter validates the snapshot's immutable source image identity
with `podman image inspect` before restore. The corresponding image must therefore
remain available in the local Podman image store even though the original
container has been removed.

The negative path uses the same restore form with an invalid archive and a unique
negative-test name. Production code must invoke these as argv through a process
runner, never by concatenating a shell command.

## Observed results

Two complete rootful runs ended with:

```text
RESULT: QUALIFIED Podman/CRIU checkpoint smoke passed
```

| Observation | First run | Immediate repeat |
| --- | --- | --- |
| Archive size | 202752 bytes | 206848 bytes |
| Archive entries | 44 | 44 |
| Counter before checkpoint | 46 | 44 |
| Original counter after checkpoint returned | 60 | 50 |
| Restored counter | 52 | 50 |
| Fixture nonce before checkpoint | `858134067962` | `1007421957965` |
| Restored nonce | continuity check passed | `1007421957965` |
| Original container ID | `1b3d40a82db1894dc7cb988c552b28607eeabe926bfbeaf472b6444da9406f4f` | `7bf6e11de490f5d00996fc15369383128ae54e5262d8f5053bbb66a33ecfd9b5` |
| Restored container ID | `b76546f0d0b037f7185075e7a8dff7ca30d1041e3f1f55d1ce3b86c6b86497df` | `fb96bc07152c2ccc57a49736b43f7b7f1741b3a7e03bb388a1b6a31c0798caf8` |

In both runs:

1. the fixture's main thread and two worker threads reported a CPU bitmask of
   `1`, proving actual execution only on CPU 0;
2. a timestamped log record proved that the original process advanced after the
   checkpoint command returned;
3. the original container and its cgroup were absent before restore;
4. invalid import failed without leaving a container;
5. valid import used the requested new name and produced a container ID different
   from the original;
6. the restored memory nonce matched the checkpointed nonce, and its counter
   continued beyond the pre-checkpoint observation rather than starting fresh;
7. restored execution remained on CPU 0;
8. containers, image, checkpoint archive, temporary files, and observed cgroups
   were removed. The second run therefore started without leftovers from the
   first.

The direct stage-02 shell flow proves absence with ordinary
`podman container exists` checks only; it does not issue
`podman container exists --external`. Ordinary and external Podman-object
absence is proved only by the production adapter path used by the stage-06
adapter smoke.

## Observed opaque artifact

The repeat run produced the following 44 tar entries:

```text
artifacts/
devshm-checkpoint.tar
config.dump
spec.dump
network.status
stats-dump
checkpoint/
checkpoint/cgroup.img
checkpoint/core-1.img
checkpoint/core-2.img
checkpoint/core-3.img
checkpoint/descriptors.json
checkpoint/fdinfo-2.img
checkpoint/files.img
checkpoint/fs-1.img
checkpoint/ids-1.img
checkpoint/ifaddr-10.img
checkpoint/inventory.img
checkpoint/ip6tables-10.img
checkpoint/ipcns-var-11.img
checkpoint/iptables-10.img
checkpoint/mm-1.img
checkpoint/mountpoints-13.img
checkpoint/netdev-10.img
checkpoint/netns-10.img
checkpoint/nftables-10.img
checkpoint/pagemap-1.img
checkpoint/pages-1.img
checkpoint/pstree.img
checkpoint/route-10.img
checkpoint/route6-10.img
checkpoint/rule-10.img
checkpoint/seccomp.img
checkpoint/timens-0.img
checkpoint/tmpfs-dev-100.tar.gz.img
checkpoint/tmpfs-dev-101.tar.gz.img
checkpoint/tmpfs-dev-102.tar.gz.img
checkpoint/tmpfs-dev-92.tar.gz.img
checkpoint/tmpfs-dev-94.tar.gz.img
checkpoint/tmpfs-dev-96.tar.gz.img
checkpoint/tmpfs-dev-98.tar.gz.img
checkpoint/tmpfs-dev-99.tar.gz.img
checkpoint/utsns-12.img
rootfs-diff.tar
```

This list is a qualification observation, not a stable CCM format or a list of
required entry names. The production system must treat the Podman export as
immutable opaque bytes: it may chunk, hash, store, and byte-identically reassemble
them, but must not extract, normalize, reorder, or synthesize archive entries.
Listing entries is allowed only as a diagnostic operation that does not modify
the artifact.

## Stage 06 production adapter smoke

The production Linux runtime and Podman adapter are built as
`//src/hypervisor:linux_runtime` and `//src/hypervisor:podman_adapter`. Their
non-privileged tests use a scripted process runner and do not require Podman:

```text
bazel test //tests/unit:linux_runtime_test //tests/unit:podman_adapter_test
bazel build //tests/integration:podman_adapter_smoke
```

The separate adapter smoke reuses the stage-02 fixture and continuity/cgroup
checks, but delegates cold start, capture, stop/remove, failed-restore cleanup and
valid restore to the production adapter and process runner. Build as the normal
user and run the built artifacts in the qualified rootful context:

```text
bazel build \
  //tests/integration/fixture:counter_fixture \
  //tests/integration:podman_adapter_smoke_driver

sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/podman_checkpoint_smoke.sh \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh \
  --adapter-helper bazel-bin/tests/integration/podman_adapter_smoke_driver
```

A CI process already running in the rootful profile may use:

```text
bazel test //tests/integration:podman_adapter_smoke --test_output=streamed
```

The target is tagged `manual`, `requires-podman`, `exclusive`, and `local` and is
therefore excluded from `bazel test //...`. A successful adapter run ends with:

```text
RESULT: QUALIFIED production PodmanAdapter smoke passed
```

**QUALIFIED for stage 06 on 2026-10-22.** The rootful prerequisite gate passed
with the documented kernel, CPU/cgroup, Podman 5.4.2, crun 1.21 and CRIU 4.1.1
profile. An initial adapter run failed closed on an AppArmor fingerprint mismatch:
the rootful measurement is `apparmorEnabled=true`, unlike the earlier rootless
observation. After correcting the canonical fingerprint to the measured rootful
security profile, the production adapter smoke completed with:

```text
RESULT: QUALIFIED production PodmanAdapter smoke passed
```

The run exercised production cold start, capture with `--leave-running`, exact
incarnation stop/remove, invalid-restore cleanup and ordinary/external absence
proof, valid import restore, state continuity, container/checkpoint CPU
isolation, cgroup cleanup and final resource cleanup.

The adapter smoke's `ColdStartSpec` contains no workload environment variables.
It therefore does not exercise the transient `0600` env-file against real
Podman. Non-privileged scripted unit tests cover env-file construction, mode,
secret exclusion from Podman's process environment/argv, and successful-path
removal; an underlying unlink failure still produces an operation error and only
a best-effort destructor retry.

## Stage 12 production hypervisor + SchedulerClient E2E

**QUALIFIED on 2026-10-24.** The manual rootful
`//tests/integration:hypervisor_scheduler_e2e` run ended with:

```text
PASS: all SchedulerClient/process-boundary scenarios passed
RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed
```

The suite started the real `gravesentinel_hypervisor` subprocess and used
`SchedulerClient` for handshake, registration, operation submission and polling,
and unregistration. It covered leave-running checkpoint continuity across memory,
local files, and `/dev/shm`; fallback from a corrupted newest checkpoint to the
previous generation; incompatible-snapshot cold start from an explicit spec; and
three-generation retention with grace-based chunk collection. Every scenario
checked CPU placement and single runtime incarnation. The final marker also
requires successful cleanup of owned containers, images, processes, cgroups,
mounts, sockets, and temporary storage.

## Adapter operation limits

The default production adapter limits are:

- profile and inspection timeout: 10 seconds each;
- checkpoint/restore/cold-start timeout: 5 minutes;
- stop/remove cleanup timeout: 30 seconds;
- TERM-to-KILL grace period: 2 seconds;
- JSON and stdout capture: 1 MiB each; stderr capture: 64 KiB;
- at most 256 adapter-supplied arguments excluding the executable, 16 KiB per
  argument;
- at most 256 environment entries, 64 KiB per value.

The 256-argument process limit includes the fixed Podman command-profile
arguments and image reference. It leaves at most 241 workload arguments without
an env-file and 239 with an env-file. Higher-level registration validation may
impose smaller per-string and aggregate limits.

These deadlines apply to commands invoked through `PosixProcessRunner`. The
direct qualification shell does not wrap each `podman build`, inspect,
checkpoint, restore, stop, or remove command in a per-command timeout. A Bazel
smoke target has an overall `timeout = "long"`, but a direct invocation can still
block in an individual host command.

## Unsupported modes and requalification limits

Rootless mode is not supported by the MVP profile. Even after
`criu check --unprivileged` and cpuset delegation succeeded, Podman 5.4.2 rejected
the actual operation with:

```text
Error: checkpointing a container requires root
```

The qualification does not establish compatibility for remote Podman, a
different kernel/package/runtime version, another architecture, cgroup v1,
another cgroup manager, another CRIU capability profile, SELinux/AppArmor policy
changes, a different seccomp policy, GPUs/devices, TCP connections, or critically
mutable shared volumes. Only CPU 0 was exercised by the smoke; availability of
CPUs `0..7` was checked for the planned MVP allocation profile.

Any change to the kernel, Podman, CRIU, crun, privilege model, cgroup setup,
security policy, source-image semantics, or command profile requires the
prerequisite gate and complete smoke to be rerun. An unknown combination must
fail as incompatible rather than attempt an unsafe restore.
