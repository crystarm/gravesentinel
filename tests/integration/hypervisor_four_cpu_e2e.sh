#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --hypervisor PATH --driver PATH --fixture-binary PATH --containerfile PATH --check-script PATH --gate-script PATH" >&2
}

hypervisor=
driver=
fixture_binary=
containerfile=
check_script=
gate_script=
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
    --gate-script) gate_script=$value ;;
    *)
      usage
      exit 2
      ;;
  esac
done

[ -n "$hypervisor" ] && [ -n "$driver" ] && [ -n "$fixture_binary" ] && \
  [ -n "$containerfile" ] && [ -n "$check_script" ] && \
  [ -n "$gate_script" ] || {
  usage
  exit 2
}

hypervisor=$(readlink -f "$hypervisor")
driver=$(readlink -f "$driver")
fixture_binary=$(readlink -f "$fixture_binary")
containerfile=$(readlink -f "$containerfile")
check_script=$(readlink -f "$check_script")
gate_script=$(readlink -f "$gate_script")

[ -x "$hypervisor" ] || {
  echo "hypervisor is not executable: $hypervisor" >&2
  exit 1
}
[ -x "$driver" ] || {
  echo "four-CPU driver is not executable: $driver" >&2
  exit 1
}
[ -r "$containerfile" ] || {
  echo "fixture Containerfile is not readable: $containerfile" >&2
  exit 1
}
[ -r "$check_script" ] || {
  echo "testbed check script is not readable: $check_script" >&2
  exit 1
}
[ -r "$gate_script" ] || {
  echo "Podman gate script is not readable: $gate_script" >&2
  exit 1
}

sh "$check_script"
[ "$(id -u)" -eq 0 ] || {
  echo "four-container CPU acceptance requires uid 0" >&2
  exit 1
}
if ! file -L "$fixture_binary" | grep -q 'statically linked'; then
  echo "fixture binary must be statically linked" >&2
  file -L "$fixture_binary" >&2
  exit 1
fi

podman=$(command -v podman)
criu=$(command -v criu)
timeout_executable=$(command -v timeout)
setsid_executable=$(command -v setsid)
command -v awk >/dev/null
command -v ps >/dev/null

work_directory=$(mktemp -d /tmp/gravesentinel-four-cpu.XXXXXX)
run_token=$(basename "$work_directory" | tr '[:upper:]' '[:lower:]')
run_id="gs4-$(id -u)-$$-$run_token"
image_name="localhost/$run_id:fixture"
image_label_key=io.gravesentinel.ccm.four-cpu.run
image_label="$image_label_key=$run_id"
socket_path="$work_directory/hypervisor.sock"
storage_root="$work_directory/storage"
daemon_log="$work_directory/hypervisor.log"
gate_directory="$work_directory/gate"
podman_gate="$work_directory/podman-gate"
daemon_pid=
cleanup_failed=0
preserve_artifacts=0
resources_claimed=0
cgroup_paths="$work_directory/cgroups.txt"
: >"$cgroup_paths"

container_names="$run_id-cpu0 $run_id-cpu2 $run_id-cpu4 $run_id-cpu6"

podman_bounded() {
  "$timeout_executable" --signal=TERM --kill-after=5s 45s "$podman" "$@"
}

