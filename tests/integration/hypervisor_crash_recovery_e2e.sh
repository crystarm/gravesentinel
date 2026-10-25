#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --hypervisor PATH --fault-hypervisor PATH --driver PATH --fixture-binary PATH --containerfile PATH --check-script PATH --gate-script PATH" >&2
}

hypervisor=
fault_hypervisor=
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
    --fault-hypervisor) fault_hypervisor=$value ;;
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

[ -n "$hypervisor" ] && [ -n "$fault_hypervisor" ] && \
  [ -n "$driver" ] && [ -n "$fixture_binary" ] && \
  [ -n "$containerfile" ] && [ -n "$check_script" ] && \
  [ -n "$gate_script" ] || {
  usage
  exit 2
}

hypervisor=$(readlink -f "$hypervisor")
fault_hypervisor=$(readlink -f "$fault_hypervisor")
driver=$(readlink -f "$driver")
fixture_binary=$(readlink -f "$fixture_binary")
containerfile=$(readlink -f "$containerfile")
check_script=$(readlink -f "$check_script")
gate_script=$(readlink -f "$gate_script")

[ -x "$hypervisor" ] || {
  echo "production hypervisor is not executable: $hypervisor" >&2
  exit 1
}
[ -x "$fault_hypervisor" ] || {
  echo "fault hypervisor is not executable: $fault_hypervisor" >&2
  exit 1
}
[ -x "$driver" ] || {
  echo "crash/recovery driver is not executable: $driver" >&2
  exit 1
}
[ -r "$fixture_binary" ] || {
  echo "fixture binary is not readable: $fixture_binary" >&2
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
  echo "stage-13 crash/recovery acceptance requires uid 0" >&2
  exit 1
}
if ! file -L "$fixture_binary" | grep -q 'statically linked'; then
  echo "fixture binary must be statically linked" >&2
  file -L "$fixture_binary" >&2
  exit 1
fi
sh -n "$gate_script"

podman=$(command -v podman)
criu=$(command -v criu)
timeout_executable=$(command -v timeout)
setsid_executable=$(command -v setsid)
command -v awk >/dev/null
command -v cat >/dev/null
command -v grep >/dev/null
command -v ps >/dev/null
command -v sort >/dev/null
command -v tr >/dev/null

rootless=$("$podman" info --format '{{.Host.Security.Rootless}}')
[ "$rootless" = false ] || {
  echo "stage-13 acceptance requires local rootful Podman" >&2
  exit 1
}

work_directory=$(mktemp -d /tmp/gravesentinel-crash-recovery.XXXXXX)
work_name=$(basename "$work_directory")
run_token=$(printf '%s\n' "${work_name##*.}" | tr '[:upper:]' '[:lower:]')
run_id="gs13-$(id -u)-$$-$run_token"
image_name="localhost/$run_id:fixture"
image_label_key=io.gravesentinel.ccm.crash-recovery.run
image_label="$image_label_key=$run_id"
logical_label_key=io.gravesentinel.ccm.logical-id
conflict_label_key=io.gravesentinel.ccm.crash-recovery-conflict
container_name="$run_id-recovery"
socket_path="$work_directory/hypervisor.sock"
storage_root="$work_directory/storage"
state_directory="$work_directory/state"
daemon_log="$work_directory/hypervisor.log"
fault_control="$work_directory/fault-control"
gate_control="$work_directory/gate-control"
podman_gate="$work_directory/podman-gate"
sessions_file="$work_directory/daemon-sessions"
cgroup_paths="$state_directory/cgroups"
daemon_pid=
daemon_kind=
cleanup_failed=0
preserve_artifacts=0
container_token_claimed=0
: >"$sessions_file"

podman_bounded() {
  "$timeout_executable" --signal=TERM --kill-after=5s 90s "$podman" "$@"
}

