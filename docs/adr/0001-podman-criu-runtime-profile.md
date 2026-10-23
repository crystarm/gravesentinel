# ADR 0001: Podman/CRIU runtime profile and opaque checkpoint artifact

- Status: Accepted
- Date: 2026-10-21
- Decision owners: Checkpoint Cache Manager team

## Context

CCM required a reproducible local Podman/CRIU workflow before accepting and
implementing a production adapter. Podman, CRIU, the OCI runtime, kernel, cgroup
mode, privilege mode, architecture, and security policy all influence compatibility. The
exported checkpoint belongs to the runtime and is not a CCM-defined file format.

The stage 02 fixture and smoke were executed twice end to end on the target host.
Both runs passed checkpoint with `--leave-running`, source removal, import under a
new name, in-memory state continuity, CPU-affinity checks, negative restore, and
bounded cleanup. Detailed observations are recorded in
[testbed.md](../testbed.md).

Rootless setup was investigated but is not viable for this version: Podman 5.4.2
rejects the actual checkpoint operation with `checkpointing a container requires
root`, even when CRIU's unprivileged check and cpuset delegation pass.

## Decision

The accepted MVP runtime profile is:

- local **rootful** Podman 5.4.2, invoked as uid 0;
- crun 1.21, commit `10269840aa07fb7e6b7e1acff6198692d8ff5c88`, with
  advertised CRIU integration;
- CRIU 4.1.1 at `/usr/sbin/criu`, with `criu check` passing in the rootful
  invocation context;
- Debian 13, Linux x86_64, kernel `6.12.111+deb13-amd64`;
- cgroup v2 managed by systemd with the `cpuset` controller available;
- a local static CPU-only workload without TCP, devices, GPUs, or critically
  mutable shared-volume state;
- checkpoint export with `--leave-running`, followed by removal of the original
  container and import restore under a new name.

Rootless Podman, remote Podman, and any version or host-profile variation are
unsupported until independently requalified. The production implementation accepts
only the profile name `podman-criu-rootful-v1` and fails closed for every other
profile.

## Implementation and verification notes

These notes record the current implementation boundary without changing the
accepted decision or the historical qualification result.

The successful stage-02 runs on 2026-10-21 and the production adapter run on
2026-10-22 are dated recorded evidence for the measured host. They are not a
claim that a current deployment has been rechecked. This repository retains the
observations and success markers, but not raw qualification logs, signed or
checksummed host reports, checkpoint hashes, or a CI evidence bundle from those
runs. A current deployment is verified only by rerunning the prerequisite gate
and the applicable complete destructive smoke in its intended rootful context.

The production libraries exist as `//src/hypervisor:linux_runtime` and
`//src/hypervisor:podman_adapter`, and
`//src/hypervisor:gravesentinel_hypervisor` now composes them with storage,
chunking, the create/restore coordinators, `CheckpointService`, the durable
operation journal, and the `SOCK_SEQPACKET` IPC server. The scheduler client and
server perform peer-UID checks, and only the hypervisor side invokes rootful
Podman/CRIU.

The dated adapter smoke still exercises the production runtime libraries through a
purpose-built integration helper. It is not evidence that the complete production
hypervisor + IPC + journal + storage + service + Podman/CRIU flow was run end to end;
no such production end-to-end execution is claimed here.

The prerequisite gate is intentionally less exact than the adapter profile. It
checks the main host capabilities and runs `criu check`, but it does not enforce
the exact Debian release/codename and Podman build origin, CRIU version, crun
commit and complete required feature set, exact cgroup controller set, or the
adapter's AppArmor/SELinux/seccomp/capability observations. The direct smoke can
therefore pass outside the exact adapter profile. The adapter-mode smoke also
executes the adapter's strict validation and is the relevant gate for the
implemented production library.

## Qualified command boundary

The source container profile includes:

```text
["podman", "run", "--detach", "--name", ORIGINAL_NAME,
 "--label", OWNERSHIP_LABEL, "--network=none", "--read-only",
 "--cap-drop=all", "--passwd=false",
 "--security-opt=no-new-privileges", "--cpuset-cpus=0",
 FIXTURE_IMAGE, "--threads", "2", "--interval-ms", "250"]
```

`--passwd=false` is part of the qualified profile, not an optional hardening flag.
Without it, Podman injects a bind mount to the original container's generated
`userdata/passwd`; removing that container before restore leaves the exported
spec referring to a nonexistent host path.

The exact checkpoint and restore forms accepted for the production adapter are:

