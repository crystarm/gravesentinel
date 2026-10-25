# Release and acceptance gates

The MVP has two distinct gates. Neither substitutes for the other.

## Non-privileged release gate

Run as the normal repository user:

```sh
sh scripts/release_gate.sh
```

Equivalent Bazel executable target:

```sh
bazel run //scripts:release_gate
```

The script performs, in order:

1. `bazel test //scripts:clang_format_test --nocache_test_results`;
2. `bazel build --config=release //...`;
3. `bazel test //... --nocache_test_results`;
4. ASan `//:unit_tests`;
5. UBSan `//:unit_tests`;
6. TSan `//:unit_tests`;
7. the external `SchedulerClient` visibility boundary check;
8. the external client-to-separate-daemon runtime check;
9. a scan for `TODO`, `FIXME`, and `TBD` markers in mandatory MVP source,
   tests, scripts, public headers, and consumer paths.

Success ends with:

```text
RESULT: QUALIFIED non-privileged release gate passed
```

The sanitizer profiles deliberately target the unit suite. The rootful static
fixture and Podman/CRIU suites are not run under sanitizers.

On 2026-10-25 the complete non-privileged script passed in the development
workspace and emitted:

```text
RESULT: QUALIFIED non-privileged release gate passed
```

This evidence covers the repository build/test/client boundary, not Podman/CRIU.

## Rootful acceptance gate

Build all artifacts as the normal user:

```sh
bazel build \
  //src/hypervisor:gravesentinel_hypervisor \
  //tests/integration:fault_hypervisor \
  //tests/integration:hypervisor_crash_recovery_driver \
  //tests/integration:hypervisor_four_cpu_driver \
  //tests/integration:hypervisor_e2e_driver \
  //tests/integration/fixture:counter_fixture
```

Then run only the shell wrappers with root privileges, following
[`../tests/integration/README.md`](../tests/integration/README.md). The complete
manual Bazel aggregate is:

```text
//tests/integration:stage13_acceptance
```

It contains:

- `//tests/integration:hypervisor_scheduler_e2e`;
- `//tests/integration:hypervisor_crash_recovery_e2e`;
- `//tests/integration:hypervisor_four_cpu_e2e`.

Do not use `sudo bazel`. The aggregate target is for a CI runner already operating
inside the qualified rootful environment; an interactive developer should build
normally and invoke the three wrappers through `sudo env ...`.

A current complete acceptance requires all three markers:

```text
RESULT: QUALIFIED Hypervisor + SchedulerClient E2E passed
RESULT: QUALIFIED crash/reconnect/recovery acceptance passed
RESULT: QUALIFIED four-container CPU acceptance passed
```

The final marker is printed only after functional assertions and strict cleanup
succeed. A prerequisite check, successful build, or non-privileged simulation is
not rootful qualification.

## Qualification status

The non-privileged release gate produced its marker on 2026-10-25. On the same date,
stage 12 was requalified against the current production startup-recovery code, the
four-container CPU suite qualified twice, and the crash/reconnect/recovery suite
qualified once. All three current-code rootful acceptance markers are recorded, so
stage 13 and the MVP are qualified for the dated testbed.

## Requalification triggers

Repeat the non-privileged gate after source, build, dependency, public API, or
protocol changes. Repeat the rootful prerequisite and all affected destructive
suites after changes to:

- hypervisor composition, IPC, storage, journal, create/restore, maintenance, or
  runtime code;
- kernel, Podman, crun, CRIU, cgroup, CPU, privilege, AppArmor, SELinux, seccomp,
  or capability profile;
- runtime command argv, fixture, container security options, or cleanup logic.