claim_unique_resources() {
  if podman_bounded container exists "$container_name"; then
    echo "generated container name already exists: $container_name" >&2
    return 1
  else
    exists_status=$?
    [ "$exists_status" -eq 1 ] || {
      echo "cannot prove generated container name is unused" >&2
      return 1
    }
  fi
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
  container_token_claimed=1
}

session_pids() {
  expected_session=$1
  ps -eo pid=,sid= | awk -v expected="$expected_session" \
    '$2 == expected {print $1}'
}

kill_session_now() {
  expected_session=$1
  attempts=0
  while [ "$attempts" -lt 100 ]; do
    process_ids=$(session_pids "$expected_session")
    [ -n "$process_ids" ] || return 0
    for process_id in $process_ids; do
      kill -KILL "$process_id" 2>/dev/null || true
    done
    sleep 0.05
    attempts=$((attempts + 1))
  done
  process_ids=$(session_pids "$expected_session")
  [ -z "$process_ids" ] || {
    echo "processes remain in daemon session $expected_session: $process_ids" >&2
    return 1
  }
}

assert_runtime_count() {
  expected=$1
  filter="label=$logical_label_key=$container_name"
  ordinary_file="$work_directory/runtime-ordinary"
  external_file="$work_directory/runtime-external"
  combined_file="$work_directory/runtime-combined"
  podman_bounded ps --all --filter "$filter" --format '{{.ID}}' >"$ordinary_file"
  podman_bounded ps --all --external --filter "$filter" \
    --format '{{.ID}}' >"$external_file"
  sort -u "$ordinary_file" "$external_file" | awk 'NF' >"$combined_file"
  observed=$(awk 'END {print NR + 0}' "$combined_file")
  [ "$observed" -eq "$expected" ] || {
    echo "expected $expected ordinary/external runtime(s), found $observed" >&2
    cat "$combined_file" >&2
    return 1
  }
}

container_owner() {
  podman_bounded inspect \
    --format '{{index .Config.Labels "io.gravesentinel.ccm.logical-id"}}' \
    "$1" 2>/dev/null
}

image_owner() {
  podman_bounded image inspect \
    --format "{{index .Labels \"$image_label_key\"}}" "$1" 2>/dev/null
}

record_container_cgroup() {
  name=$1
  container_pid=$(podman_bounded inspect --format '{{.State.Pid}}' "$name" \
    2>/dev/null || true)
  if [ -n "$container_pid" ] && [ "$container_pid" -gt 0 ] 2>/dev/null && \
    [ -r "/proc/$container_pid/cgroup" ]; then
    awk -F: '$1 == "0" {print $3}' "/proc/$container_pid/cgroup" \
      >>"$cgroup_paths"
  fi
}

write_control_mode() {
  directory=$1
  mode=$2
  rm -f "$directory/reached" "$directory/release"
  temporary="$directory/.mode.tmp.$$"
  (umask 077 && printf '%s\n' "$mode" >"$temporary")
  mv "$temporary" "$directory/mode"
}

append_log_header() {
  label=$1
  {
    printf '\n===== stage-13 daemon start: %s =====\n' "$label"
    printf 'socket=%s storage=%s\n' "$socket_path" "$storage_root"
  } >>"$daemon_log"
}

start_production() {
  label=$1
  [ -z "$daemon_pid" ] || {
    echo "refusing to start a second daemon" >&2
    return 1
  }
  append_log_header "production/$label"
  "$setsid_executable" "$hypervisor" \
    --socket "$socket_path" \
    --storage "$storage_root" \
    --podman "$podman" \
    --criu "$criu" \
    --runtime-profile podman-criu-rootful-v1 \
    --scheduler-uid "$(id -u)" \
    --socket-mode 0600 \
    --gc-grace-seconds 1 \
    >>"$daemon_log" 2>&1 &
  daemon_pid=$!
  daemon_kind="production/$label"
  printf '%s\n' "$daemon_pid" >>"$sessions_file"
}

