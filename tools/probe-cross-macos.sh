#!/usr/bin/env bash
# Reproduces the two measurements musl/PATCHES.md records for the other system,
# from a machine that is not one.
#
# It answers two questions, and it is written so that either can fail:
#
#   1. How many indirect symbols does a complete build produce? The number
#      recorded is zero; a build that produces any is a build this probe should
#      report, because the linker that would refuse them is still in use.
#
#   2. Which names does that system supply TO THIS PORT'S OWN CODE? The number
#      recorded is two, and they are listed. A third would mean this port has
#      acquired a dependency on that system that nobody decided to acquire.
#
#      ⚠️ That is not the same as the set the LINK needs. `dyld_stub_binder' is
#      referenced by the linker for its own lazy binding and by no source here,
#      and whether it is referenced at all depends on the linker's version:
#      ld64.lld 22 does not, ld64.lld 18 does. So the enumeration below asks
#      what the port calls, and the link below asks whether the stub is
#      sufficient --- two questions, and answering the first does not answer the
#      second. Measured 2026-08-22, when the first was taken for the second and
#      continuous integration disagreed with the machine it was written on.
#
# The second question is asked of the LINKER rather than of the sources. A
# reading of the sources produces a false positive --- a name referenced by a
# source the configured build excludes --- and the linker does not.
#
# Requires: clang, llvm-nm, and ld64.lld. Nothing from the other system.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
arch=${1:-aarch64}
case "$arch" in
    aarch64) triple=arm64-apple-macos14  ;;
    x86_64)  triple=x86_64-apple-macos14 ;;
    *) echo "usage: $0 [aarch64|x86_64]" >&2; exit 2 ;;
esac

CC=${CC:-clang}
NM=${NM:-llvm-nm}
LD64=${LD64:-ld64.lld}
for tool in "$CC" "$NM" "$LD64"; do
    command -v "$tool" >/dev/null || { echo "probe: $tool not found" >&2; exit 2; }
done

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

inc=(-Iport/include -Imusl/src/include -Imusl/src/internal
     -Imusl-generated/internal -Imusl-generated/"$arch"
     -Imusl/arch/"$arch" -Imusl/arch/generic -Imusl/include
     -I"$here"/../openkal/include)
# ⚠️ `-DOKM_MUSL_INTERNAL=1` IS LOAD-BEARING AND WAS ADDED AFTER THIS LIST WAS
# WRITTEN, WHICH IS THE POINT.
#
# It says the unit being compiled is one of musl's own, which is what
# port/include/features.h reads to decide whether `weak`, `hidden` and
# `weak_alias` mean anything. Without it musl's own sources stop at
#
#     crypt_r.c:23: type specifier missing  |  weak_alias(__crypt_r, crypt_r);
#
# ⭐ This list is a SECOND COPY of the manifest's, and the comment below already
# says keeping it in step is what makes the answer the configured one. It went
# out of step the first time the manifest gained a flag, and continuous
# integration is what said so. Two places for one decision, and this is the
# other one.
cflags=(-std=c99 -D_XOPEN_SOURCE=700 -DOKM_MUSL_INTERNAL=1
        -ffreestanding -nostdinc
        -fno-stack-protector -fno-strict-aliasing -frounding-math -w
        -ffunction-sections -fdata-sections)

cd "$here"

