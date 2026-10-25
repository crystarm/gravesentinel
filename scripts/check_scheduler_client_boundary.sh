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
forbidden_pattern='//src/(core|storage|hypervisor)(:|/)|(^|[@/])blake3|podman'

temporary_directory=$(mktemp -d)
cleanup() {
  rm -rf "$temporary_directory"
}
trap cleanup EXIT HUP INT TERM

check_graph() {
  graph_file=$1
  description=$2
  if grep -E "$forbidden_pattern" "$graph_file" >"$temporary_directory/forbidden"; then
    echo "$description contains forbidden dependencies:" >&2
    cat "$temporary_directory/forbidden" >&2
    return 1
  fi
}

expect_visibility_failure() {
  target=$1
  name=$2
  log=$temporary_directory/$name.log
  if (
    cd "$consumer_root"
    "$bazel_executable" build "$target"
  ) >"$log" 2>&1; then
    echo "$target unexpectedly bypassed Gravesentinel visibility" >&2
    return 1
  fi
  if ! grep -F "Visibility error:" "$log" >/dev/null ||
    ! grep -F "is not visible from" "$log" >/dev/null; then
    echo "$target failed for a reason other than visibility:" >&2
    cat "$log" >&2
    return 1
  fi
}

(
  cd "$repository_root"
  "$bazel_executable" query 'deps(//:scheduler_client)' --output=label
) >"$temporary_directory/root-graph"
check_graph "$temporary_directory/root-graph" "@gravesentinel//:scheduler_client"

(
  cd "$consumer_root"
  "$bazel_executable" build //:scheduler_client_consumer
  "$bazel_executable" query 'deps(//:scheduler_client_consumer)' --output=label
) >"$temporary_directory/consumer-graph"
check_graph "$temporary_directory/consumer-graph" "external scheduler_client_consumer"

expect_visibility_failure //negative:forbidden_core forbidden-core
expect_visibility_failure //negative:forbidden_storage forbidden-storage
expect_visibility_failure //negative:forbidden_podman forbidden-podman

echo "SchedulerClient Bazel boundary check passed"
