#!/usr/bin/env bash
# Release build in Valve's Steam Runtime 3 "sniper" SDK container (same as CI).
#
#   scripts/sniper-build.sh            # -> build-sniper/libserver.so
#
# The result links OpenSSL, libpq, libcurl, libstdc++ and libgcc statically and only
# needs glibc >= 2.31 on the host, so it runs on any CS2 dedicated server.
# Static deps are cached in $DEPS_CACHE (default ~/.cache/readyup-sniper-deps).
# For fast dev iteration against the local host, scripts/docker-build.sh is still fine.
#
# Env:
#   BUILD_TYPE   CMake build type (default Release)
#   BUILD_DIR    output dir relative to repo root (default build-sniper)
#   SNIPER_IMAGE (default registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest)
#   DEPS_CACHE   host dir for the static deps prefix + tarballs
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SNIPER_IMAGE="${SNIPER_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-build-sniper}"
DEPS_CACHE="${DEPS_CACHE:-$HOME/.cache/readyup-sniper-deps}"
mkdir -p "$DEPS_CACHE/prefix" "$DEPS_CACHE/work"

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
  -e HOME=/tmp -e BUILD_TYPE="$BUILD_TYPE" \
  -e DEPS_PREFIX=/deps -e DEPS_WORK=/deps-work \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
  -v "$ROOT_DIR:/src" "${extra_mounts[@]}" \
  -v "$DEPS_CACHE/prefix:/deps" -v "$DEPS_CACHE/work:/deps-work" \
  -w /src \
  "$SNIPER_IMAGE" \
  /src/scripts/ci/build-release.sh "/src/$BUILD_DIR"

echo "Built: $ROOT_DIR/$BUILD_DIR/libserver.so"
