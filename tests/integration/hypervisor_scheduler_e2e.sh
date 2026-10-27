#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --hypervisor PATH --driver PATH --fixture-binary PATH --containerfile PATH --check-script PATH" >&2
}

hypervisor=
driver=
fixture_binary=
containerfile=
check_script=
while [ "$#" -gt 0 ]; do
  option=$1
  shift
  [ "$#" -gt 0 ] || {
    usage
    exit 2
  }
  value=$1
  shift
  case "$option" in
    --hypervisor) hypervisor=$value ;;
    --driver) driver=$value ;;
    --fixture-binary) fixture_binary=$value ;;
    --containerfile) containerfile=$value ;;
    --check-script) check_script=$value ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[ -n "$hypervisor" ] && [ -n "$driver" ] && [ -n "$fixture_binary" ] && \
  [ -n "$containerfile" ] && [ -n "$check_script" ] || {
  usage
  exit 2
}

hypervisor=$(readlink -f "$hypervisor")
driver=$(readlink -f "$driver")
fixture_binary=$(readlink -f "$fixture_binary")
containerfile=$(readlink -f "$containerfile")
check_script=$(readlink -f "$check_script")

sh "$check_script"
if ! file -L "$fixture_binary" | grep -q 'statically linked'; then
  echo "fixture binary must be statically linked" >&2
  file -L "$fixture_binary" >&2
  exit 1
fi

podman=$(command -v podman)
criu=$(command -v criu)
timeout_executable=$(command -v timeout)
setsid_executable=$(command -v setsid)
work_directory=$(mktemp -d /tmp/gravesentinel-e2e.XXXXXX)
run_token=$(basename "$work_directory" | tr '[:upper:]' '[:lower:]')
run_id="gs-e2e-$(id -u)-$$-$run_token"
image_name="localhost/$run_id:fixture"
image_label_key=io.gravesentinel.ccm.e2e.run
image_label="$image_label_key=$run_id"
socket_path="$work_directory/hypervisor.sock"
storage_root="$work_directory/storage"
daemon_log="$work_directory/hypervisor.log"
daemon_pid=
cleanup_failed=0
preserve_artifacts=0
cgroup_paths="$work_directory/cgroups.txt"
: >"$cgroup_paths"

container_names="$run_id-smoke $run_id-fallback $run_id-cold $run_id-retention"

podman_bounded() {
  "$timeout_executable" --signal=TERM --kill-after=5s 45s "$podman" "$@"
}

container_owner() {
  podman_bounded inspect \
    --format '{{index .Config.Labels "io.gravesentinel.ccm.logical-id"}}' \
    "$1" 2>/dev/null
}

image_owner() {
  podman_bounded image inspect --format "{{index .Labels \"$image_label_key\"}}" \
    "$1" 2>/dev/null
}

terminate_session_processes() {
  session_id=$1
  session_pids=$(ps -eo pid=,sid= | \
    awk -v expected="$session_id" '$2 == expected {print $1}')
  [ -n "$session_pids" ] || return 0

  echo "terminating leaked hypervisor-session processes: $session_pids" >&2
  for process_id in $session_pids; do
    kill -TERM "$process_id" 2>/dev/null || true
  done
  attempts=0
  while [ "$attempts" -lt 50 ]; do
    session_pids=$(ps -eo pid=,sid= | \
      awk -v expected="$session_id" '$2 == expected {print $1}')
    [ -n "$session_pids" ] || break
    sleep 0.1
    attempts=$((attempts + 1))
  done
  for process_id in $session_pids; do
    kill -KILL "$process_id" 2>/dev/null || true
  done
  cleanup_failed=1
}

stop_daemon() {
  [ -n "$daemon_pid" ] || return 0
  if ! kill -0 "$daemon_pid" 2>/dev/null; then
    wait "$daemon_pid" 2>/dev/null
    daemon_status=$?
    echo "hypervisor exited unexpectedly before teardown: status=$daemon_status" >&2
    if kill -0 "-$daemon_pid" 2>/dev/null; then
      kill -KILL "-$daemon_pid" 2>/dev/null || true
    fi
    terminate_session_processes "$daemon_pid"
    cleanup_failed=1
    daemon_pid=
    return 0
  fi

  kill -TERM "$daemon_pid" 2>/dev/null || true
  (
    sleep 20
    kill -KILL "-$daemon_pid" 2>/dev/null || true
  ) &
  watchdog_pid=$!
  if ! wait "$daemon_pid"; then
    echo "hypervisor did not exit successfully" >&2
    cleanup_failed=1
  fi
  kill "$watchdog_pid" 2>/dev/null || true
  wait "$watchdog_pid" 2>/dev/null || true
  if kill -0 "-$daemon_pid" 2>/dev/null; then
    echo "hypervisor process group remains after shutdown" >&2
    kill -KILL "-$daemon_pid" 2>/dev/null || true
    cleanup_failed=1
  fi
  terminate_session_processes "$daemon_pid"
  daemon_pid=

  if [ -e "$socket_path" ]; then
    echo "hypervisor socket remains after shutdown: $socket_path" >&2
    cleanup_failed=1
    rm -f "$socket_path"
  fi
}

