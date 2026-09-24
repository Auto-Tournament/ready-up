#!/usr/bin/env bash
# Fail unless a release libserver.so is portable across CS2 hosts:
#   - DT_NEEDED only glibc components (no libstdc++, libgcc_s, libssl, libpq, libcurl, ...)
#   - no versioned symbol newer than GLIBC_2.31 (Steam Runtime 3 "sniper")
#   - exports the engine entry points (core/src/exports.map) and nothing from static deps
#
#   scripts/ci/check-portable.sh build-sniper/libserver.so
set -euo pipefail

so="${1:?usage: $0 <libserver.so>}"
MAX_GLIBC="${MAX_GLIBC:-2.31}"
fail=0

allowed='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|ld-linux-x86-64\.so\.2)$'
echo "DT_NEEDED:"
while read -r lib; do
  [[ -z "$lib" ]] && continue
  if [[ "$lib" =~ $allowed ]]; then
    echo "  ok   $lib"
  else
    echo "  BAD  $lib (not part of glibc; link it statically)"
    fail=1
  fi
done < <(readelf -dW "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')

newest="$(objdump -T "$so" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n1)"
echo "Newest GLIBC symbol version: ${newest:-none} (max allowed $MAX_GLIBC)"
if [[ -n "$newest" && "$(printf '%s\n%s\n' "$newest" "$MAX_GLIBC" | sort -V | tail -n1)" != "$MAX_GLIBC" ]]; then
  echo "  BAD  requires GLIBC_$newest:"
  objdump -T "$so" | grep -E "GLIBC_($(printf '%s' "$newest" | sed 's/\./\\./g'))" | head -n 10 | sed 's/^/    /'
  fail=1
fi

# The engine entry points from core/src/exports.map must be exported.
for sym in $(sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\);$/\1/p' "$(dirname "$0")/../../core/src/exports.map"); do
  if nm -D --defined-only "$so" | awk '{print $3}' | grep -qx "$sym"; then
    echo "  ok   exports $sym"
  else
    echo "  BAD  $sym is not exported"
    fail=1
  fi
done

# Exported (defined, global) dynamic symbols. Anything from OpenSSL/libpq/libcurl/libstdc++
# showing up here would interpose on the engine's own copies.
leaks="$(nm -D --defined-only "$so" | awk '{print $3}' | grep -E '^(SSL_|OPENSSL_|EVP_|CRYPTO_|PQ|pq|curl_|_ZNSt|_ZSt|__cxa_|__gxx_)' || true)"
if [[ -n "$leaks" ]]; then
  echo "  BAD  static dependency symbols are exported:"
  echo "$leaks" | head -n 20 | sed 's/^/    /'
  fail=1
fi
echo "Exported symbols: $(nm -D --defined-only "$so" | wc -l)"

if [[ "$fail" -ne 0 ]]; then
  echo "check-portable: FAILED" >&2
  exit 1
fi
echo "check-portable: OK"
