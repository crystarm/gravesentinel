#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: run_testbed_check.sh CHECK_SCRIPT" >&2
  exit 2
fi

exec sh "$1"