```text
["podman", "container", "checkpoint", "--compress=none",
 "--leave-running", "--export", CHECKPOINT_TAR, ORIGINAL_NAME]
["podman", "container", "restore", "--import", CHECKPOINT_TAR,
 "--name", RESTORED_NAME]
```

The adapter must use an argv process runner and must not concatenate a shell
command. A disconnect, nonzero exit, malformed result, or indeterminate cleanup
is never converted to success.

## Adapter command profile v1

The production adapter implements only `podman-criu-rootful-v1`. The composed daemon
rejects any other `--runtime-profile` value and defaults to `/usr/bin/podman` and
`/usr/sbin/criu` for the executable paths. In addition to the two qualified
checkpoint/restore commands above, its cold-start argv is:

```text
["podman", "run", "--detach", "--name", RUNTIME_NAME,
 "--label", "io.gravesentinel.ccm.managed=true",
 "--label", "io.gravesentinel.ccm.logical-id=" + CONTAINER_ID,
 "--network=none", "--read-only", "--cap-drop=all", "--passwd=false",
 "--security-opt=no-new-privileges", "--cpuset-cpus=" + CONTAINER_CPU,
 ["--env-file", TRANSIENT_ENV_FILE], IMAGE_REFERENCE, WORKLOAD_ARGV...]
```

The optional environment file is created under the private adapter staging root
with mode `0600`. Its contents are not logged or copied into manifests/storage
metadata, Podman's process environment, or command-line arguments; only the file
path is passed through `--env-file`. After the synchronous Podman invocation the
adapter attempts to unlink the file, and an unlink failure makes the operation
fail. The destructor retries best-effort, but if both unlink attempts fail the
file can remain and requires trusted staging cleanup. Values containing NUL, CR,
or LF are rejected. Mounts and arbitrary runtime options are rejected by v1
instead of being mapped to an unqualified command form.

Operational inspection and cleanup use these exact argv forms:

```text
["podman", "version", "--format=json"]
["podman", "info", "--format=json"]
["criu", "--version"]
["podman", "container", "inspect", "--format=json", REF_OR_ID]
["podman", "image", "inspect", "--format=json", IMAGE]
["podman", "stop", "--time=10", EXPECTED_RUNTIME_ID]
["podman", "rm", EXPECTED_RUNTIME_ID]
["podman", "container", "exists", RUNTIME_NAME]
["podman", "container", "exists", "--external", RUNTIME_NAME]
```

The adapter's implemented acceptance checks require:

- Podman client version `5.4.2`, `OsArch=linux/amd64`, `BuildOrigin=Debian`, and
  Podman info version `5.4.2`;
- Podman host distribution `debian`, version `13`, codename `trixie`, architecture
  `amd64`, and kernel `6.12.111+deb13-amd64`;
- local rootful mode, null uid/gid mappings, cgroup v2 with the `systemd` manager,
  and exactly `cpu cpuset hugetlb io memory misc pids rdma` controllers;
- AppArmor enabled, SELinux disabled, seccomp enabled with
  `/usr/share/containers/seccomp.json`, and the qualified Podman capability set;
- `crun` with first version line `crun version 1.21`, commit
  `10269840aa07fb7e6b7e1acff6198692d8ff5c88`, and required `+APPARMOR`, `+CAP`,
  `+CRIU`, `+SECCOMP`, and `+SELINUX` tokens;
- CRIU with first version line `Version: 4.1.1` and command profile
  `podman-criu-rootful-v1`.

It records a canonical fingerprint and requires exact equality before restore.
The adapter invokes `criu --version` but does not run `criu check`; the separate
prerequisite gate remains required. It validates the enabled/disabled security
state and seccomp path, but does not fingerprint AppArmor policy contents, the
contents of the seccomp file, or the host LSM policy set. Such policy changes
still require explicit requalification even when the canonical fingerprint is
unchanged.

Restore also performs `podman image inspect` on the snapshot's immutable source
image identity. The source image must remain available in the local Podman image
store after the original container is removed and until restore validation has
completed.

The production adapter-mode smoke qualified these adapter assumptions on the rootful
host on 2026-10-22 and ended with
`RESULT: QUALIFIED production PodmanAdapter smoke passed`. This remains dated
recorded evidence under the verification boundary above and must not be described as
a production daemon end-to-end test.

### Implemented limits and test coverage boundary

