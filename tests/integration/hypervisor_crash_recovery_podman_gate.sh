#!/bin/sh
set -eu

real_file=$0.real
control_file=$0.control

[ -r "$real_file" ] || {
  echo "podman recovery gate real executable file is unavailable: $real_file" >&2
  exit 126
}
[ -r "$control_file" ] || {
  echo "podman recovery gate control file is unavailable: $control_file" >&2
  exit 126
}

real_podman=$(cat "$real_file")
control_directory=$(cat "$control_file")
[ -x "$real_podman" ] || {
  echo "podman recovery gate real executable is not executable: $real_podman" >&2
  exit 126
}
[ -d "$control_directory" ] || {
  echo "podman recovery gate control directory is unavailable: $control_directory" >&2
  exit 126
}
[ -f "$control_directory/mode" ] || {
  echo "podman recovery gate mode is unavailable" >&2
  exit 126
}

mode=$(cat "$control_directory/mode")
case "$mode" in
  none | before-restore | after-restore) ;;
  *)
    echo "podman recovery gate rejected mode: $mode" >&2
    exit 64
    ;;
esac

is_restore=0
if [ "${1:-}" = container ] && [ "${2:-}" = restore ]; then
  is_restore=1
fi

publish_reached() {
  marker=$control_directory/reached
  temporary=$control_directory/.reached.tmp.$$
  [ ! -e "$marker" ] || {
    echo "podman recovery gate reached marker already exists" >&2
    exit 73
  }
  (umask 077 && : >"$temporary")
  mv "$temporary" "$marker"
}

wait_for_release() {
  while [ ! -e "$control_directory/release" ]; do
    sleep 0.02
  done
}

if [ "$is_restore" -eq 1 ] && [ "$mode" = before-restore ]; then
  publish_reached
  wait_for_release
  exec "$real_podman" "$@"
fi

if [ "$is_restore" -eq 1 ] && [ "$mode" = after-restore ]; then
  if "$real_podman" "$@"; then
    status=0
  else
    status=$?
  fi
  [ "$status" -eq 0 ] || exit "$status"
  publish_reached
  wait_for_release
  exit 0
fi

exec "$real_podman" "$@"
