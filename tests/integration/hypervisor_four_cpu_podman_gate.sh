#!/bin/sh
set -eu

real_file=$0.real
control_file=$0.control

[ -r "$real_file" ] || {
  echo "podman gate real executable file is unavailable: $real_file" >&2
  exit 126
}
[ -r "$control_file" ] || {
  echo "podman gate control file is unavailable: $control_file" >&2
  exit 126
}

real_podman=$(cat "$real_file")
control_directory=$(cat "$control_file")
[ -x "$real_podman" ] || {
  echo "podman gate real executable is not executable: $real_podman" >&2
  exit 126
}
[ -d "$control_directory" ] || {
  echo "podman gate control directory is unavailable: $control_directory" >&2
  exit 126
}

if [ "${1:-}" = container ] && [ "${2:-}" = checkpoint ]; then
  container_name=
  for argument in "$@"; do
    container_name=$argument
  done
  case "$container_name" in
    '' | *[!A-Za-z0-9_.-]*)
      echo "podman gate rejected unsafe container name: $container_name" >&2
      exit 64
      ;;
  esac

  marker="$control_directory/checkpoint.$container_name.ready"
  temporary="$marker.tmp.$$"
  printf '%s\n' "$$" >"$temporary"
  mv "$temporary" "$marker"
  while [ ! -e "$control_directory/release" ]; do
    sleep 0.02
  done
fi

exec "$real_podman" "$@"
