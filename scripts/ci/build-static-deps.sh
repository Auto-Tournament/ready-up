#!/usr/bin/env bash
# Build OpenSSL + libpq + libcurl as static PIC archives into a prefix, so the
# ReadyUp shim can link them in and depend on nothing but glibc at runtime.
#
#   scripts/ci/build-static-deps.sh /opt/readyup-deps
#
# Meant to run inside the Steam Runtime 3 "sniper" SDK image (glibc 2.31), which
# is what CS2 targets; see scripts/sniper-build.sh / .github/workflows/build.yml.
#
# Everything is pinned by version + sha256. Bump DEPS_REV below whenever you
# change versions or flags: CI keys its cache on this file's hash anyway.
#
# Choices (keep the runtime surface small and host-independent):
#   OpenSSL  no-shared, no-module/no-engine/no-dso (nothing dlopen'd),
#            no-autoload-config (never reads the host's openssl.cnf, which may be
#            written for a different OpenSSL), openssldir=/etc/ssl.
#   libpq    --with-openssl, no GSSAPI / LDAP / readline / ICU / zlib.
#   libcurl  HTTP(S) only with OpenSSL; no zlib/brotli/zstd/nghttp2/idn/psl/ldap/ssh.
#            CA bundle is probed at runtime by http_client.cpp (READYUP_CURL_CA_PROBE)
#            because the default path differs between distros.
set -euo pipefail

PREFIX="${1:?usage: $0 <prefix>}"
DEPS_REV=1
JOBS="${JOBS:-$(nproc)}"
WORK="${DEPS_WORK:-$(mktemp -d)}"

OPENSSL_VER=3.5.8
OPENSSL_SHA=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"

PG_VER=17.11
PG_SHA=dd27f2b3c59e73ed14aa3324901242bf69a032a6347805f274e6260322d42979
PG_URL="https://ftp.postgresql.org/pub/source/v${PG_VER}/postgresql-${PG_VER}.tar.bz2"

CURL_VER=8.22.0
CURL_SHA=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
CURL_URL="https://curl.se/download/curl-${CURL_VER}.tar.xz"

STAMP="$PREFIX/.readyup-deps"
WANT_STAMP="rev=$DEPS_REV openssl=$OPENSSL_VER pg=$PG_VER curl=$CURL_VER"
if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$WANT_STAMP" ]]; then
  echo "static deps already built in $PREFIX ($WANT_STAMP)"
  exit 0
fi

export CFLAGS="-O2 -fPIC -fvisibility=hidden"
export CC="${CC:-gcc}"

fetch() {  # url sha256 -> extracted dir name on stdout
  local url="$1" sha="$2" file
  file="$WORK/$(basename "$url")"
  if [[ ! -f "$file" ]]; then
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  echo "$sha  $file" | sha256sum -c --quiet - >&2
  tar -C "$WORK" -xf "$file"
}

mkdir -p "$PREFIX" "$WORK"
echo "Building static deps into $PREFIX (work: $WORK, jobs: $JOBS)"

# Per-component stamps let an interrupted run resume without redoing OpenSSL.
done_step() { [[ -f "$PREFIX/.step-$1-$DEPS_REV" ]]; }
mark_step() { touch "$PREFIX/.step-$1-$DEPS_REV"; }

# --- OpenSSL -----------------------------------------------------------------
if ! done_step "openssl-$OPENSSL_VER"; then
fetch "$OPENSSL_URL" "$OPENSSL_SHA"
(
  cd "$WORK/openssl-$OPENSSL_VER"
  ./Configure linux-x86_64 \
    --prefix="$PREFIX" --libdir=lib --openssldir=/etc/ssl \
    no-shared no-module no-engine no-dso no-tests no-docs no-apps \
    no-autoload-config no-comp no-ui-console \
    -fPIC -fvisibility=hidden >/dev/null
  make -j"$JOBS" build_libs >/dev/null
  make install_dev >/dev/null 2>&1
)
mark_step "openssl-$OPENSSL_VER"
fi

# --- libpq (client library only) ---------------------------------------------
if ! done_step "pg-$PG_VER"; then
fetch "$PG_URL" "$PG_SHA"
(
  cd "$WORK/postgresql-$PG_VER"
  CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" LIBS="-ldl -pthread" \
  ./configure --prefix="$PREFIX" \
    --with-openssl --without-readline --without-zlib --without-icu \
    --without-gssapi --without-ldap --without-pam --without-libxml \
    --without-llvm --without-zstd --without-lz4 >/dev/null
  make -C src/include -j"$JOBS" >/dev/null
  make -C src/common -j"$JOBS" >/dev/null
  make -C src/port -j"$JOBS" >/dev/null
  # Static archive only: libpq's shared-lib "no exit() references" check
  # (libpq-refs-stamp) trips on static OpenSSL, and we never want libpq.so anyway.
  make -C src/interfaces/libpq -j"$JOBS" libpq.a >/dev/null
  make -C src/include install >/dev/null
  make -C src/interfaces/libpq install-lib-static >/dev/null
  install -m 644 src/interfaces/libpq/libpq-fe.h src/interfaces/libpq/libpq-events.h "$PREFIX/include/"
  make -C src/common install >/dev/null
  make -C src/port install >/dev/null
) 2>&1 | grep -v -e '^DEBUG' -e 'uninitialized value' || true
[[ -f "$PREFIX/lib/libpq.a" ]] || { echo "libpq.a build failed" >&2; exit 1; }
mark_step "pg-$PG_VER"
fi

# --- libcurl -----------------------------------------------------------------
fetch "$CURL_URL" "$CURL_SHA"
(
  cd "$WORK/curl-$CURL_VER"
  PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" LIBS="-ldl -pthread" \
  ./configure --prefix="$PREFIX" --disable-shared --enable-static --with-pic \
    --with-openssl="$PREFIX" \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs \
    --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 --without-libidn2 --without-libpsl --without-libssh2 \
    --without-libgsasl --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
    --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
    --disable-gopher --disable-mqtt --disable-smb --disable-manual --disable-docs \
    --enable-threaded-resolver >/dev/null
  make -C lib -j"$JOBS" >/dev/null
  make -C lib install >/dev/null
  make -C include install >/dev/null
  make install-pkgconfigDATA >/dev/null 2>&1 || true
)

rm -f "$PREFIX"/.step-*
echo "$WANT_STAMP" >"$STAMP"
echo "static deps ready:"
ls -1 "$PREFIX"/lib/*.a