start_fault() {
  requested_storage_mode=$1
  requested_gate_mode=$2
  fault_label=$3
  [ -z "$daemon_pid" ] || {
    echo "refusing to start a second daemon" >&2
    return 1
  }
  write_control_mode "$fault_control" "$requested_storage_mode"
  write_control_mode "$gate_control" "$requested_gate_mode"
  append_log_header "fault/$fault_label storage=$requested_storage_mode gate=$requested_gate_mode"
  "$setsid_executable" "$fault_hypervisor" \
    --fault-control "$fault_control" \
    --socket "$socket_path" \
    --storage "$storage_root" \
    --podman "$podman_gate" \
    --criu "$criu" \
    --runtime-profile podman-criu-rootful-v1 \
    --scheduler-uid "$(id -u)" \
    --socket-mode 0600 \
    --gc-grace-seconds 1 \
    >>"$daemon_log" 2>&1 &
  daemon_pid=$!
  daemon_kind="fault/$fault_label"
  printf '%s\n' "$daemon_pid" >>"$sessions_file"
}

stop_daemon() {
  [ -n "$daemon_pid" ] || return 0
  stopped_pid=$daemon_pid
  stopped_kind=$daemon_kind
  if ! kill -0 "$stopped_pid" 2>/dev/null; then
    set +e
    wait "$stopped_pid"
    daemon_status=$?
    set -e
    echo "$stopped_kind exited unexpectedly: status=$daemon_status" >&2
    kill_session_now "$stopped_pid" || true
    daemon_pid=
    daemon_kind=
    cleanup_failed=1
    return 0
  fi

  kill -TERM "$stopped_pid"
  (
    sleep 30
    kill -KILL "-$stopped_pid" 2>/dev/null || true
  ) &
  watchdog_pid=$!
  set +e
  wait "$stopped_pid"
  daemon_status=$?
  set -e
  kill "$watchdog_pid" 2>/dev/null || true
  wait "$watchdog_pid" 2>/dev/null || true
  daemon_pid=
  daemon_kind=

  if [ "$daemon_status" -ne 0 ]; then
    echo "$stopped_kind did not exit 0 after SIGTERM: status=$daemon_status" >&2
    cleanup_failed=1
  fi
  if ! kill_session_now "$stopped_pid"; then
    cleanup_failed=1
  fi
  if [ -e "$socket_path" ]; then
    echo "$stopped_kind left its socket after graceful shutdown" >&2
    rm -f "$socket_path"
    cleanup_failed=1
  fi
  [ "$daemon_status" -eq 0 ]
}

crash_daemon() {
  [ -n "$daemon_pid" ] || {
    echo "no daemon is available for expected SIGKILL" >&2
    return 1
  }
  crashed_pid=$daemon_pid
  crashed_kind=$daemon_kind
  process_ids=$(session_pids "$crashed_pid")
  [ -n "$process_ids" ] || {
    echo "$crashed_kind session disappeared before expected SIGKILL" >&2
    return 1
  }
  for process_id in $process_ids; do
    kill -KILL "$process_id" 2>/dev/null || true
  done
  set +e
  wait "$crashed_pid"
  daemon_status=$?
  set -e
  daemon_pid=
  daemon_kind=
  [ "$daemon_status" -eq 137 ] || {
    echo "$crashed_kind did not report expected SIGKILL status 137: $daemon_status" >&2
    return 1
  }
  kill_session_now "$crashed_pid" || {
    echo "$crashed_kind retained same-session descendants after SIGKILL" >&2
    return 1
  }
  printf 'PASS: expected SIGKILL daemon/session=%s kind=%s\n' \
    "$crashed_pid" "$crashed_kind"
}

wait_for_marker() {
  marker=$1
  description=$2
  attempts=0
  while [ "$attempts" -lt 12000 ]; do
    if [ -f "$marker" ]; then
      return 0
    fi
    if [ -n "$daemon_pid" ] && ! kill -0 "$daemon_pid" 2>/dev/null; then
      echo "daemon exited while waiting for $description" >&2
      return 1
    fi
    sleep 0.05
    attempts=$((attempts + 1))
  done
  echo "timed out waiting for $description: $marker" >&2
  return 1
}

