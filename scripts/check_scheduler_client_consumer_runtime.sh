#!/bin/sh
set -eu

if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
  repository_root=$BUILD_WORKSPACE_DIRECTORY
else
  script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
fi
consumer_root=$repository_root/examples/scheduler_client_consumer
bazel_executable=${BAZEL:-bazel}
work_directory=$(mktemp -d /tmp/gravesentinel-consumer.XXXXXX)
socket_path=$work_directory/hypervisor.sock
storage_root=$work_directory/storage
daemon_log=$work_directory/hypervisor.log
daemon_pid=

cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    attempts=0
    while kill -0 "$daemon_pid" 2>/dev/null && [ "$attempts" -lt 100 ]; do
      sleep 0.05
      attempts=$((attempts + 1))
    done
    kill -KILL "$daemon_pid" 2>/dev/null || true
    wait "$daemon_pid" 2>/dev/null || true
  fi
  if [ "$status" -ne 0 ] && [ -f "$daemon_log" ]; then
    echo "---- external consumer daemon log ----" >&2
    cat "$daemon_log" >&2
  fi
  rm -rf "$work_directory"
  exit "$status"
}
trap cleanup EXIT HUP INT TERM

(
  cd "$repository_root"
  "$bazel_executable" build //src/hypervisor:gravesentinel_hypervisor
)
(
  cd "$consumer_root"
  "$bazel_executable" build //:scheduler_client_consumer
)

hypervisor=$repository_root/bazel-bin/src/hypervisor/gravesentinel_hypervisor
consumer=$consumer_root/bazel-bin/scheduler_client_consumer
mkdir -p "$storage_root"
"$hypervisor" \
  --socket "$socket_path" \
  --storage "$storage_root" \
  --podman "$work_directory/nonexistent-podman" \
  --criu "$work_directory/nonexistent-criu" \
  --scheduler-uid "$(id -u)" \
  --socket-mode 0600 \
  >"$daemon_log" 2>&1 &
daemon_pid=$!

attempts=0
while [ ! -S "$socket_path" ] && [ "$attempts" -lt 200 ]; do
  kill -0 "$daemon_pid" 2>/dev/null || {
    echo "hypervisor exited before external consumer connection" >&2
    exit 1
  }
  sleep 0.05
  attempts=$((attempts + 1))
done
[ -S "$socket_path" ] || {
  echo "hypervisor socket did not become ready" >&2
  exit 1
}

consumer_output=$("$consumer" "$socket_path" "$(id -u)")
printf '%s\n' "$consumer_output"
printf '%s\n' "$consumer_output" | grep -F \
  'status=healthy queue=available protocol_version=' >/dev/null

kill -TERM "$daemon_pid"
if ! wait "$daemon_pid"; then
  echo "hypervisor did not exit successfully after external consumer check" >&2
  exit 1
fi
daemon_pid=
[ ! -e "$socket_path" ] || {
  echo "hypervisor socket remains after external consumer check" >&2
  exit 1
}

echo "SchedulerClient external runtime check passed"