claim_unique_resources() {
  for name in $container_names; do
    if podman_bounded container exists "$name"; then
      echo "generated container name already exists: $name" >&2
      return 1
    else
      exists_status=$?
      [ "$exists_status" -eq 1 ] || {
        echo "cannot prove generated container name is unused: $name" >&2
        return 1
      }
    fi
  done
  if podman_bounded image exists "$image_name"; then
    echo "generated fixture image already exists: $image_name" >&2
    return 1
  else
    exists_status=$?
    [ "$exists_status" -eq 1 ] || {
      echo "cannot prove generated fixture image name is unused" >&2
      return 1
    }
  fi
  [ ! -e "$socket_path" ] || {
    echo "generated socket path already exists: $socket_path" >&2
    return 1
  }
  resources_claimed=1
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

record_container_cgroup() {
  name=$1
  container_pid=$(podman_bounded inspect --format '{{.State.Pid}}' "$name" 2>/dev/null || true)
  if [ -n "$container_pid" ] && [ "$container_pid" -gt 0 ] 2>/dev/null && \
    [ -r "/proc/$container_pid/cgroup" ]; then
    awk -F: '$1 == "0" {print $3}' "/proc/$container_pid/cgroup" >>"$cgroup_paths"
  fi
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
  session_id=$daemon_pid
  if ! kill -0 "$daemon_pid" 2>/dev/null; then
    wait "$daemon_pid" 2>/dev/null
    daemon_status=$?
    echo "hypervisor exited unexpectedly before teardown: status=$daemon_status" >&2
    terminate_session_processes "$session_id"
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
  if kill -0 "-$session_id" 2>/dev/null; then
    echo "hypervisor process group remains after shutdown" >&2
    kill -KILL "-$session_id" 2>/dev/null || true
    cleanup_failed=1
  fi
  terminate_session_processes "$session_id"
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
  record_container_cgroup "$name"
  if ! podman_bounded rm --force "$name" >/dev/null; then
    echo "failed to remove four-CPU container: $name" >&2
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
    echo "failed to remove four-CPU fixture image: $image_name" >&2
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

check_recorded_cgroups_absent() {
  sort -u "$cgroup_paths" | while IFS= read -r cgroup_path; do
    [ -n "$cgroup_path" ] || continue
    case "$cgroup_path" in
      /*) ;;
      *)
        echo "invalid recorded cgroup path: $cgroup_path" >&2
        exit 1
        ;;
    esac
    attempts=0
    while [ -e "/sys/fs/cgroup$cgroup_path" ] && [ "$attempts" -lt 100 ]; do
      sleep 0.1
      attempts=$((attempts + 1))
    done
    if [ -e "/sys/fs/cgroup$cgroup_path" ]; then
      echo "four-CPU cgroup remains after cleanup: $cgroup_path" >&2
      exit 1
    fi
  done
}

check_no_owned_runtime_objects() {
  if ordinary_objects=$(podman_bounded ps --all --format '{{.Names}}'); then
    remaining=$(printf '%s\n' "$ordinary_objects" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "ordinary four-CPU containers remain: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect ordinary containers after cleanup" >&2
    cleanup_failed=1
  fi

  if external_objects=$(podman_bounded ps --all --external --format '{{.Names}}'); then
    remaining=$(printf '%s\n' "$external_objects" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "external four-CPU containers remain: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect external containers after cleanup" >&2
    cleanup_failed=1
  fi

  if mount_objects=$(podman_bounded mount --format json); then
    mounted=$(printf '%s\n' "$mount_objects" | grep -F "$run_id" || true)
    if [ -n "$mounted" ]; then
      echo "four-CPU container mounts remain: $mounted" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect container mounts after cleanup" >&2
    cleanup_failed=1
  fi
}

cleanup() {
  set +e
  stop_daemon
  if [ "$resources_claimed" -eq 1 ]; then
    for name in $container_names; do
      remove_owned_container "$name" "$name"
    done
    remove_owned_image
  fi
  check_recorded_cgroups_absent || cleanup_failed=1
  check_no_owned_runtime_objects

  if [ "$cleanup_failed" -eq 0 ] && [ "$preserve_artifacts" -eq 0 ]; then
    if ! rm -rf "$work_directory" || [ -e "$work_directory" ]; then
      echo "failed to remove four-CPU temporary root: $work_directory" >&2
      cleanup_failed=1
    fi
  else
    echo "four-CPU diagnostics retained at $work_directory" >&2
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
    echo "RESULT: QUALIFIED four-container CPU acceptance passed"
    exit 0
  fi
  if [ -f "$daemon_log" ]; then
    echo "---- four-CPU hypervisor log ----" >&2
    cat "$daemon_log" >&2
  fi
  [ "$test_status" -ne 0 ] && exit "$test_status"
  exit "$cleanup_status"
}
trap on_exit EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

mkdir -p "$storage_root" "$work_directory/image" "$gate_directory"
cp "$fixture_binary" "$work_directory/image/counter_fixture"
cp "$containerfile" "$work_directory/image/Containerfile"
cp "$gate_script" "$podman_gate"
printf '%s\n' "$podman" >"$podman_gate.real"
printf '%s\n' "$gate_directory" >"$podman_gate.control"
chmod 0555 "$work_directory/image/counter_fixture" "$podman_gate"
chmod 0444 "$podman_gate.real" "$podman_gate.control"
claim_unique_resources

printf 'RUN: podman build four-CPU fixture image %s\n' "$image_name"
"$timeout_executable" --signal=TERM --kill-after=10s 5m \
  "$podman" build --network=none --pull=never --label "$image_label" \
  --tag "$image_name" --file "$work_directory/image/Containerfile" \
  "$work_directory/image"

printf 'RUN: gravesentinel_hypervisor four-CPU socket=%s storage=%s\n' \
  "$socket_path" "$storage_root"
"$setsid_executable" "$hypervisor" \
  --socket "$socket_path" \
  --storage "$storage_root" \
  --podman "$podman_gate" \
  --criu "$criu" \
  --runtime-profile podman-criu-rootful-v1 \
  --scheduler-uid "$(id -u)" \
  --socket-mode 0600 \
  --gc-grace-seconds 1 \
  >"$daemon_log" 2>&1 &
daemon_pid=$!

"$driver" \
  --socket "$socket_path" \
  --podman "$podman" \
  --fixture-image "$image_name" \
  --run-id "$run_id" \
  --gate-directory "$gate_directory" \
  --daemon-pid "$daemon_pid"
