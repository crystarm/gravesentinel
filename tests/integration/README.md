# Podman/CRIU integration tests

These targets are destructive, host-dependent, and opt-in. They are excluded from
`bazel test //...` by the `manual` tag and also carry `requires-podman`,
`exclusive`, and `local`.

The qualified MVP profile is local **rootful** Podman. Rootless Podman 5.4.2 is
unsupported because it rejects checkpoint creation even when the CRIU and cpuset
prerequisites pass.

Build the static fixture as the normal repository user:

```text
bazel build //tests/integration/fixture:counter_fixture
```

Check prerequisites without creating a container:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  scripts/check_testbed.sh
```

Run the complete qualification smoke only on a disposable, dedicated Linux host:

```text
sudo env \
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  tests/integration/podman_checkpoint_smoke.sh \
  --fixture-binary bazel-bin/tests/integration/fixture/counter_fixture \
  --containerfile tests/integration/fixture/Containerfile \
  --check-script scripts/check_testbed.sh
```

Do not use `sudo bazel` for the normal local workflow. It can create root-owned
workspace/cache files and may select a different Bazel installation. The manual
Bazel targets below are intended for CI or another process already running in the
qualified rootful environment:

```text
bazel test //tests/integration:testbed_check --test_output=streamed
bazel test //tests/integration:podman_checkpoint_smoke \
  --test_output=streamed
```

The smoke builds a `scratch` image without a network pull. It creates only
uniquely named resources with the `gravesentinel-ccm-smoke-` prefix and a per-run
ownership label. On ordinary completion and failure it removes its containers,
image, archive, and observed Podman-managed cgroups. `--keep-artifacts` exists in
the underlying script only for deliberate qualification diagnostics.

A passing prerequisite check is not qualification by itself. The complete smoke
must finish with:

```text
RESULT: QUALIFIED Podman/CRIU checkpoint smoke passed
```

The accepted versions, exact argv, observed artifact entries, security limits,
and two successful qualification runs are recorded in `docs/testbed.md` and
`docs/adr/0001-podman-criu-runtime-profile.md`.
