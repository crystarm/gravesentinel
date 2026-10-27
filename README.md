# GraveSentinel

GraveSentinel is a Linux Checkpoint Cache Manager (CCM) for a scheduler and a
separate privileged hypervisor process. The scheduler uses the public C++23
`SchedulerClient`; only `gravesentinel_hypervisor` invokes Podman and CRIU.

The MVP runtime profile is intentionally narrow: one Linux x86-64 host, local
rootful Podman/CRIU, cgroup v2 with `systemd`, eight available logical CPUs, and
CPU-only containers without restorable TCP sessions, devices, GPUs, or
critically mutable shared volumes. See [`docs/testbed.md`](docs/testbed.md) for
the exact qualified versions and limits.

## Prerequisites

Normal builds and non-privileged tests require:

- Linux x86-64;
- Bazel `8.8.2` (pinned by `.bazelversion`) with Bzlmod enabled;
- a C++23 compiler and standard library;
- `clang-format` compatible with the repository `.clang-format`.

Destructive integration tests additionally require the exact rootful Podman,
crun, CRIU, kernel, cgroup, CPU, and security profile documented in
[`docs/testbed.md`](docs/testbed.md). Run `scripts/check_testbed.sh` as root
before those tests. Never run Bazel through `sudo`; build as the repository user
and invoke only the prepared integration wrappers through `sudo`.

## Build and test

```sh
bazel build //...
bazel test //...
```

Optimized and sanitizer checks:

```sh
bazel build --config=release //...
bazel test --config=asan //:unit_tests --nocache_test_results
bazel test --config=ubsan //:unit_tests --nocache_test_results
bazel test --config=tsan //:unit_tests --nocache_test_results
```

The complete non-privileged release gate runs formatting, release build, normal
tests, all three sanitizer suites, the external-client visibility boundary, the
external runtime consumer, and the mandatory-path placeholder check:

```sh
sh scripts/release_gate.sh
```

Manual rootful suites are excluded from `bazel test //...`. Their build/run
commands and qualification markers are in
[`tests/integration/README.md`](tests/integration/README.md).

## Run the hypervisor

Build the foreground service as an ordinary user:

```sh
bazel build --config=release //:hypervisor
```

Display its exact options:

```sh
bazel-bin/src/hypervisor/gravesentinel_hypervisor --help
```

Production defaults are:

| Option | Default / behavior |
| --- | --- |
| `--socket PATH` | `/run/gravesentinel/hypervisor.sock` |
| `--storage DIR` | `/var/lib/gravesentinel` |
| `--podman PATH` | `/usr/bin/podman` |
| `--criu PATH` | `/usr/sbin/criu` |
| `--runtime-profile NAME` | only `podman-criu-rootful-v2` is accepted |
| `--scheduler-uid UID` | if omitted, only the daemon effective UID is accepted |
| `--socket-gid GID` | optional socket group owner |
| `--socket-mode OCTAL` | `0660` |
| `--gc-grace-seconds N` | `3600` |

Example, with numeric scheduler UID/GID `1234`:

```sh
sudo bazel-bin/src/hypervisor/gravesentinel_hypervisor \
  --scheduler-uid 1234 \
  --socket-gid 1234 \
  --socket-mode 0660
```

The daemon does not fork or daemonize. It acquires
`STORAGE/hypervisor.lock`, recovers `STORAGE/checkpoint-data`, opens the durable
`STORAGE/operations.jsonl` journal, then listens on the Unix
`SOCK_SEQPACKET` socket. `SIGTERM` and `SIGINT` request graceful shutdown after
accepted work reaches the service shutdown boundary. A second daemon using the
same storage root exits instead of sharing it.

The socket directory must be traversable by the scheduler, the socket mode/GID
must grant filesystem access, and `--scheduler-uid` must match the scheduler's
actual UID. Filesystem permission alone is insufficient: both peers also check
`SO_PEERCRED`. Keep the storage root private to the rootful service.

For a `systemd` unit, ownership, restart/recovery, upgrade, and diagnostics, see
[`docs/operations.md`](docs/operations.md).

## Use `SchedulerClient`

External Bzlmod consumers depend only on:

```text
@gravesentinel//:scheduler_client
```

and include:

```cpp
#include <gravesentinel/scheduler_client.hxx>
```

A minimal health check is available in
[`examples/scheduler_client_consumer/main.cxx`](examples/scheduler_client_consumer/main.cxx).
The example module uses a development `local_path_override` and can be built with:

```sh
sh scripts/check_scheduler_client_boundary.sh
```

Protocol v4 exposes durable async create, restore/restart, and stop/remove, plus
multi-CPU container cpusets.
Each submission uses a caller-owned `RequestId`, returns a durable `OperationId`,
and is observed through `get_operation()`. The convenience `bool` wrappers
generate a request ID, submit, poll until the configured operation timeout, and
return `true` only for `succeeded`; `false` intentionally collapses transport,
protocol, terminal failure, timeout, and `indeterminate`. Use the async API when
the scheduler needs reconciliation detail.

A registration carries a nonempty `container_cpus` set and one disjoint
`checkpoint_cpu`, all within logical CPUs `0..7`; active registrations must be
globally disjoint and at most four may exist. `get_runtime_status()` reports
`absent`, `running`, or `exited`; only `exited` contains an exit code. A successful
inspection validates ownership, the exact cpuset, and
the immutable Podman runtime ID, then adopts that incarnation into the service's
in-memory fence. This is how a scheduler restores runtime knowledge after replaying
a registration following daemon restart.

Connections are authenticated and recreated after closure. A disconnect does
not cancel accepted work. Resubmit an ambiguous async request only with the same
`RequestId`; never invent a new request ID for an operation that may already have
started. A recovered running operation or a stop/remove whose final runtime state
cannot be proven is conservatively `indeterminate` and is not replayed automatically.

See [`docs/scheduler-client.md`](docs/scheduler-client.md) for registration,
cold-start, retry, reconnect, and protocol details.

## Logs and diagnostics

The hypervisor writes startup/fatal diagnostics and structured transition lines
to stderr. Transition logs use a level prefix and quoted fields such as
`container_id`, `request_id`, `operation_id`, states, stable error code,
retryability, and diagnostic text. Under `systemd`, inspect them with:

```sh
journalctl -u gravesentinel-hypervisor.service
```

Do not place secrets in identifiers or diagnostics. Runtime environment values
are passed to Podman through a transient mode-`0600` env file and are not logged
by the adapter.

## Documentation

Start with [`docs/README.md`](docs/README.md). In particular:

- [`docs/architecture.md`](docs/architecture.md) — components and trust boundaries;
- [`docs/scheduler-client.md`](docs/scheduler-client.md) — public client API;
- [`docs/operations.md`](docs/operations.md) — service operation and recovery;
- [`docs/requirements-traceability.md`](docs/requirements-traceability.md) — MVP acceptance coverage;
- [`docs/release-gate.md`](docs/release-gate.md) — release and rootful gates;
- [`docs/testbed.md`](docs/testbed.md) — exact Podman/CRIU qualification profile.
