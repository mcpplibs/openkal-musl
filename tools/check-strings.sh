#!/usr/bin/env bash
# Builds port/src/okm_string.c for this machine under other names and checks it
# against byte-at-a-time answers with guard pages on both sides of every buffer
# (tools/check-strings.c says why here and not in an example). It is built as a
# program would have it, with the word-at-a-time functions other processors
# use, and on x86_64 once more with the 32-byte loops withheld.
#
#   check-strings.sh [<compiler>]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cc="${1:-${CC:-cc}}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
names="-Dmemcpy=okm_memcpy -Dmemmove=okm_memmove -Dmemset=okm_memset -Dmemcmp=okm_memcmp
       -Dmemchr=okm_memchr -Dstrchr=okm_strchr -Dstrlen=okm_strlen -Dstrcmp=okm_strcmp"
variants="default words"
case "$(uname -m)" in x86_64|amd64) variants="default sse2 words" ;; esac
for v in $variants; do
    extra=""
    [ "$v" = sse2 ] && extra="-DOKM_STRING_AVX2_INITIAL=0"
    [ "$v" = words ] && extra="-DOKM_STRING_WORDS"
    # -fno-builtin: the names above must be called, not expanded in place.
    $cc -O2 -fno-builtin $names $extra -c "$here/port/src/okm_string.c" -o "$out/s.o"
    $cc -O1 -fno-builtin -c "$here/tools/check-strings.c" -o "$out/c.o"
    $cc "$out/c.o" "$out/s.o" -o "$out/check"
    echo "== $v ($cc, $(uname -m))"
    "$out/check"
done
