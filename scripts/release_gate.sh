#!/bin/sh
set -eu

if [ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]; then
  repository_root=$BUILD_WORKSPACE_DIRECTORY
else
  script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  repository_root=$(CDPATH= cd -- "$script_directory/.." && pwd)
fi
bazel_executable=${BAZEL:-bazel}

cd "$repository_root"

printf '%s\n' 'GATE: clang-format'
"$bazel_executable" test //scripts:clang_format_test --nocache_test_results

printf '%s\n' 'GATE: optimized release build'
"$bazel_executable" build --config=release //...

printf '%s\n' 'GATE: default non-privileged tests'
"$bazel_executable" test //... --nocache_test_results

printf '%s\n' 'GATE: AddressSanitizer unit suite'
"$bazel_executable" test --config=asan //:unit_tests --nocache_test_results

printf '%s\n' 'GATE: UndefinedBehaviorSanitizer unit suite'
"$bazel_executable" test --config=ubsan //:unit_tests --nocache_test_results

printf '%s\n' 'GATE: ThreadSanitizer unit suite'
"$bazel_executable" test --config=tsan //:unit_tests --nocache_test_results

printf '%s\n' 'GATE: external SchedulerClient dependency boundary'
sh scripts/check_scheduler_client_boundary.sh

printf '%s\n' 'GATE: external SchedulerClient runtime consumer'
sh scripts/check_scheduler_client_consumer_runtime.sh

printf '%s\n' 'GATE: no mandatory-path placeholder markers'
placeholder_pattern='TO''DO|FIX''ME|T''BD'
if find src tests scripts build_defs include examples/scheduler_client_consumer \
  -type f \( -name '*.cxx' -o -name '*.hxx' -o -name '*.bzl' -o \
  -name '*.sh' \) -exec grep -nH -E "$placeholder_pattern" {} \; | grep .; then
  echo 'mandatory MVP paths contain placeholder markers' >&2
  exit 1
fi

echo 'RESULT: QUALIFIED non-privileged release gate passed'
