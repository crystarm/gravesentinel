#!/bin/sh
set -eu

hypervisor=$1

help_output=$("$hypervisor" --help)
printf '%s\n' "$help_output" | grep -F -- '--gc-grace-seconds N' >/dev/null
printf '%s\n' "$help_output" | grep -F -- 'default: 3600' >/dev/null

set +e
"$hypervisor" --gc-grace-seconds invalid >/dev/null 2>&1
invalid_status=$?
"$hypervisor" \
  --gc-grace-seconds 1 \
  --socket /proc/gravesentinel-test/hypervisor.sock \
  --storage /proc/gravesentinel-test/storage \
  >/dev/null 2>&1
valid_status=$?
set -e

[ "$invalid_status" -eq 2 ] || {
  echo "invalid GC grace must fail argument parsing with exit 2" >&2
  exit 1
}
[ "$valid_status" -eq 3 ] || {
  echo "valid GC grace must reach filesystem startup and fail with exit 3" >&2
  exit 1
}
