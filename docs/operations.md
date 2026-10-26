# Hypervisor operations and recovery

## Service model

`gravesentinel_hypervisor` is a foreground process. It does not fork, write a PID
file, or provide a remote shutdown RPC. A supervisor should start it directly and
send `SIGTERM` for graceful shutdown.

The service owns:

- the Unix socket, default `/run/gravesentinel/hypervisor.sock`;
- the process lock, `STORAGE/hypervisor.lock`;
- checkpoint namespaces under `STORAGE/checkpoint-data`;
- the durable async journal, `STORAGE/operations.jsonl`;
- rootful Podman/CRIU operations for registered runtime names.

Only one daemon may use a storage root. The process lock is held for its lifetime.
A stale socket left by `SIGKILL` is replaced only after the restarted daemon
acquires the storage lock. Do not share the storage root between hosts or daemon
instances.

## Ownership and permissions

The supported runtime profile runs the hypervisor as UID 0. Keep the storage root
owned by root and inaccessible to the scheduler. The scheduler needs traversal
permission on the socket parent plus access allowed by `--socket-mode` and
`--socket-gid`.

The socket's filesystem permissions are not authentication. Configure
`--scheduler-uid` to the exact scheduler UID. The server checks it using
`SO_PEERCRED`; `SchedulerClient` independently requires the server UID to be 0 by
default.

Recommended baseline:

```text
/run/gravesentinel                 root:gravesentinel-scheduler 0750
/run/gravesentinel/hypervisor.sock root:gravesentinel-scheduler 0660
/var/lib/gravesentinel             root:root                     0700
```

Use numeric UID/GID values in daemon CLI options. Do not put the storage root on
NFS or another shared/distributed filesystem; the locking and durability contract
is local-filesystem oriented and the MVP supports one host only.

## systemd example

Create a dedicated scheduler user/group first and substitute their numeric UID/GID
for `1234`. Install the release binary at
`/usr/local/libexec/gravesentinel_hypervisor`.

```ini
[Unit]
Description=GraveSentinel checkpoint hypervisor
After=local-fs.target
RequiresMountsFor=/var/lib/gravesentinel

[Service]
Type=simple
User=root
Group=gravesentinel-scheduler
RuntimeDirectory=gravesentinel
RuntimeDirectoryMode=0750
StateDirectory=gravesentinel
StateDirectoryMode=0700
UMask=0077
Environment=HOME=/root
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ExecStart=/usr/local/libexec/gravesentinel_hypervisor \
  --socket /run/gravesentinel/hypervisor.sock \
  --storage /var/lib/gravesentinel \
  --podman /usr/bin/podman \
  --criu /usr/sbin/criu \
  --runtime-profile podman-criu-rootful-v1 \
  --scheduler-uid 1234 \
  --socket-gid 1234 \
  --socket-mode 0660 \
  --gc-grace-seconds 3600
Restart=on-failure
RestartSec=2s
KillSignal=SIGTERM
TimeoutStopSec=10min

[Install]
WantedBy=multi-user.target
```

`Type=simple` is required because the daemon does not self-fork. A long
`TimeoutStopSec` lets accepted checkpoint/restore work quiesce. Do not add
`NoNewPrivileges=yes`, capability bounding, mount-namespace restrictions, or
write-path restrictions without requalifying Podman/CRIU; generic hardening can
silently break the rootful runtime profile. The exact host privilege fingerprint
is in [`testbed.md`](testbed.md).

