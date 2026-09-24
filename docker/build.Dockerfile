# Dev build image for Ready Up (libserver.so shim).
#
# Debian 12 (bookworm) matches the cs2 host that runs the dedicated server, so
# the shared libs we link dynamically (libcurl, libssl, libgssapi_krb5, ...)
# resolve against the same sonames at runtime.
#
# NOTE: release builds should move to the Steam Runtime "sniper" SDK image
# (registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest), which is what
# CS2 itself targets. bookworm is used here because it is small and fast.
FROM debian:bookworm

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake git python3 ca-certificates \
      libpq-dev postgresql-server-dev-15 \
      libcurl4-openssl-dev \
      libssl-dev libkrb5-dev libldap2-dev \
 && rm -rf /var/lib/apt/lists/*

# plugins/fleet needs libcurl with WebSockets, which bookworm's libcurl 7.88 does not have.
# Build the curl 8.22 the release uses (scripts/ci/build-static-deps.sh) as a static PIC archive
# against the system OpenSSL into /opt/curl-ws, where plugins/fleet/CMakeLists.txt finds it.
ARG CURL_VER=8.22.0
ARG CURL_SHA=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl xz-utils pkg-config \
 && rm -rf /var/lib/apt/lists/* \
 && cd /tmp \
 && curl -fsSL --retry 3 -o curl.tar.xz "https://curl.se/download/curl-${CURL_VER}.tar.xz" \
 && echo "${CURL_SHA}  curl.tar.xz" | sha256sum -c --quiet - \
 && tar xf curl.tar.xz \
 && cd "curl-${CURL_VER}" \
 && CFLAGS="-O2 -fPIC -fvisibility=hidden" ./configure --prefix=/opt/curl-ws --disable-shared --enable-static \
      --with-pic --with-openssl --enable-websockets --enable-threaded-resolver \
      --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs \
      --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 \
      --without-ngtcp2 --without-libidn2 --without-libpsl --without-libssh2 --without-libgsasl \
      --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
      --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
      --disable-manual --disable-docs >/dev/null \
 && ! grep -q "^#define CURL_DISABLE_WEBSOCKETS 1" lib/curl_config.h \
 && make -C lib -j"$(nproc)" >/dev/null \
 && make -C lib install >/dev/null \
 && make -C include install >/dev/null \
 && cd / && rm -rf /tmp/curl*

# The repo is bind-mounted here; git needs to trust it (different owner uid).
RUN git config --system --add safe.directory '*'

WORKDIR /src
