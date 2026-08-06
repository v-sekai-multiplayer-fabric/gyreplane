# Local CI, via podman + systemd quadlet

`Containerfile` in this directory is a step-by-step mirror of
`.github/workflows/real-build.yml` -- same dependency versions, same
build flags, same per-test compile recipes, in the same order. A pass
here means the same thing a green `real-build.yml` run means.

It exists for one concrete reason: GitHub Actions itself was in a
platform-wide major outage on 2026-08-06 (confirmed via
githubstatus.com -- "Workflow runs are failing or delayed in starting,
and some queued jobs may time out"), and task #18 (reverify the real
build after the mbedtls-to-OpenSSL switch) could not wait on it.

## Usage

```sh
cp ci-local/zone-server-h2o-ci.build.example \
   ~/.config/containers/systemd/zone-server-h2o-ci.build
sed -i "s|REPO_PATH|$(pwd)|g" \
   ~/.config/containers/systemd/zone-server-h2o-ci.build
systemctl --user daemon-reload
systemctl --user start zone-server-h2o-ci-build.service
journalctl --user -u zone-server-h2o-ci-build.service -f
```

`systemctl --user status zone-server-h2o-ci-build.service` after it
finishes reports success or failure the same way a CI job's own
pass/fail does. This is a `Type=oneshot` unit: it builds the image
once and exits, it does not run as a persistent daemon.

```sh
podman run --rm localhost/zone-server-h2o-ci:latest true
```

confirms the built image (and therefore the full build and every
`test/unit/*.c` it ran during `podman build`) is real and intact.

## Why a few packages are listed here that real-build.yml does not list

GitHub's `ubuntu-latest` runner image ships `make`, `clang`,
`zlib1g-dev`, and `adduser` preinstalled. A plain `ubuntu:24.04`
container image does not, so this `Containerfile` installs them
explicitly -- confirmed by the real errors the first few local build
attempts hit without each one (`CMAKE_MAKE_PROGRAM is not set`,
`Could NOT find ZLIB`, FoundationDB's postinst needing `adduser`), not
guessed. Every other dependency and step matches `real-build.yml`
verbatim.
