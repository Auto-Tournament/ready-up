#!/usr/bin/env bash
# Build libserver.so inside a Debian 12 container with libpq + libcurl.
#
#   scripts/docker-build.sh            # Release build -> build-docker/libserver.so
#   BUILD_TYPE=Debug scripts/docker-build.sh
#
# Env:
#   BUILD_TYPE   CMake build type (default Release)
#   BUILD_DIR    output dir relative to repo root (default build-docker)
#   IMAGE        image tag (default readyup-build:bookworm)
#   BUILD_TARGET build only this CMake target (e.g. readyup_plugin_hello); default all
#
# libpq is linked statically (libpq.a + pgcommon/pgport _shlib archives) because
# the server host has no libpq.so.5 and we have no root there. Its remaining
# deps (libssl, libcrypto, libgssapi_krb5, libldap) are present on the host.
#
# TODO(release): switch to registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest
# for release artifacts; bookworm is only used for fast dev iteration.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-readyup-build:bookworm}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-build-docker}"
BUILD_TARGET="${BUILD_TARGET:-}"

docker build -q -t "$IMAGE" -f "$ROOT_DIR/docker/build.Dockerfile" "$ROOT_DIR/docker" >/dev/null

# git worktrees keep their .git as a file pointing at the main repo's gitdir;
# mount that too so `git rev-parse` (version string) works inside the container.
extra_mounts=()
if [[ -f "$ROOT_DIR/.git" ]]; then
  common_dir="$(git -C "$ROOT_DIR" rev-parse --git-common-dir)"
  common_dir="$(cd "$ROOT_DIR" && cd "$common_dir" && pwd)"
  extra_mounts+=(-v "$common_dir:$common_dir:ro")
fi

docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$ROOT_DIR:/src" "${extra_mounts[@]}" \
  -w /src \
  "$IMAGE" \
  bash -c "
    set -euo pipefail
    cmake -S /src -B '/src/$BUILD_DIR' -DCMAKE_BUILD_TYPE='$BUILD_TYPE' -DREADYUP_STATIC_LIBPQ=ON
    cmake --build '/src/$BUILD_DIR' -j\"\$(nproc)\" ${BUILD_TARGET:+--target '$BUILD_TARGET'}
  "

if [[ -n "$BUILD_TARGET" ]]; then
  echo "Built target $BUILD_TARGET in $ROOT_DIR/$BUILD_DIR"
else
  echo "Built: $ROOT_DIR/$BUILD_DIR/libserver.so"
fi
