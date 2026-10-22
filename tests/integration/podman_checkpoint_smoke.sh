#!/bin/sh
set -eu

usage() {
  cat <<'EOF'
Usage: podman_checkpoint_smoke.sh \
  --fixture-binary PATH --containerfile PATH --check-script PATH \
  [--adapter-helper PATH] [--keep-artifacts]

Runs the destructive, opt-in Podman/CRIU checkpoint qualification using only
resources carrying a unique gravesentinel-ccm-smoke prefix and run ownership
label.
EOF
}

fixture_binary=
containerfile=
check_script=
adapter_helper=
keep_artifacts=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --fixture-binary)
      [ "$#" -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      fixture_binary=$2
      shift 2
      ;;
    --containerfile)
      [ "$#" -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      containerfile=$2
      shift 2
      ;;
    --check-script)
      [ "$#" -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      check_script=$2
      shift 2
      ;;
    --adapter-helper)
      [ "$#" -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
      adapter_helper=$2
      shift 2
      ;;
    --keep-artifacts)
      keep_artifacts=1
      shift
      ;;
    --help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[ -x "$fixture_binary" ] || { echo "fixture binary is not executable" >&2; exit 2; }
[ -f "$containerfile" ] || { echo "Containerfile does not exist" >&2; exit 2; }
[ -f "$check_script" ] || { echo "testbed check script does not exist" >&2; exit 2; }
if [ -n "$adapter_helper" ]; then
  [ -x "$adapter_helper" ] || {
    echo "adapter helper is not executable" >&2
    exit 2
  }
fi

for command_name in awk date file grep id mktemp podman tar timeout tr wc; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "required command not found: $command_name" >&2
    exit 1
  }
done

podman_executable=
criu_executable=
if [ -n "$adapter_helper" ]; then
  for command_name in criu mkdir readlink; do
    command -v "$command_name" >/dev/null 2>&1 || {
      echo "required adapter-mode command not found: $command_name" >&2
      exit 1
    }
  done
  podman_command=$(command -v podman)
  criu_command=$(command -v criu)
  podman_executable=$(readlink -f "$podman_command")
  criu_executable=$(readlink -f "$criu_command")
  adapter_helper=$(readlink -f "$adapter_helper")
  [ -x "$podman_executable" ] || {
    echo "canonical Podman executable is unavailable" >&2
    exit 1
  }
  [ -x "$criu_executable" ] || {
    echo "canonical CRIU executable is unavailable" >&2
    exit 1
  }
  [ -x "$adapter_helper" ] || {
    echo "canonical adapter helper is unavailable" >&2
    exit 1
  }
fi

sh "$check_script"

if ! file -L "$fixture_binary" | grep -q 'statically linked'; then
  echo "fixture binary must be statically linked for the scratch image" >&2
  file -L "$fixture_binary" >&2
  exit 1
fi

base_tmp=${TEST_TMPDIR:-${TMPDIR:-/tmp}}
work_directory=$(mktemp -d "$base_tmp/gravesentinel-ccm-smoke.XXXXXX")
if [ -n "$adapter_helper" ]; then
  work_directory=$(readlink -f "$work_directory")
fi
run_token=$(printf '%s' "${work_directory##*.}" | tr '[:upper:]' '[:lower:]')
run_id="gravesentinel-ccm-smoke-$(id -u)-$$-$run_token"
original_name="$run_id-original"
restored_name="$run_id-restored"
negative_name="$run_id-negative"
image_name="localhost/$run_id:fixture"
label_key=io.gravesentinel.ccm.smoke.run
logical_id_label_key=io.gravesentinel.ccm.logical-id
run_label="$label_key=$run_id"
container_label_key=$label_key
staging_directory=
metadata_path=
archive_path="$work_directory/checkpoint.tar"
archive_listing="$work_directory/artifact-entries.txt"
negative_archive="$work_directory/not-a-checkpoint.tar"
if [ -n "$adapter_helper" ]; then
  container_label_key=$logical_id_label_key
  staging_directory="$work_directory/staging"
  metadata_path="$work_directory/checkpoint-metadata.json"
  archive_path="$staging_directory/checkpoint.tar"
  negative_archive="$staging_directory/not-a-checkpoint.tar"
