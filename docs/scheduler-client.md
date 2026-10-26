# SchedulerClient integration

## Public Bazel boundary

The supported external C++ API is the root target:

```text
@gravesentinel//:scheduler_client
```

with the public include:

```cpp
#include <gravesentinel/scheduler_client.hxx>
```

The target exports the protocol value types needed by the client, but internal
core, storage, IPC, and Podman targets remain package-private. Consumers must not
depend on `//src/client:scheduler_client` or another `//src/...` target.

A development consumer can use Bzlmod as follows:

```starlark
module(name = "my_scheduler")

bazel_dep(name = "gravesentinel", version = "0.1.0")
local_path_override(
    module_name = "gravesentinel",
    path = "/absolute/path/to/gravesentinel",
)
```

```starlark
cc_binary(
    name = "scheduler",
    srcs = ["main.cxx"],
    deps = ["@gravesentinel//:scheduler_client"],
)
```

The checked example is under `examples/scheduler_client_consumer`. Run:

```sh
sh scripts/check_scheduler_client_boundary.sh
sh scripts/check_scheduler_client_consumer_runtime.sh
```

The first command builds the external consumer and proves that attempted external
dependencies on core, storage, and Podman targets fail visibility analysis. The
second starts a separate real hypervisor process and verifies that the external
consumer completes handshake and `GetHealth` over the Unix socket.

## Construction and peer authentication

```cpp
SchedulerClient(
    std::string socket_path,
    std::chrono::milliseconds rpc_timeout = std::chrono::seconds(3),
    std::chrono::milliseconds operation_timeout = std::chrono::minutes(30),
    std::optional<uid_t> expected_server_uid = std::nullopt);
```

`expected_server_uid = std::nullopt` means UID 0, not “accept any peer.” The
client checks the connected server with `SO_PEERCRED`. The hypervisor separately
checks the scheduler UID; socket mode/group permissions only control whether the
connection can be attempted.

`connect()` performs an eager connection and protocol-v3 handshake. Public RPC
methods also open and handshake a connection when no descriptor is active, so an
explicit `connect()` is useful for readiness but is not required before every
call. The class serializes calls with an internal mutex. It is movable, not
copyable; a moved-from object returns failure.

## Minimal health check

```cpp
#include <gravesentinel/scheduler_client.hxx>

#include <chrono>
#include <iostream>
#include <string>

int main() {
  gravesentinel::ccm::SchedulerClient client{
      "/run/gravesentinel/hypervisor.sock",
      std::chrono::seconds{3},
      std::chrono::minutes{30}};

  if (const auto connected = client.connect(); !connected) {
    std::cerr << connected.error().message << '\n';
    return 1;
  }
  if (const auto health = client.get_health(); !health) {
    std::cerr << health.error().message << '\n';
    return 1;
  } else if (health->state != gravesentinel::ccm::HealthState::healthy) {
    return 2;
  }
  return 0;
}
```

The complete compiling example also prints queue state and protocol version:
`examples/scheduler_client_consumer/main.cxx`.

## Registration and cold start

A `ContainerRegistration` binds:

- a scheduler-owned logical `ContainerId`;
- a stable Podman `RuntimeContainerRef`;
- checkpoint interval and `keep_last` policy;
- one standard CPU pair: container CPU `0/2/4/6` and matching checkpoint CPU
  `1/3/5/7`;
- an optional explicit `ColdStartSpec`;
- an optional runtime fingerprint.

At most four registrations are active. CPU assignments cannot overlap. A cold
start is possible only when `cold_start` is present; it specifies the image,
argv, environment, mounts, and validated runtime options. It is not synthesized
from hidden defaults. Mutable shared-volume contents are not checkpointed by CCM.

`register_container()` and `unregister_container()` are synchronous boolean
operations. Overloads accepting a `RequestId` provide correlation but these
mutations are not stored in the durable operation journal. Protocol v3 maps
service-side registration failures to `request_failed`, and the boolean API
returns only `false`; reconcile scheduler desired state before retrying an
ambiguous register/unregister exchange.

## Durable asynchronous operations

The detailed API is:

```cpp
std::expected<OperationId, RpcFailure>
submit_create_checkpoint(const ContainerId &, const RequestId &);

std::expected<OperationId, RpcFailure>
submit_restore_or_restart(const ContainerId &, const RequestId &);

std::expected<OperationId, RpcFailure>
submit_stop_container(const ContainerId &, const RequestId &);

std::expected<OperationStatus, RpcFailure>
get_operation(const OperationId &);

std::expected<RuntimeStatus, RpcFailure>
get_runtime_status(const ContainerId &);
```

Submission is acknowledged only after a `queued` record is durable. The
`OperationId` is deterministic for the request ID. Repeating the same request ID
with the same canonical method/container payload returns the same operation;
reusing it for another payload is rejected.