# The configured source set: musl's own rule, minus the ten this port replaces,
# minus the one this system's build excludes. Keeping this list in step with
# mcpp.toml is what makes question 2's answer the configured one.
#
# ⚠️⚠️ AND `posix_spawnp' IS NAMED SEPARATELY, WHICH IS NOT REDUNDANT. The match
# is anchored on the whole basename, so `posix_spawn' does NOT cover
# `posix_spawnp.c' --- and when that source became the tenth this port replaces,
# this list said nothing and the link reported
#
#     ld64.lld: error: duplicate symbol: _posix_spawnp
#
# ⭐ Which is the whole reason this list carries the warning it does: it is a
# SECOND statement of what mcpp.toml already states, and a second statement is
# a thing that falls behind the first. It fell behind on the release that added
# the tenth entry, and it is this job that said so.
skip='__libc_start_main|__init_tls|__set_thread_area|clone|posix_spawn|posix_spawnp|mmap|syscall_ret|getcwd|dl_iterate_phdr|okm_phdr|cache'
units=0
for f in musl/src/*/*.c musl/src/malloc/mallocng/*.c port/src/*.c port/src/*.S; do
    base=$(basename "$f"); base=${base%.*}
    [[ "$base" =~ ^($skip)$ ]] && continue
    "$CC" --target="$triple" "${cflags[@]}" "${inc[@]}" -c "$f" \
        -o "$out/$(echo "$f" | tr / _).o"
    units=$((units + 1))
done

# The implementation beneath, without which there is nothing to link.
impl=$here/../openkal-macos/src
if [ -d "$impl" ]; then
    for f in "$impl"/*.cpp; do
        "$CC"++ --target="$triple" -std=c++23 -fno-exceptions -fno-rtti \
            -fno-stack-protector -DOKM_STANDALONE -nostdinc++ -w \
            -ffunction-sections -fdata-sections -I"$here"/../openkal/include \
            -c "$f" -o "$out/impl_$(basename "$f" .cpp).o"
        units=$((units + 1))
    done
else
    echo "probe: ../openkal-macos not present; question 2 cannot be asked" >&2
    exit 2
fi

echo "probe: $units objects for $triple"

# ---- question 1 -------------------------------------------------------------
indirect=$("$NM" -m "$out"/*.o 2>/dev/null | grep -ci indirect || true)
echo "probe: indirect symbols = $indirect (recorded: 0)"

# ---- question 2 -------------------------------------------------------------
# The probe has been shown to fail as well as to pass: a source referencing one
# further name was added to port/src, and both assertions below went red and the
# link with the stub failed. A probe that has only ever passed is not yet known
# to be a probe.
printf 'int main(void){return 0;}\n' > "$out/probe_main.c"
"$CC" --target="$triple" "${cflags[@]}" "${inc[@]}" -c "$out/probe_main.c" \
    -o "$out/zz_probe_main.o"

set +e
undef=$("$LD64" -arch "${triple%%-*}" -platform_version macos 14.0 14.0 \
        -o /dev/null "$out"/*.o -e _okm_start -dead_strip 2>&1 \
        | sed -n 's/.*undefined symbol: //p' | sort -u \
        | grep -vx 'dyld_stub_binder' || true)
set -e

# `dyld_stub_binder' is filtered out above rather than listed here, because it
# is not an answer to this question: no source names it, the linker emits the
# reference for its own lazy binding, and whether it appears at all depends on
# the linker's version. It belongs to the stub, which the link below exercises.
expected=$'_clock_gettime_nsec_np\n_pthread_create_from_mach_thread'
echo "probe: names this system supplies:"
printf '  %s\n' $undef

fail=0
[ "$indirect" -eq 0 ] || { echo "FAIL: $indirect indirect symbols; recorded 0" >&2; fail=1; }
[ "$undef" = "$expected" ] || {
    echo "FAIL: the set of names differs from the recorded two" >&2
    echo "  recorded:" >&2; printf '    %s\n' $expected >&2
    fail=1
}

# The link must also succeed once the stub is named, which is the claim the
# whole probe exists to support.
"$LD64" -arch "${triple%%-*}" -platform_version macos 14.0 14.0 \
    -o "$out/probe.out" "$out"/*.o "$here"/../openkal-macos/port/libSystem.tbd \
    -e _okm_start -dead_strip \
    || { echo "FAIL: link with the stub did not succeed" >&2; fail=1; }

[ "$fail" -eq 0 ] && echo "probe: ok"
exit "$fail"