fi
original_cgroup=
restored_cgroup=
cleanup_complete=0

log_command() {
  printf 'RUN:'
  printf ' %s' "$@"
  printf '\n'
}

run() {
  log_command "$@"
  "$@"
}

container_owner() {
  podman inspect --format "{{index .Config.Labels \"$container_label_key\"}}" \
    "$1" 2>/dev/null
}

image_owner() {
  podman image inspect \
    --format '{{index .Labels "io.gravesentinel.ccm.smoke.run"}}' \
    "$1" 2>/dev/null
}

run_adapter() {
  adapter_subcommand=$1
  adapter_container=$2
  shift 2
  log_command "$adapter_helper" "$adapter_subcommand" \
    --podman "$podman_executable" --criu "$criu_executable" \
    --staging-root "$work_directory" --logical-id "$run_id" \
    --container "$adapter_container" "$@"
  "$adapter_helper" "$adapter_subcommand" \
    --podman "$podman_executable" --criu "$criu_executable" \
    --staging-root "$work_directory" --logical-id "$run_id" \
    --container "$adapter_container" "$@"
}

cleanup_adapter_container() {
  cleanup_name=$1
  if podman container exists "$cleanup_name"; then
    cleanup_owner=$(container_owner "$cleanup_name" || true)
    if [ "$cleanup_owner" != "$run_id" ]; then
      echo "REFUSE: adapter container logical ID does not match: $cleanup_name" >&2
      return 1
    fi
  fi
  if ! run_adapter cleanup "$cleanup_name"; then
    echo "FAIL: adapter cleanup failed: $cleanup_name" >&2
    return 1
  fi
}

stop_remove_adapter_container() {
  stop_remove_name=$1
  stop_remove_id=$2
  if ! podman container exists "$stop_remove_name"; then
    echo "adapter container is absent before stop/remove: $stop_remove_name" >&2
    return 1
  fi
  stop_remove_owner=$(container_owner "$stop_remove_name" || true)
  if [ "$stop_remove_owner" != "$run_id" ]; then
    echo "REFUSE: adapter container logical ID does not match: $stop_remove_name" >&2
    return 1
  fi
  run_adapter stop-remove "$stop_remove_name" \
    --expected-runtime-id "$stop_remove_id"
}

remove_owned_container() {
  resource_name=$1
  if ! podman container exists "$resource_name"; then
    return 0
  fi
  owner=$(container_owner "$resource_name" || true)
  if [ "$owner" != "$run_id" ]; then
    echo "REFUSE: container is not owned by this run: $resource_name" >&2
    return 1
  fi
  if [ -n "$adapter_helper" ]; then
    cleanup_adapter_container "$resource_name"
  else
    podman rm --force "$resource_name" >/dev/null
  fi
}

remove_owned_image() {
  if ! podman image exists "$image_name"; then
    return 0
  fi
  owner=$(image_owner "$image_name" || true)
  if [ "$owner" != "$run_id" ]; then
    echo "REFUSE: image is not owned by this run: $image_name" >&2
    return 1
  fi
  podman image rm --force "$image_name" >/dev/null
}

remove_resources() {
  set +e
  cleanup_status=0
  remove_owned_container "$negative_name" || cleanup_status=1
  remove_owned_container "$restored_name" || cleanup_status=1
  remove_owned_container "$original_name" || cleanup_status=1
  remove_owned_image || cleanup_status=1
  if [ "$keep_artifacts" -eq 0 ]; then
    rm -rf "$work_directory" || cleanup_status=1
  else
    printf 'INFO: retained artifacts in %s\n' "$work_directory"
  fi
  return "$cleanup_status"
}

on_exit() {
  status=$?
  trap - EXIT HUP INT TERM
  if [ "$cleanup_complete" -eq 0 ] && ! remove_resources; then
    echo "FAIL: smoke cleanup did not complete" >&2
    status=1
  fi
  exit "$status"
}

on_signal() {
  status=$1
  trap - EXIT HUP INT TERM
  if ! remove_resources && [ -n "$adapter_helper" ]; then
    echo "FAIL: adapter-mode smoke cleanup did not complete" >&2
    exit 1
  fi
  exit "$status"
}

trap on_exit EXIT
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

