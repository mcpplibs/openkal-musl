#!/usr/bin/env bash
# Compile the actual helper with present/absent optional-operation controls.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cc="${1:-${CC:-cc}}"
spec="${2:-$here/../openkal}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for absent in 0 1; do
    "$cc" -std=gnu11 -O2 -I"$here/port/src" -iquote "$here/port/include" -I"$spec/include" \
        -D__okm_realpath=okm_test_realpath -DREALPATH_ABSENT="$absent" \
        "$here/port/src/okm_realpath.c" "$here/tools/check-realpath.c" \
        -o "$out/check-$absent"
    "$out/check-$absent"
done