After installation:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now gravesentinel-hypervisor.service
sudo systemctl status gravesentinel-hypervisor.service
sudo journalctl -u gravesentinel-hypervisor.service
```

## Startup and readiness

Startup order is deliberately fail-closed:

1. parse and validate CLI options;
2. create socket/storage parent directories;
3. acquire `hypervisor.lock`;
4. open checkpoint storage;
5. repeat bounded startup recovery until no work remains;
6. recover/open the operation journal;
7. create service and IPC components;
8. print `startup ready` and listen.

Do not treat process existence as readiness. Use `SchedulerClient::connect()` and
`get_health()`. A second daemon on the same storage exits with code 4. Storage or
journal initialization failures exit before readiness.

Registrations and runtime-incarnation fences are in memory, not persisted. After every
daemon restart, the scheduler must reconnect, replay its desired registrations, and
call `get_runtime_status()` for each registration before submitting new destructive
runtime operations. A successful present status adopts the owned immutable Podman
incarnation; `absent` clears the fence.

## Graceful and crash recovery

`SIGTERM`/`SIGINT` stop IPC acceptance, stop the operation store, shut down the
service, remove the owned socket, and release the process lock. A supervisor may
send `SIGKILL` only after its stop timeout, which converts in-flight work into
crash-recovery cases.

On startup, storage recovery immediately removes exact canonical publication
temporaries, removes stale operation staging, and performs bounded conservative chunk
GC after the configured grace period. It never publishes an incomplete manifest. Previously committed manifests
remain the source of truth.

Operation journal recovery is conservative:

- recovered `queued` becomes terminal failed/interrupted and is not executed;
- recovered `running` becomes `indeterminate` and is not automatically retried;
- terminal records remain terminal;
- repeating the same canonical request with the same `RequestId` returns the same
  `OperationId` and does not duplicate the side effect.

For `indeterminate`, inspect the logical container's committed manifests and call
`get_runtime_status()` to reconcile the owned Podman runtime incarnation, then make an
explicit scheduler/operator decision. Never submit a new destructive request ID merely
because the old client connection disappeared. Resubmit an ambiguously acknowledged
operation only with its original `RequestId`. The current MVP does not perform
phase-aware automatic classification of interrupted restore or stop as succeeded.

## Upgrade policy

Protocol version 3 has exact envelopes and no negotiation. The persisted manifest,
storage layout, and operation journal are compatibility-sensitive. Unless release
notes explicitly state compatibility, do not perform a rolling mixed-version
upgrade and do not let two versions touch the same storage.

Safe single-host sequence:

1. stop new scheduler submissions;
2. poll known operations to terminal state and record their request/operation IDs;
3. stop the scheduler or disable reconnect loops;
4. send `SIGTERM` to the hypervisor and require a clean exit;
5. take an offline backup/snapshot of the private storage root;
6. install a scheduler/hypervisor pair supporting the same protocol and persisted
   formats;
7. run the non-privileged release gate and requalify the rootful testbed after any
   protocol, lifecycle, runtime, kernel, security, or command-profile change;
8. start the hypervisor and require a healthy protocol-v3 response;
9. replay desired registrations and inspect/adopt runtime state;
10. reconcile known `indeterminate` operations, then resume new operations.

If the old daemon crashed rather than stopped cleanly, reconcile every recovered
`indeterminate` operation before allowing a logically equivalent new request. Protocol
v3 runtime status and stop/remove changed privileged lifecycle behavior, so the
2026-10-25 rootful markers from protocol v2 do not qualify the current code; rerun all
affected destructive suites before release.
There is no online journal migration, cross-host failover, or inter-host checkpoint
migration in the MVP.

## Logging

The daemon logs to stderr. Under `systemd`, journald captures it. Startup and fatal
messages are plain text. Lifecycle transitions are structured as:

```text
[LEVEL] message key="value" ...
```

Useful correlation fields include `container_id`, `request_id`, `operation_id`,
`from_state`, `to_state`, `error_code`, `retryable`, and `diagnostic`. Values are
quoted and control characters are sanitized. The logger has no dynamic level
configuration in the MVP.

Runtime environment secrets are not placed in Podman argv/environment by the
adapter; they are written to a transient mode-`0600` env file and removed. Do not
use secret values as IDs, image names, mount paths, or operator-provided
diagnostics.

## Diagnostics

Start with:

```sh
sudo env PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  scripts/check_testbed.sh
sudo systemctl status gravesentinel-hypervisor.service
sudo journalctl -u gravesentinel-hypervisor.service --since today
sudo podman info
sudo criu check
```

Then verify:

- the scheduler UID equals `--scheduler-uid`;
- parent directory, socket GID/mode, and `SO_PEERCRED` expectations agree;
- only one daemon owns the storage lock;
- CPUs `0..7` are available in the daemon's effective affinity/cpuset;
- the exact Podman/crun/CRIU/kernel fingerprint matches `testbed.md`;
- sufficient local storage and inodes are available;
- Podman contains at most one expected runtime incarnation per logical container.

Manual acceptance wrappers retain their temporary diagnostics directory on a
functional failure and print its path plus the daemon log. They still perform
ownership-checked runtime cleanup; a cleanup failure fails qualification.

## Unsupported operational modes

The MVP does not support:

- rootless or remote Podman;
- cgroup v1 or an unqualified cgroup manager/security profile;
- TCP transport between scheduler and hypervisor;
- restore of live TCP sessions;
- GPU or arbitrary device checkpoint/restore;
- atomic snapshot/rollback of critically mutable shared volumes;
- storage shared by multiple daemons or hosts;
- inter-host migration, rolling mixed-version upgrade, or automatic failover;
- arbitrary logical CPU layouts outside the four standard pairs.