The default adapter limits are 10 seconds for profile and inspection commands,
5 minutes for checkpoint/restore/cold-start, 30 seconds for cleanup, and a
2-second TERM-to-KILL grace period. JSON and stdout are limited to 1 MiB each,
stderr to 64 KiB, adapter-supplied argv to 256 entries excluding the executable
with 16 KiB per entry, and process environment to 256 entries with 64 KiB per
value. Because the argv limit includes
the fixed Podman profile arguments and image reference, at most 241 workload
arguments fit without an env-file and 239 with an env-file. Higher-level
registration validation may impose smaller string and aggregate limits.

The direct stage-02 shell checks ordinary Podman container absence but does not
probe `podman container exists --external`. Ordinary and external absence is
proved by the production adapter path used in the stage-06 adapter smoke. The
adapter smoke uses an empty workload environment, so it does not exercise the
transient env-file against real Podman. Scripted unit tests cover creation, mode,
argument/process-environment exclusion, and successful removal; they do not turn
an underlying unlink failure into a guaranteed cleanup. The direct shell also has no
per-command timeout around each Podman build/inspect/checkpoint/restore/cleanup
invocation; Bazel
provides only an overall long test timeout for the corresponding test target.

## Artifact decision

A successful Podman export is an immutable opaque artifact:

- CCM stores the exact bytes produced by Podman;
- CCM may split those bytes into fixed-size chunks and hash them;
- CCM does not extract, normalize, reorder, or synthesize archive entries;
- a diagnostic qualification tool may list entries without changing the archive;
- restore receives a byte-identical reassembly of the export.

The qualification observed 44 tar entries, including Podman metadata, CRIU image
files, tmpfs images, and `rootfs-diff.tar`. Those names are diagnostic facts for
the qualified run, not a stable CCM contract. Production correctness must not
depend on an entry name or Podman's current archive organization.

## Compatibility fingerprint

Every committed checkpoint retains enough non-secret metadata to reject an
unqualified restore environment. The runtime fingerprint must cover at
least:

- machine architecture and kernel release;
- Podman version and build origin;
- CRIU version;
- OCI runtime name, version, commit, and relevant feature set;
- rootless/rootful mode and user-namespace mapping mode;
- cgroup version, manager, and required controllers;
- relevant SELinux/AppArmor/seccomp state;
- source image identity or digest;
- checkpoint command-profile version, including passwd-injection policy.

The adapter requires exact fingerprint equality; an unknown or unsupported
combination fails as incompatible rather than attempting a restore.
Secrets, environment values, credential-bearing host paths, and arbitrary stderr
are not fingerprint fields.

## Failed restore and cleanup

Qualification proved that a deliberately invalid import fails without leaving a
container. Before valid restore, the original instance and its observed cgroup
must be absent. Import must create the requested new name and a runtime container
ID distinct from the original.

The smoke generates an unpredictable per-run suffix and removes a Podman object
only after its ownership label exactly matches that run. The production adapter
also requires the first inspected immutable runtime ID to equal the expected
`RuntimeIncarnation`; a name or logical-ID label match alone is insufficient.
Before a restore attempt, production `confirm_absence` is deliberately read-only:
it requires both ordinary and external `podman container exists` probes to return
the documented not-found status and never stops or removes an unexpected live
container. Destructive `cleanup_failed_restore` is permitted only after the
coordinator proved absence and then initiated its own restore/cold-start attempt
under an uninterrupted per-container storage lease. It proves both ordinary
libpod-record and external containers/storage absence before fallback. Cleanup
failures fail the gate, and an indeterminate result carries
`incarnation_may_exist` and forbids another start for the same logical
`ContainerId`. The integration smoke separately observes and verifies cgroup
removal.

## Consequences

- Production runtime support is limited to the exact accepted rootful
  `podman-criu-rootful-v1` profile.
- Deployments must deliberately provision the rootful CRIU capability model,
  cgroup v2/systemd, and cpuset availability; repository scripts do not modify the
  host. The daemon defaults to `/usr/bin/podman` and `/usr/sbin/criu`, and alternate
  executable paths do not expand the qualified runtime profile.
- Rootless Podman 5.4.2 is explicitly unsupported for checkpoint creation.
- Runtime, kernel, privilege, cgroup, or security-policy changes require the
  prerequisite gate and full destructive smoke to be rerun.
- Preserving opaque bytes limits format-specific optimization but avoids coupling
  storage correctness to undocumented Podman/CRIU internals.
- Container launch must preserve `--passwd=false` for the qualified scratch/nonroot
  fixture semantics unless a replacement profile is separately qualified.