driver_phase() {
  phase=$1
  shift
  "$driver" \
    --phase "$phase" \
    --socket "$socket_path" \
    --storage "$storage_root" \
    --state "$state_directory" \
    --podman "$podman" \
    --fixture-image "$image_name" \
    --run-id "$run_id" \
    "$@"
}

create_test_conflict() {
  assert_runtime_count 0
  podman_bounded run --detach \
    --name "$container_name" \
    --label "$logical_label_key=$container_name" \
    --label "$conflict_label_key=$container_name" \
    --network=none \
    --read-only \
    --cap-drop=all \
    --passwd=false \
    --security-opt=no-new-privileges \
    --cpuset-cpus=0 \
    "$image_name" \
    --threads 2 \
    --interval-ms 250 \
    --state-file /dev/shm/gravesentinel-stage13.state \
    --instance-tag stage13 >/dev/null
  assert_runtime_count 1
}

remove_owned_container() {
  name=$1
  if podman_bounded container exists "$name"; then
    :
  else
    exists_status=$?
    if [ "$exists_status" -eq 1 ]; then
      return 0
    fi
    echo "cannot determine whether test container exists: $name" >&2
    cleanup_failed=1
    return 0
  fi

  owner=$(container_owner "$name" || true)
  if [ "$owner" != "$container_name" ]; then
    echo "REFUSE: container ownership mismatch: $name owner=$owner" >&2
    cleanup_failed=1
    return 0
  fi
  record_container_cgroup "$name"
  if ! podman_bounded rm --force "$name" >/dev/null; then
    echo "failed to remove test-owned container: $name" >&2
    cleanup_failed=1
    return 0
  fi
  if podman_bounded container exists "$name"; then
    echo "test-owned container remains after cleanup: $name" >&2
    cleanup_failed=1
  else
    exists_status=$?
    if [ "$exists_status" -ne 1 ]; then
      echo "container absence was not proven: status=$exists_status" >&2
      cleanup_failed=1
    fi
  fi
}

remove_owned_image() {
  [ "$container_token_claimed" -eq 1 ] || return 0
  if podman_bounded image exists "$image_name"; then
    :
  else
    exists_status=$?
    if [ "$exists_status" -eq 1 ]; then
      return 0
    fi
    echo "cannot determine whether fixture image exists" >&2
    cleanup_failed=1
    return 0
  fi

  owner=$(image_owner "$image_name" || true)
  if [ "$owner" != "$run_id" ]; then
    echo "REFUSE: image ownership mismatch: $image_name owner=$owner" >&2
    cleanup_failed=1
    return 0
  fi
  if ! podman_bounded image rm --force "$image_name" >/dev/null; then
    echo "failed to remove test-owned fixture image" >&2
    cleanup_failed=1
    return 0
  fi
  if podman_bounded image exists "$image_name"; then
    echo "test-owned fixture image remains after cleanup" >&2
    cleanup_failed=1
  else
    exists_status=$?
    if [ "$exists_status" -ne 1 ]; then
      echo "fixture image absence was not proven: status=$exists_status" >&2
      cleanup_failed=1
    fi
  fi
}

