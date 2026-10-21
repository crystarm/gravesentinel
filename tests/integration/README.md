# Integration tests

Stage 01 intentionally contains no Podman/CRIU integration test. Privileged tests
will be added only after the stage 02 testbed qualification and will carry the
`manual`, `requires-podman`, and `exclusive` Bazel tags.
