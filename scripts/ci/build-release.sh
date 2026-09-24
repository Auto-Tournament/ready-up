#!/usr/bin/env bash
# Release build of the Ready Up shim. Runs INSIDE the Steam Runtime 3 "sniper" SDK
# (registry.gitlab.steamos.cloud/steamrt/sniper/sdk), either as the CI job container
# (.github/workflows/build.yml) or via scripts/sniper-build.sh locally.
#
#   scripts/ci/build-release.sh [build-dir]      (default: build-sniper)
#
# Env:
#   DEPS_PREFIX  where static OpenSSL/libpq/libcurl live / get built (default /opt/readyup-deps)
#   BUILD_TYPE   CMake build type (default Release)
#   CC / CXX     compilers (default gcc-14 / g++-14 from the SDK: sniper's default GCC 10
#                libstdc++ rejects std::unordered_map with an incomplete value type, which
#                minijson::Value uses. GCC 14 in sniper still targets glibc 2.31, and its
#                libstdc++/libgcc are linked statically, so the host's versions don't matter.)
#
# Produces <build-dir>/libserver.so (only glibc <= 2.31 needed at runtime) plus the
# readyup_sigcheck / readyup_hookcheck tools, then runs scripts/ci/check-portable.sh.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-build-sniper}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;; esac
DEPS_PREFIX="${DEPS_PREFIX:-/opt/readyup-deps}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
if command -v g++-14 >/dev/null 2>&1; then
  export CC="${CC:-gcc-14}" CXX="${CXX:-g++-14}"
fi

if [[ -r /etc/os-release ]] && ! grep -q 'sniper' /etc/os-release; then
  echo "WARNING: not running in the Steam Runtime sniper SDK; the result may need a newer glibc." >&2
fi

"$ROOT_DIR/scripts/ci/build-static-deps.sh" "$DEPS_PREFIX"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DREADYUP_DEPS_PREFIX="$DEPS_PREFIX"
cmake --build "$BUILD_DIR" -j"$(nproc)"

"$ROOT_DIR/scripts/ci/check-portable.sh" "$BUILD_DIR/libserver.so"
for so in "$BUILD_DIR"/plugins/*.so; do
  "$ROOT_DIR/scripts/ci/check-portable.sh" "$so" "$ROOT_DIR/plugins/plugin.map"
done
echo "Built: $BUILD_DIR/libserver.so"