parse_latest_field() {
  field_name=$1
  awk -v wanted="$field_name" '
    /^counter=[0-9]+/ {
      for (field_index = 1; field_index <= NF; ++field_index) {
        split($field_index, item, "=")
        if (item[1] == wanted) value = item[2]
      }
    }
    END {
      if (value != "") print value
    }
  '
}

latest_field() {
  timeout 2 podman logs "$1" 2>/dev/null | parse_latest_field "$2"
}

latest_field_since() {
  timeout 2 podman logs --since "$3" "$1" 2>/dev/null |
    parse_latest_field "$2"
}

wait_for_field_greater() {
  container_name=$1
  field_name=$2
  threshold=$3
  attempts=$4
  attempt=0
  while [ "$attempt" -lt "$attempts" ]; do
    observed=$(latest_field "$container_name" "$field_name")
    if [ -n "$observed" ] && [ "$observed" -gt "$threshold" ]; then
      printf '%s\n' "$observed"
      return 0
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}

wait_for_field_greater_since() {
  container_name=$1
  field_name=$2
  threshold=$3
  since=$4
  attempts=$5
  attempt=0
  while [ "$attempt" -lt "$attempts" ]; do
    observed=$(latest_field_since "$container_name" "$field_name" "$since")
    if [ -n "$observed" ] && [ "$observed" -gt "$threshold" ]; then
      printf '%s\n' "$observed"
      return 0
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}

wait_for_field_since() {
  container_name=$1
  field_name=$2
  since=$3
  attempts=$4
  attempt=0
  while [ "$attempt" -lt "$attempts" ]; do
    observed=$(latest_field_since "$container_name" "$field_name" "$since")
    if [ -n "$observed" ]; then
      printf '%s\n' "$observed"
      return 0
    fi
    attempt=$((attempt + 1))
    sleep 1
  done
  return 1
}

container_cgroup() {
  container_pid=$(podman inspect --format '{{.State.Pid}}' "$1")
  awk -F: '$1 == "0" {print $3}' "/proc/$container_pid/cgroup"
}