remove_owned_container() {
  name=$1
  expected_owner=$2
  podman_bounded container exists "$name"
  exists_status=$?
  case "$exists_status" in
    0) ;;
    1) return 0 ;;
    *)
      echo "cannot determine whether container exists: $name" >&2
      cleanup_failed=1
      return 0
      ;;
  esac
  owner=$(container_owner "$name" || true)
  if [ "$owner" != "$expected_owner" ]; then
    echo "REFUSE: container ownership mismatch: $name owner=$owner" >&2
    cleanup_failed=1
    return 0
  fi
  container_pid=$(podman_bounded inspect --format '{{.State.Pid}}' "$name" 2>/dev/null || true)
  if [ -n "$container_pid" ] && [ "$container_pid" -gt 0 ] 2>/dev/null && \
    [ -r "/proc/$container_pid/cgroup" ]; then
    awk -F: '$1 == "0" {print $3}' "/proc/$container_pid/cgroup" >>"$cgroup_paths"
  fi
  if ! podman_bounded rm --force "$name" >/dev/null; then
    echo "failed to remove E2E container: $name" >&2
    cleanup_failed=1
    return 0
  fi
  podman_bounded container exists "$name"
  exists_status=$?
  if [ "$exists_status" -ne 1 ]; then
    echo "container absence was not proven after cleanup: $name status=$exists_status" >&2
    cleanup_failed=1
  fi
}

remove_owned_image() {
  podman_bounded image exists "$image_name"
  exists_status=$?
  case "$exists_status" in
    0) ;;
    1) return 0 ;;
    *)
      echo "cannot determine whether fixture image exists: $image_name" >&2
      cleanup_failed=1
      return 0
      ;;
  esac
  owner=$(image_owner "$image_name" || true)
  if [ "$owner" != "$run_id" ]; then
    echo "REFUSE: image ownership mismatch: $image_name owner=$owner" >&2
    cleanup_failed=1
    return 0
  fi
  if ! podman_bounded image rm --force "$image_name" >/dev/null; then
    echo "failed to remove E2E image: $image_name" >&2
    cleanup_failed=1
    return 0
  fi
  podman_bounded image exists "$image_name"
  exists_status=$?
  if [ "$exists_status" -ne 1 ]; then
    echo "fixture image absence was not proven: status=$exists_status" >&2
    cleanup_failed=1
  fi
}

cleanup() {
  set +e
  stop_daemon
  for name in $container_names; do
    remove_owned_container "$name" "$name"
  done
  remove_owned_image

  while IFS= read -r cgroup_path; do
    [ -n "$cgroup_path" ] || continue
    attempts=0
    while [ -e "/sys/fs/cgroup$cgroup_path" ] && [ "$attempts" -lt 100 ]; do
      sleep 0.1
      attempts=$((attempts + 1))
    done
    if [ -e "/sys/fs/cgroup$cgroup_path" ]; then
      echo "E2E cgroup remains after cleanup: $cgroup_path" >&2
      cleanup_failed=1
    fi
  done <"$cgroup_paths"

  if ordinary_objects=$(podman_bounded ps --all --format '{{.Names}}'); then
    remaining=$(printf '%s\n' "$ordinary_objects" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "ordinary E2E containers remain after cleanup: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect ordinary containers after cleanup" >&2
    cleanup_failed=1
  fi
  if external_objects=$(podman_bounded ps --all --external --format '{{.Names}}'); then
    remaining=$(printf '%s\n' "$external_objects" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "external E2E containers remain after cleanup: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect external containers after cleanup" >&2
    cleanup_failed=1
  fi
  if mount_objects=$(podman_bounded mount --format json); then
    mounted=$(printf '%s\n' "$mount_objects" | grep -F "$run_id" || true)
    if [ -n "$mounted" ]; then
      echo "E2E container mounts remain after cleanup: $mounted" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect container mounts after cleanup" >&2
    cleanup_failed=1
  fi

  if [ "$cleanup_failed" -eq 0 ] && [ "$preserve_artifacts" -eq 0 ]; then
    if ! rm -rf "$work_directory" || [ -e "$work_directory" ]; then
      echo "failed to remove E2E temporary root: $work_directory" >&2
      cleanup_failed=1
    fi
  else
    echo "E2E diagnostics retained at $work_directory" >&2
  fi
  return "$cleanup_failed"
}

on_signal() {
  exit "$1"
}

on_exit() {
  test_status=$?
  trap - EXIT HUP INT TERM
  if [ "$test_status" -ne 0 ]; then
    preserve_artifacts=1
  fi
  cleanup_status=0
  cleanup || cleanup_status=$?
  if [ "$test_status" -eq 0 ] && [ "$cleanup_status" -eq 0 ]; then
    echo "RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed"
    exit 0
  fi
  if [ -f "$daemon_log" ]; then
    echo "---- hypervisor log ----" >&2
    cat "$daemon_log" >&2
  fi
  [ "$test_status" -ne 0 ] && exit "$test_status"
  exit "$cleanup_status"
}
trap on_exit EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

mkdir -p "$storage_root" "$work_directory/image"
cp "$fixture_binary" "$work_directory/image/counter_fixture"
cp "$containerfile" "$work_directory/image/Containerfile"
chmod 0555 "$work_directory/image/counter_fixture"

printf 'RUN: podman build fixture image %s\n' "$image_name"
"$timeout_executable" --signal=TERM --kill-after=10s 5m \
  "$podman" build --network=none --pull=never --label "$image_label" \
  --tag "$image_name" --file "$work_directory/image/Containerfile" \
  "$work_directory/image"

printf 'RUN: gravesentinel_hypervisor socket=%s storage=%s\n' \
  "$socket_path" "$storage_root"
"$setsid_executable" "$hypervisor" \
  --socket "$socket_path" \
  --storage "$storage_root" \
  --podman "$podman" \
  --criu "$criu" \
  --runtime-profile podman-criu-rootful-v2 \
  --scheduler-uid "$(id -u)" \
  --socket-mode 0600 \
  --gc-grace-seconds 1 \
  >"$daemon_log" 2>&1 &
daemon_pid=$!

"$driver" \
  --socket "$socket_path" \
  --storage "$storage_root" \
  --daemon-log "$daemon_log" \
  --podman "$podman" \
  --fixture-image "$image_name" \
  --run-id "$run_id"
