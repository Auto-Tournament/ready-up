#!/usr/bin/env bash
# Print the public-branch buildid of CS2 (app 730) from api.steamcmd.net.
#
#   scripts/ci/cs2-buildid.sh            -> 25492732
set -euo pipefail

url="${CS2_APPINFO_URL:-https://api.steamcmd.net/v1/info/730}"
for attempt in 1 2 3; do
  if json="$(curl -fsSL --max-time 30 "$url")"; then
    if id="$(printf '%s' "$json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(d["data"]["730"]["depots"]["branches"]["public"]["buildid"])
' 2>/dev/null)" && [[ "$id" =~ ^[0-9]+$ ]]; then
      echo "$id"
      exit 0
    fi
  fi
  sleep $((attempt * 5))
done
echo "cs2-buildid: could not read buildid from $url" >&2
exit 1
