# Podman + CRIU testbed qualification

## Qualification status

**QUALIFIED on 2026-10-21.** The complete rootful checkpoint/export/restore
smoke passed twice on the target host. Both runs proved `--leave-running`
progress, removal of the original container before restore, in-memory state
continuity after import, CPU affinity, negative-restore cleanup, cgroup cleanup,
and idempotent resource cleanup.

Qualification applies only to the exact runtime profile and command boundary in
this document. A `+CRIU` feature in `crun --version` means that the OCI runtime
was compiled with CRIU integration; the successful smoke, not that feature flag,
is the evidence that the complete workflow works.

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
| Podman | local rootful Podman `5.4.2`, uid 0 |
| OCI runtime | `crun 1.21`, commit `10269840aa07fb7e6b7e1acff6198692d8ff5c88` |
| OCI runtime features | includes `+CRIU`, `+APPARMOR`, `+SELINUX`, `+SECCOMP` |
| CRIU | `4.1.1` at `/usr/sbin/criu` |
| Qualification process capabilities | `CapEff=000001ffffffffff`, `NoNewPrivs=0` |
| Podman security | AppArmor enabled, SELinux disabled, seccomp enabled with `/usr/share/containers/seccomp.json` |
| Host LSM list | `lockdown,capability,landlock,yama,apparmor,tomoyo,bpf,ipe,ima,evm` |
| Workload | static C++23 CPU-only fixture in a `scratch` image |

The host must make `/usr/sbin` available in `PATH`, and `criu check` must pass in
the same rootful execution context that will invoke Podman. The smoke container
itself runs as uid/gid `65532`, drops every capability, enables
`no-new-privileges`, has no network, and has a read-only root filesystem.

## Reproducing the qualification

Build the fixture as the normal repository user; do not run Bazel as root:

```text
bazel build //tests/integration/fixture:counter_fixture
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

The manual Bazel targets are available for CI or another process that is already
running in the correct rootful environment:

```text
bazel test //tests/integration:testbed_check --test_output=streamed
bazel test //tests/integration:podman_checkpoint_smoke \
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
incarnation stop/remove, invalid-restore cleanup and absence proof, valid import
restore, state continuity, container/checkpoint CPU isolation, cgroup cleanup and
final resource cleanup.

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
