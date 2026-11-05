#!/usr/bin/env bash
# Compile the actual helper with present/absent optional-operation controls.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cc="${1:-${CC:-cc}}"
spec="${2:-$here/../openkal}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
for absent in 0 1; do
    link=()
    # Mach-O requires the intentionally unbound weak name to be allowed at
    # link time. The executed absent-entry control still checks it is null.
    if [ "$absent" = 1 ] && [ "$(uname -s)" = Darwin ]; then
        link=(-Wl,-U,_kal_fs_resolved_name)
    fi
    "$cc" -std=gnu11 -O2 -I"$here/port/src" -iquote "$here/port/include" -I"$spec/include" \
        -include "$here/tools/check-realpath-host.h" \
        -D__okm_realpath=okm_test_realpath -DREALPATH_ABSENT="$absent" \
        "$here/port/src/okm_realpath.c" "$here/tools/check-realpath.c" \
        -o "$out/check-$absent" "${link[@]}"
    "$out/check-$absent"
done
