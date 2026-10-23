#!/bin/sh
set -eu

if [ "$#" -eq 0 ]; then
  echo "no C++ sources were supplied" >&2
  exit 2
fi

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "$CLANG_FORMAT is required for this test" >&2
  exit 2
fi

echo "PATH=$PATH" >&2
command -v "$CLANG_FORMAT" >&2

"$CLANG_FORMAT" --dry-run --Werror "$@"
