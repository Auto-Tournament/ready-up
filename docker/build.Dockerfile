# Dev build image for ReadyUp (libserver.so shim).
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

# The repo is bind-mounted here; git needs to trust it (different owner uid).
RUN git config --system --add safe.directory '*'

WORKDIR /src
