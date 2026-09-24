#!/bin/sh
# Post-build sanity checks for the ReadyUp shim (run by CMake after linking).
#
# 1. No direct calls to __cxa_pure_virtual. GCC emits these when it devirtualizes a call
#    through an interface mirror that has internal linkage and no implementation in the
#    TU (it "proves" the only target is the pure stub). At runtime that aborts the server
#    with "pure virtual method called" (server-4, build 967edc8).
# 2. readyup_ctor must be the last .init_array entry, so every other translation unit's
#    static initializers have run before it (otherwise they wipe state it set up).
#
# Skips (exit 0) if objdump/readelf/addr2line are unavailable.
set -eu
so="$1"

for tool in objdump readelf addr2line; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "check_shim_binary: $tool not found; skipping checks"
    exit 0
  fi
done

fail=0

pure_calls=$(objdump -d --no-show-raw-insn "$so" | awk '
  /^[0-9a-f]+ <.*>:$/ { fn = $2 }
  /call.*<__cxa_pure_virtual@plt>/ { print fn }' | sort -u)
if [ -n "$pure_calls" ]; then
  echo "check_shim_binary: ERROR: direct calls to __cxa_pure_virtual in:" >&2
  echo "$pure_calls" | c++filt >&2 || echo "$pure_calls" >&2
  echo "  (an interface mirror probably has internal linkage; see src/readyup/sdk/igameevents.h)" >&2
  fail=1
fi

init_addr=$(readelf -SW "$so" | awk '$2 == ".init_array" { print $4 }')
init_size=$(readelf -SW "$so" | awk '$2 == ".init_array" { print $6 }')
if [ -n "$init_addr" ] && [ -n "$init_size" ]; then
  last_slot=$(printf '%x' $((0x$init_addr + 0x$init_size - 8)))
  # .init_array entries are R_X86_64_RELATIVE relocations; the addend is the function.
  last_fn=$(readelf -rW "$so" | awk -v slot="$last_slot" '
    { off = $1; sub(/^0+/, "", off) }
    off == slot && $3 == "R_X86_64_RELATIVE" { print $4 }')
  if [ -n "$last_fn" ]; then
    name=$(addr2line -f -e "$so" "0x$last_fn" | head -n 1)
    case "$name" in
      *readyup_ctor*) ;;
      *)
        echo "check_shim_binary: ERROR: last .init_array entry is '$name', expected readyup_ctor" >&2
        echo "  (keep src/exports.cpp last in the add_library source list)" >&2
        fail=1
        ;;
    esac
  else
    echo "check_shim_binary: could not resolve last .init_array entry; skipping order check"
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "check_shim_binary: OK (no __cxa_pure_virtual calls, readyup_ctor runs last)"