cgroup_still_exists() {
  cgroup_path=$1
  if [ -z "$cgroup_path" ]; then
    return 1
  fi
  case "$cgroup_path" in
    /*) ;;
    *) echo "invalid cgroup path: $cgroup_path" >&2; return 0 ;;
  esac
  if [ -e "/sys/fs/cgroup$cgroup_path" ]; then
    return 0
  fi
  if [ "${cgroup_path##*/}" = "container" ]; then
    parent_path=${cgroup_path%/*}
    if [ -e "/sys/fs/cgroup$parent_path" ]; then
      return 0
    fi
  fi
  return 1
}

assert_cgroup_removed() {
  cgroup_path=$1
  description=$2
  attempts=10
  while [ "$attempts" -gt 0 ]; do
    if ! cgroup_still_exists "$cgroup_path"; then
      return 0
    fi
    attempts=$((attempts - 1))
    sleep 1
  done
  echo "$description cgroup still exists: $cgroup_path" >&2
  return 1
}

assert_cpu_mask() {
  observed_mask=$1
  description=$2
  if [ "$observed_mask" != "1" ]; then
    echo "$description threads escaped CPU 0; observed cpu_mask=$observed_mask" >&2
    exit 1
  fi
}

if [ -n "$adapter_helper" ]; then
  mkdir -m 0700 "$staging_directory"
fi
cp "$fixture_binary" "$work_directory/counter_fixture"
chmod 0555 "$work_directory/counter_fixture"
cp "$containerfile" "$work_directory/Containerfile"

run podman build --network=none --pull=never --label "$run_label" \
  --tag "$image_name" --file "$work_directory/Containerfile" "$work_directory"
image_id=$(podman image inspect --format '{{.Id}}' "$image_name")
printf 'INFO: fixture_image_id=%s\n' "$image_id"

if [ -n "$adapter_helper" ]; then
  run_adapter cold-start "$original_name" --image "$image_name"
else
  run podman run --detach --name "$original_name" --label "$run_label" \
    --network=none --read-only --cap-drop=all --passwd=false \
    --security-opt=no-new-privileges --cpuset-cpus=0 \
    "$image_name" --threads 2 --interval-ms 250
fi

original_id=$(podman inspect --format '{{.Id}}' "$original_name")
original_cpuset=$(podman inspect --format '{{.HostConfig.CpusetCpus}}' "$original_name")
original_cgroup=$(container_cgroup "$original_name")
printf 'INFO: original_container_id=%s\n' "$original_id"
printf 'INFO: original_cgroup=%s\n' "$original_cgroup"
[ "$original_cpuset" = "0" ] || {
  echo "container cpuset mismatch: expected 0, found $original_cpuset" >&2
  exit 1
}

before_checkpoint=$(wait_for_field_greater "$original_name" counter 40 30) || {
  echo "fixture counter did not advance before checkpoint" >&2
  podman logs "$original_name" >&2 || true
  exit 1
}
before_nonce=$(latest_field "$original_name" nonce)
before_cpu_mask=$(latest_field "$original_name" cpu_mask)
[ -n "$before_nonce" ] || { echo "fixture nonce is missing" >&2; exit 1; }
assert_cpu_mask "$before_cpu_mask" "original container"
podman logs "$original_name" 2>&1 | grep -q 'fixture-start' || {
  echo "fixture stderr startup marker is missing" >&2
  exit 1
}
printf 'INFO: counter_before_checkpoint=%s\n' "$before_checkpoint"
printf 'INFO: fixture_nonce=%s\n' "$before_nonce"

if [ -n "$adapter_helper" ]; then
  run_adapter capture "$original_name"
  [ -s "$metadata_path" ] || {
    echo "adapter capture metadata is empty" >&2
    exit 1
  }
else
  run podman container checkpoint --compress=none --leave-running \
    --export "$archive_path" "$original_name"
fi
checkpoint_returned_at=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
[ -s "$archive_path" ] || { echo "checkpoint archive is empty" >&2; exit 1; }
tar -tf "$archive_path" >"$archive_listing"
[ -s "$archive_listing" ] || {
  echo "checkpoint archive has no entries" >&2
  exit 1
}
printf 'INFO: checkpoint_archive_bytes=%s\n' "$(wc -c <"$archive_path")"
printf 'INFO: checkpoint_archive_entries=%s\n' "$(wc -l <"$archive_listing")"
awk '{print "INFO: checkpoint_archive_entry=" $0}' "$archive_listing"

original_running=$(podman inspect --format '{{.State.Running}}' "$original_name")
[ "$original_running" = "true" ] || {
  echo "original container is not running after --leave-running" >&2
  exit 1
}
counter_at_return=$(latest_field "$original_name" counter)
after_checkpoint=$(wait_for_field_greater_since "$original_name" counter \
  "$counter_at_return" "$checkpoint_returned_at" 10) || {
  echo "original counter did not advance after checkpoint returned" >&2
  exit 1
}
after_cpu_mask=$(latest_field_since "$original_name" cpu_mask \
  "$checkpoint_returned_at")
assert_cpu_mask "$after_cpu_mask" "post-checkpoint original container"
printf 'INFO: counter_after_checkpoint=%s\n' "$after_checkpoint"

[ "$(container_owner "$original_name")" = "$run_id" ] || {
  echo "original container ownership label changed" >&2
  exit 1
}
if [ -n "$adapter_helper" ]; then
  stop_remove_adapter_container "$original_name" "$original_id"
else
  run podman stop --time=10 "$original_name"
  run podman rm "$original_name"
fi
if podman container exists "$original_name"; then
  echo "original container still exists before restore" >&2
  exit 1
fi
assert_cgroup_removed "$original_cgroup" "original"
printf 'PASS: original container is absent before restore\n'

printf '%s\n' 'this is not a checkpoint archive' >"$negative_archive"
if [ -n "$adapter_helper" ]; then
  negative_restore_succeeded=0
  if run_adapter restore "$negative_name" --artifact "$negative_archive" \
    >"$work_directory/negative-restore.stdout" \
    2>"$work_directory/negative-restore.stderr"; then
    negative_restore_succeeded=1
  fi
  cleanup_adapter_container "$negative_name"
  if [ "$negative_restore_succeeded" -eq 1 ]; then
    echo "restore unexpectedly accepted an invalid archive" >&2
    exit 1
  fi
else
  if podman container restore --import "$negative_archive" \
    --name "$negative_name" >"$work_directory/negative-restore.stdout" \
    2>"$work_directory/negative-restore.stderr"; then
    echo "restore unexpectedly accepted an invalid archive" >&2
    exit 1
  fi
fi
if podman container exists "$negative_name"; then
  if [ -n "$adapter_helper" ]; then
    echo "failed restore cleanup did not prove container absence" >&2
  else
    echo "failed restore left a container behind" >&2
  fi
  exit 1
fi
printf 'PASS: invalid restore failed without leaving a container\n'

restore_started_at=$(date -u +%Y-%m-%dT%H:%M:%S.%NZ)
if [ -n "$adapter_helper" ]; then
  run_adapter restore "$restored_name" --artifact "$archive_path"
else
  run podman container restore --import "$archive_path" --name "$restored_name"
fi
restored_id=$(podman inspect --format '{{.Id}}' "$restored_name")
restored_name_observed=$(podman inspect --format '{{.Name}}' "$restored_name")
restored_cpuset=$(podman inspect --format '{{.HostConfig.CpusetCpus}}' "$restored_name")
restored_cgroup=$(container_cgroup "$restored_name")
printf 'INFO: restored_container_id=%s\n' "$restored_id"
printf 'INFO: restored_cgroup=%s\n' "$restored_cgroup"
[ "$restored_name_observed" = "$restored_name" ] || {
  echo "restored container name mismatch: $restored_name_observed" >&2
  exit 1
}
[ "$restored_id" != "$original_id" ] || {
  echo "import restore unexpectedly reused the original container ID" >&2
  exit 1
}
[ "$restored_cpuset" = "0" ] || {
  echo "restored cpuset mismatch: expected 0, found $restored_cpuset" >&2
  exit 1
}

restored_nonce=$(wait_for_field_since "$restored_name" nonce \
  "$restore_started_at" 10) || {
  echo "restored fixture emitted no post-restore nonce" >&2
  exit 1
}
[ "$restored_nonce" = "$before_nonce" ] || {
  echo "restored nonce changed; process was freshly started instead of restored" >&2
  exit 1
}
restored_counter=$(wait_for_field_greater_since "$restored_name" counter \
  "$before_checkpoint" "$restore_started_at" 10) || {
  echo "restored counter did not continue from checkpointed memory state" >&2
  podman logs "$restored_name" >&2 || true
  exit 1
}
restored_cpu_mask=$(latest_field_since "$restored_name" cpu_mask \
  "$restore_started_at")
assert_cpu_mask "$restored_cpu_mask" "restored container"
printf 'INFO: restored_nonce=%s\n' "$restored_nonce"
printf 'INFO: restored_counter=%s\n' "$restored_counter"

[ "$(container_owner "$restored_name")" = "$run_id" ] || {
  echo "restored container lacks this run's ownership label" >&2
  exit 1
}
if [ -n "$adapter_helper" ]; then
  stop_remove_adapter_container "$restored_name" "$restored_id"
else
  run podman stop --time=10 "$restored_name"
  run podman rm "$restored_name"
fi
assert_cgroup_removed "$restored_cgroup" "restored"
[ "$(image_owner "$image_name")" = "$run_id" ] || {
  echo "fixture image lacks this run's ownership label" >&2
  exit 1
}
run podman image rm "$image_name"

if [ -n "$adapter_helper" ]; then
  remaining=$(podman ps --all \
    --filter "label=$logical_id_label_key=$run_id" --format '{{.Names}}')
else
  remaining=$(podman ps --all --filter "label=$run_label" --format '{{.Names}}')
fi
[ -z "$remaining" ] || {
  echo "smoke containers remain after cleanup: $remaining" >&2
  exit 1
}

if [ "$keep_artifacts" -eq 0 ]; then
  rm -rf "$work_directory"
  [ ! -e "$archive_path" ] || {
    echo "checkpoint archive remains after cleanup" >&2
    exit 1
  }
else
  printf 'INFO: retained artifacts in %s\n' "$work_directory"
fi

cleanup_complete=1
trap - EXIT HUP INT TERM
if [ -n "$adapter_helper" ]; then
  printf 'RESULT: QUALIFIED production PodmanAdapter smoke passed\n'
else
  printf 'RESULT: QUALIFIED Podman/CRIU checkpoint smoke passed\n'
fi
