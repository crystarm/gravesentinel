# ADR 0001: Podman/CRIU runtime profile and opaque checkpoint artifact

- Status: Accepted
- Date: 2026-10-21
- Decision owners: Checkpoint Cache Manager team

## Context

CCM must invoke a reproducible local Podman/CRIU workflow before implementing a
production adapter. Podman, CRIU, the OCI runtime, kernel, cgroup mode, privilege
mode, architecture, and security policy all influence compatibility. The
exported checkpoint belongs to the runtime and is not a CCM-defined file format.

The stage 02 fixture and smoke were executed twice end to end on the target host.
Both runs passed checkpoint with `--leave-running`, source removal, import under a
new name, in-memory state continuity, CPU-affinity checks, negative restore, and
bounded cleanup. Detailed observations are recorded in `docs/testbed.md`.

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
unsupported until independently requalified. Stage 06 may implement real Podman
support only for this accepted profile and must fail closed for unknown profiles.

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

Every committed checkpoint must eventually retain enough non-secret metadata to
reject an unqualified restore environment. The runtime fingerprint must cover at
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

Exact field matching policy belongs to the adapter stage, but an unknown or
unsupported combination must fail as incompatible rather than attempt a restore.
Secrets, environment values, credential-bearing host paths, and arbitrary stderr
are not fingerprint fields.

## Failed restore and cleanup

Qualification proved that a deliberately invalid import fails without leaving a
container. Before valid restore, the original instance and its observed cgroup
must be absent. Import must create the requested new name and a runtime container
ID distinct from the original.

The smoke generates an unpredictable per-run suffix and removes a Podman object
only after its ownership label exactly matches that run. A name match alone is
insufficient. Cleanup failures fail the gate, and an indeterminate result forbids
another start for the same logical `ContainerId`. This is the reference safety
behavior for the stage 06 adapter.

## Consequences

- Stage 06 is unblocked for the exact accepted rootful profile only.
- Deployments must deliberately provision the rootful CRIU capability model,
  `/usr/sbin` in `PATH`, cgroup v2/systemd, and cpuset availability; repository
  scripts do not modify the host.
- Rootless Podman 5.4.2 is explicitly unsupported for checkpoint creation.
- Runtime, kernel, privilege, cgroup, or security-policy changes require the
  prerequisite gate and full destructive smoke to be rerun.
- Preserving opaque bytes limits format-specific optimization but avoids coupling
  storage correctness to undocumented Podman/CRIU internals.
- Container launch must preserve `--passwd=false` for the qualified scratch/nonroot
  fixture semantics unless a replacement profile is separately qualified.
