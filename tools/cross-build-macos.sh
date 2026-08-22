#!/usr/bin/env bash
# Builds a program for the other system, from a machine that is not one.
#
# The probe beside this file answers whether the objects are right and whether
# the stub is sufficient. This one answers the question neither of them can:
# whether what comes out STARTS. Those are different questions, and the second
# is not implied by the first --- a Mach-O that links may still be refused by
# that system's kernel, which on its 64-bit ARM variant requires a signature
# before it will start anything.
#
# So this script produces the artifact and stops. Signing and running happen on
# a machine of that system, because that is the only place they can happen, and
# the workflow carries the file there.
#
#     cross-build-macos.sh <aarch64|x86_64> <program.c> <output>
#
# Requires: clang, ld64.lld, and the two sibling checkouts. Nothing from the
# other system.
set -euo pipefail

arch=${1:?usage: cross-build-macos.sh <aarch64|x86_64> <program.c> <output>}
program=${2:?usage: cross-build-macos.sh <aarch64|x86_64> <program.c> <output>}
output=${3:?usage: cross-build-macos.sh <aarch64|x86_64> <program.c> <output>}

case "$arch" in
    aarch64) triple=arm64-apple-macos14  ;;
    x86_64)  triple=x86_64-apple-macos14 ;;
    *) echo "cross-build: unknown architecture $arch" >&2; exit 2 ;;
esac

here=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-clang}
LD64=${LD64:-ld64.lld}
stub="$here/../openkal-macos/port/libSystem.tbd"
[ -f "$stub" ] || { echo "cross-build: no stub at $stub" >&2; exit 2; }

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

inc=(-Iport/include -Imusl/src/include -Imusl/src/internal
     -Imusl-generated/internal -Imusl-generated/"$arch"
     -Imusl/arch/"$arch" -Imusl/arch/generic -Imusl/include
     -I"$here"/../openkal/include)
cflags=(-std=c99 -D_XOPEN_SOURCE=700 -ffreestanding -nostdinc
        -fno-stack-protector -fno-strict-aliasing -frounding-math -w
        -ffunction-sections -fdata-sections)

program=$(cd "$(dirname "$program")" && pwd)/$(basename "$program")
cd "$here"

# Kept in step with mcpp.toml, INCLUDING that system's own exclusions:
# okm_phdr.c answers dl_iterate_phdr from an ELF header and that format has none.
skip='__libc_start_main|__init_tls|__set_thread_area|clone|posix_spawn|mmap|syscall_ret|getcwd|dl_iterate_phdr|okm_phdr'
for f in musl/src/*/*.c musl/src/malloc/mallocng/*.c port/src/*.c port/src/*.S; do
    base=$(basename "$f"); base=${base%.*}
    [[ "$base" =~ ^($skip)$ ]] && continue
    "$CC" --target="$triple" "${cflags[@]}" "${inc[@]}" -c "$f" \
        -o "$out/$(echo "$f" | tr / _).o"
done

for f in "$here"/../openkal-macos/src/*.cpp; do
    "$CC"++ --target="$triple" -std=c++23 -fno-exceptions -fno-rtti \
        -fno-stack-protector -DOKM_STANDALONE -nostdinc++ -w \
        -ffunction-sections -fdata-sections -I"$here"/../openkal/include \
        -c "$f" -o "$out/impl_$(basename "$f" .cpp).o"
done

"$CC" --target="$triple" "${cflags[@]}" "${inc[@]}" -c "$program" \
    -o "$out/zz_program.o"

# -e names the entry, because this format records one in the image header
# rather than looking for a fixed symbol; -dead_strip is this linker's spelling
# of discarding what nothing reaches.
"$LD64" -arch "${triple%%-*}" -platform_version macos 14.0 14.0 \
    -o "$output" "$out"/*.o "$stub" -e _okm_start -dead_strip

echo "cross-build: $output"
