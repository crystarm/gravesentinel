#!/bin/sh
set -eu

if [ "$#" -eq 0 ]; then
  echo "no C++ sources were supplied" >&2
  exit 2
fi

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format is required for this test" >&2
  exit 2
fi

clang-format --dry-run --Werror "$@"