check_recorded_cgroups_absent() {
  [ -f "$cgroup_paths" ] || return 0
  sorted_cgroups="$work_directory/cgroups.sorted"
  sort -u "$cgroup_paths" >"$sorted_cgroups"
  while IFS= read -r cgroup_path; do
    [ -n "$cgroup_path" ] || continue
    case "$cgroup_path" in
      /*) ;;
      *)
        echo "invalid recorded cgroup path: $cgroup_path" >&2
        cleanup_failed=1
        continue
        ;;
    esac
    attempts=0
    while [ -e "/sys/fs/cgroup$cgroup_path" ] && \
      [ "$attempts" -lt 100 ]; do
      sleep 0.1
      attempts=$((attempts + 1))
    done
    if [ -e "/sys/fs/cgroup$cgroup_path" ]; then
      echo "test-owned cgroup remains: $cgroup_path" >&2
      cleanup_failed=1
    fi
  done <"$sorted_cgroups"
}

release_test_gates() {
  for control_directory in "$fault_control" "$gate_control"; do
    [ -d "$control_directory" ] || continue
    if ! (umask 077 && : >"$control_directory/release"); then
      echo "failed to release test gate: $control_directory" >&2
      cleanup_failed=1
    fi
  done
}

check_no_owned_runtime_objects() {
  if ordinary=$(podman_bounded ps --all --format '{{.ID}} {{.Names}}'); then
    remaining=$(printf '%s\n' "$ordinary" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "ordinary test containers remain: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect ordinary containers after cleanup" >&2
    cleanup_failed=1
  fi

  if external=$(podman_bounded ps --all --external \
    --format '{{.ID}} {{.Names}}'); then
    remaining=$(printf '%s\n' "$external" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "external test containers remain: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect external containers after cleanup" >&2
    cleanup_failed=1
  fi

  if mounts=$(podman_bounded mount --format json); then
    remaining=$(printf '%s\n' "$mounts" | grep -F "$run_id" || true)
    if [ -n "$remaining" ]; then
      echo "test-owned Podman mounts remain: $remaining" >&2
      cleanup_failed=1
    fi
  else
    echo "cannot inspect Podman mounts after cleanup" >&2
    cleanup_failed=1
  fi
}

cleanup() {
  set +e
  release_test_gates
  if [ -n "$daemon_pid" ]; then
    stop_daemon || true
  fi
  if [ "$container_token_claimed" -eq 1 ]; then
    remove_owned_container "$container_name"
  fi
  if ! assert_runtime_count 0; then
    cleanup_failed=1
  fi
  remove_owned_image
  check_recorded_cgroups_absent
  check_no_owned_runtime_objects
  return "$cleanup_failed"
}

on_signal() {
  exit "$1"
}

on_exit() {
  test_status=$?
  trap - EXIT HUP INT TERM
  cleanup_status=0
  cleanup || cleanup_status=$?

  if [ "$test_status" -ne 0 ] || [ "$cleanup_status" -ne 0 ]; then
    preserve_artifacts=1
    if [ -f "$daemon_log" ]; then
      echo "---- stage-13 appended daemon log ----" >&2
      cat "$daemon_log" >&2
    fi
  fi

  if [ "$preserve_artifacts" -eq 0 ]; then
    if ! rm -rf "$work_directory" || [ -e "$work_directory" ]; then
      echo "failed to remove stage-13 temporary root: $work_directory" >&2
      cleanup_status=1
    fi
  else
    echo "stage-13 diagnostics retained at $work_directory" >&2
  fi

  if [ "$test_status" -eq 0 ] && [ "$cleanup_status" -eq 0 ]; then
    echo "RESULT: QUALIFIED crash/reconnect/recovery acceptance passed"
    exit 0
  fi
  [ "$test_status" -ne 0 ] && exit "$test_status"
  exit "$cleanup_status"
}
trap on_exit EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

mkdir -p "$storage_root" "$state_directory" "$work_directory/image" \
  "$fault_control" "$gate_control"
: >"$cgroup_paths"
cp "$fixture_binary" "$work_directory/image/counter_fixture"
cp "$containerfile" "$work_directory/image/Containerfile"
cp "$gate_script" "$podman_gate"
printf '%s\n' "$podman" >"$podman_gate.real"
printf '%s\n' "$gate_control" >"$podman_gate.control"
chmod 0555 "$work_directory/image/counter_fixture" "$podman_gate"
chmod 0444 "$podman_gate.real" "$podman_gate.control"
claim_unique_resources

printf 'RUN: podman build stage-13 fixture image %s\n' "$image_name"
"$timeout_executable" --signal=TERM --kill-after=10s 5m \
  "$podman" build --network=none --pull=never --label "$image_label" \
  --tag "$image_name" --file "$work_directory/image/Containerfile" \
  "$work_directory/image"

start_production baseline
driver_phase initialize
assert_runtime_count 1
stop_daemon

for create_fault_mode in chunk-write-block manifest-before-rename-block \
  manifest-after-rename-block; do
  case_name="create-$create_fault_mode"
  request_id="$run_id-$case_name-request"
  start_fault "$create_fault_mode" none "$case_name-crash"
  driver_phase register --case "$case_name"
  driver_phase submit-create-crash \
    --case "$case_name" \
    --request-id "$request_id"
  wait_for_marker "$fault_control/reached" "$create_fault_mode reached marker"
  assert_runtime_count 1
  crash_daemon

  start_fault none none "$case_name-restart"
  driver_phase register --case "$case_name-restart"
  driver_phase reconcile-create-crash \
    --case "$case_name" \
    --request-id "$request_id" \
    --fault-mode "$create_fault_mode"
  assert_runtime_count 1
  stop_daemon
done

case_name=restore-before
request_id="$run_id-$case_name-request"
start_fault none before-restore "$case_name-crash"
driver_phase register --case "$case_name"
driver_phase prepare-restore-crash \
  --case "$case_name" \
  --request-id "$request_id"
wait_for_marker "$gate_control/reached" "before-restore gate"
assert_runtime_count 0
crash_daemon
start_fault none none "$case_name-restart"
driver_phase register --case "$case_name-restart"
driver_phase reconcile-restore-before \
  --case "$case_name" \
  --request-id "$request_id"
assert_runtime_count 1
stop_daemon

case_name=restore-after
request_id="$run_id-$case_name-request"
start_fault none after-restore "$case_name-crash"
driver_phase register --case "$case_name"
driver_phase prepare-restore-crash \
  --case "$case_name" \
  --request-id "$request_id"
wait_for_marker "$gate_control/reached" "after-restore gate"
assert_runtime_count 1
crash_daemon
start_fault none none "$case_name-restart"
driver_phase register --case "$case_name-restart"
driver_phase reconcile-restore-after \
  --case "$case_name" \
  --request-id "$request_id"
assert_runtime_count 1
stop_daemon

case_name=restore-conflict
request_id="$run_id-$case_name-request"
start_fault none before-restore "$case_name-crash"
driver_phase register --case "$case_name"
driver_phase prepare-restore-crash \
  --case "$case_name" \
  --request-id "$request_id"
wait_for_marker "$gate_control/reached" "conflict before-restore gate"
assert_runtime_count 0
create_test_conflict
crash_daemon
start_fault none none "$case_name-restart"
driver_phase register --case "$case_name-restart"
driver_phase reconcile-restore-conflict \
  --case "$case_name" \
  --request-id "$request_id"
assert_runtime_count 1
stop_daemon

for storage_fault_mode in chunk-enospc chunk-eacces chunk-eintr-once; do
  case_name="storage-$storage_fault_mode"
  request_id="$run_id-$case_name-request"
  start_fault "$storage_fault_mode" none "$case_name"
  driver_phase register --case "$case_name"
  driver_phase storage-fault \
    --case "$case_name" \
    --request-id "$request_id" \
    --fault-mode "$storage_fault_mode"
  [ -f "$fault_control/reached" ] || {
    echo "controlled storage mode did not publish reached: $storage_fault_mode" >&2
    exit 1
  }
  assert_runtime_count 1
  stop_daemon
done

start_production stress
driver_phase register --case stress
driver_phase stress
assert_runtime_count 1
stop_daemon

sleep 2
start_production final-startup-gc
driver_phase register --case final
driver_phase finalize
assert_runtime_count 1
stop_daemon