Poll until one of the terminal states:

| State | Meaning / scheduler action |
| --- | --- |
| `queued` | Durable but not yet executing; continue polling. |
| `running` | Execution began; a disconnect does not cancel it. |
| `succeeded` | Operation completed successfully. |
| `failed` | Definite terminal failure; inspect the stable error code. |
| `indeterminate` | A crash interrupted running work and its side effect cannot be proven; reconcile storage/runtime state and do not blindly replay. |

After restart, a recovered `queued` operation becomes a definite interrupted
failure and is not executed. A recovered `running` operation becomes
`indeterminate`. The same applies when a stop/remove command may have changed the
runtime but its final absence cannot be proven. The MVP deliberately does not infer
success from an observed runtime or publication phase and never automatically retries
destructive work.

A robust scheduler stores `{RequestId, OperationId}` before losing its own state,
reconnects after transport failure, and resumes `get_operation(OperationId)`. If
submission response delivery was ambiguous and no `OperationId` was received,
resubmit the exact operation with the same `RequestId`.

## Runtime observation and adoption

`get_runtime_status()` is a read-only RPC for a registered logical container. Its
strict result is one of:

| State | `exit_code` | Meaning |
| --- | --- | --- |
| `absent` | absent | Neither the normal nor external Podman namespace contains the configured runtime name. |
| `running` | absent | The owned runtime incarnation is present and running. |
| `exited` | integer | The owned runtime incarnation is present but its process exited with this code. |

A present result is returned only after the hypervisor validates GraveSentinel
ownership labels, the registered container cpuset, and the immutable Podman runtime
ID. The service stores that incarnation as its current in-memory fence. An `absent`
result clears any cached incarnation. Status inspection runs on the same per-container
worker as create, restore, and stop, so it cannot race those operations inside the
service.

Registrations are not persisted. After a daemon restart, reconnect, replay each desired
registration, then call `get_runtime_status()` before making a new runtime decision.
This inspect-and-adopt sequence lets later checkpoint, restore, or stop use the observed
incarnation rather than assuming that daemon memory reflects Podman state.

`submit_stop_container()` is a durable desired-state operation. It stops the expected
owned incarnation when running, removes it, and proves absence in both normal and
external Podman namespaces. If daemon memory has no incarnation after restart, the
operation first inspects and adopts the owned runtime; an already absent runtime is a
successful no-op. Foreign ownership, a changed runtime ID, a wrong cpuset, or external
presence fails closed. If mutation may have begun but final absence is not proven, the
operation is `indeterminate` and the scheduler must inspect again.

## Boolean wrappers

`create_checkpoint()`, `restore_or_restart()`, and `stop_container()` are convenience
methods. Each:

1. creates a random request ID;
2. submits the durable operation;
3. polls every bounded interval;
4. returns `true` only for `succeeded`;
5. returns `false` for terminal failure, `indeterminate`, transport/protocol
   failure, or operation timeout.

They are appropriate only when a binary success value is sufficient. They do
not expose the operation ID after returning and therefore are not the preferred
interface for a scheduler that must survive its own restart or perform explicit
reconciliation.

## Reconnect and retry rules

Each packet operation is bounded by `rpc_timeout`; wrapper polling is also
bounded by `operation_timeout`. The client closes a descriptor on unavailable,
deadline, or protocol errors and reconnects when a later call needs transport.
It may make one transport retry when connection failed before an exchange.
Async submission can repeat an ambiguous exchange safely because caller-owned request
IDs deduplicate the mutation. Read-only calls may also be repeated. Register/unregister disable retry
after a request may have reached the server.

The server bounds packet size, accepted-peer queue, requests per connection, and
worker count. It can deliberately set `close_connection=true`; the client closes
and handshakes a new connection on the next call.

## Protocol compatibility

The current wire protocol is exactly version 3. Handshake requires both version
3 and service name `gravesentinel_hypervisor`. There is no version negotiation
or TCP transport. Unknown versions, envelope fields, methods, and malformed or
oversized packets are rejected. Upgrade scheduler and hypervisor as a compatible
pair; see [`operations.md`](operations.md) for the stop/upgrade/re-register
sequence.

## Failure information

Detailed methods return `RpcFailure` with one of:

- `invalid_argument`;
- `unavailable`;
- `deadline_exceeded`;
- `permission_denied`;
- `protocol_error`;
- `server_busy`;
- `request_failed`;
- `internal_error`.

`OperationStatus::error` instead uses stable operation error codes such as
`resource_exhausted`, `storage_error`, `corrupted_snapshot`,
`incompatible_runtime`, or `restore_failed`. A transport failure describes the
RPC observation, not necessarily whether an already accepted operation ran.
