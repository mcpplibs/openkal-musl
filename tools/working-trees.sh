#!/usr/bin/env bash
# Places the specification and the implementation for this system beside this
# package, and points every manifest at the working trees rather than at
# published versions.
#
#   working-trees.sh [<branch>] [<target-triple>]
#
# Why this exists rather than a version in the manifest: a run asserts that this
# package as written here and the specification and implementation as written
# there agree today. A version names what agreed when it was published, which is
# a different and weaker claim, and is the one that holds after a release.
#
# The manifest names the implementation by a path already, and that is not a
# development convenience --- see the comment beside it. What this script
# supplies is the tree that path refers to.
#
# The working trees are modified in place. That is intended: this runs in
# checkouts that exist for the length of one job.
set -euo pipefail

branch="${1:-main}"
target="${2:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
beside="$(cd "$here/.." && pwd)"

# The implementation is chosen by what is being built for, not by what is
# building. A cross build asks the host which implementation it needs and is
# told the host's, and what then happens is that the implementation the build
# actually uses comes from the index while everything around it has been pointed
# at a working tree --- reported as one package requested as a path and as a
# version at once, which names neither the host nor the target.
case "${target:-$(uname -s)}" in
    *windows*|MINGW*|MSYS*|CYGWIN*) implementation=openkal-windows ;;
    *macos*|*darwin*|Darwin)        implementation=openkal-macos   ;;
    *linux*|Linux)                  implementation=openkal-linux   ;;
    *) echo "no openkal implementation is known for ${target:-$(uname -s)}" >&2; exit 2 ;;
esac
echo "building for ${target:-$(uname -s)}, so the implementation is $implementation"

fetch() {
    local repo="$1" at="$beside/$1"
    if [ -d "$at/.git" ]; then
        echo "$repo is already beside this package"
        return
    fi
    git clone --quiet "https://github.com/mcpplibs/$repo.git" "$at"
    if git -C "$at" rev-parse --verify --quiet "origin/$branch" > /dev/null; then
        git -C "$at" checkout --quiet "origin/$branch"
        echo "$repo is at $branch $(git -C "$at" rev-parse --short HEAD)"
    else
        echo "$repo has no $branch; its default branch is used" \
             "($(git -C "$at" rev-parse --short HEAD))"
    fi
}

fetch openkal
fetch "$implementation"

# ⭐ AND WHAT WAS FETCHED HAS TO BE WHAT THE MANIFEST ASKED FOR.
#
# Substituting a working tree for a version removes the one check that would
# otherwise happen: the resolver never sees a version requirement, so a tree of
# any age satisfies it. `fetch` above falls back to the default branch when a
# repository has no branch of this name, which is right — most branches here
# have no counterpart — but it means a change that spans two repositories is
# built against whichever half happens to be on `main`.
#
# ⚠️ THAT FALLBACK ONCE PRODUCED A FAILURE THAT NAMED THE WRONG THING. This
# package's branch was `feat/getrandom-through-openkal` while the
# specification's was `feat/openkal-random`, so the fallback supplied openkal
# 0.6.0 to a manifest asking for 0.7.0, and five jobs reported:
#
#     port/src/okm_syscall.c:27:10: fatal error: 'openkal/random.h' file not found
#
# A missing header reads as a mistake in this package. The mistake was that the
# two halves were not in step, which is what this says instead. A hard failure
# is also the only safe answer: had the specification's change been additive,
# the same mismatch would have passed while testing the wrong specification.
satisfies() {   # satisfies <required> <actual> --- caret, for 0.x and above
    local rq="$1" ac="$2"
    local rmaj="${rq%%.*}" amaj="${ac%%.*}"
    [ "$rmaj" = "$amaj" ] || return 1
    local rrest="${rq#*.}" arest="${ac#*.}"
    local rmin="${rrest%%.*}" amin="${arest%%.*}"
    if [ "$rmaj" = "0" ]; then
        # Below 1.0 a minor bump is a breaking change, so it must match exactly
        # and only the patch may move forward.
        [ "$rmin" = "$amin" ] || return 1
        [ "${arest#*.}" -ge "${rrest#*.}" ] || return 1
    else
        [ "$amin" -gt "$rmin" ] || { [ "$amin" = "$rmin" ] &&
            [ "${arest#*.}" -ge "${rrest#*.}" ]; } || return 1
    fi
}

in_step() {   # in_step <package> <tree>
    local pkg="$1" tree="$2"
    local want actual
    want="$(sed -n "s/^$pkg *= *\"\([0-9.]*\)\".*/\1/p;
                    s/^$pkg *= *{.*version *= *\"\([0-9.]*\)\".*/\1/p" \
                 "$here/mcpp.toml" | head -1)"
    actual="$(sed -n 's/^version *= *"\([0-9.]*\)".*/\1/p' "$tree/mcpp.toml" | head -1)"
    if [ -z "$want" ] || [ -z "$actual" ]; then
        echo "cannot read a version for $pkg (asked '$want', tree '$actual')" >&2
        exit 2
    fi
    if satisfies "$want" "$actual"; then
        echo "$pkg: this package asks for $want and the tree is $actual"
        return
    fi
    echo "" >&2
    echo "this package is written against $pkg $want and the tree beside it" >&2
    echo "is $actual. Nothing is wrong with either; they are not in step." >&2
    echo "" >&2
    echo "The trees are taken from the branch named '$branch' where a" >&2
    echo "repository has one, so a change spanning both is built as a whole" >&2
    echo "only when both use that name." >&2
    echo "" >&2
    exit 1
}

in_step openkal "$beside/openkal"
in_step "$implementation" "$beside/$implementation"

# On one of the three systems the shell and the build tool disagree about what
# a path is. The translation exists there and is a no-op everywhere else.
native() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
specification="$(native "$beside/openkal")"

# ⚠️ THE REWRITE IS PERMANENT AND IS MEANT TO BE. This runs in a checkout that
# is thrown away, and the manifests must keep naming the working trees for the
# rest of the job, so there is no trap restoring them.
#
# THE CONSEQUENCE FALLS ON WHOEVER RUNS IT BY HAND. A local run leaves this
# repository's manifest naming absolute paths on that machine, and committing
# that publishes them: a consumer resolving from the index is handed a manifest
# pointing at a directory that exists nowhere. That has happened once. The
# workflow asserts against it rather than relying on this note, because a note
# is read by whoever already knows.
#
# A package may be reached by a path or by a version and not by both at once, so
# every manifest that reaches openkal is rewritten and not only this one.
#
# Not `sed -i`: BSD sed reads the next word as a backup suffix, so the GNU form
# fails on macOS. This form is the same on every system.
point() {
    [ -f "$1" ] || return 0
    sed "s|^openkal = .*$|openkal = { path = \"$specification\" }|" "$1" > "$1.next"
    mv "$1.next" "$1"
    echo "pointed $1 at the specification's working tree"
}
point "$here/mcpp.toml"
point "$beside/$implementation/mcpp.toml"

# And this package's own reference to the implementation. It names a version,
# because a published package must: a path names a directory that exists in the
# working tree it was written in and nowhere else, and a consumer resolving from
# the index would be handed a manifest pointing at nothing. The feature travels
# with the version, because a program above this library carries no other
# runtime and that is what the feature states.
impl_path="$(native "$beside/$implementation")"
sed "s|^$implementation = .*$|$implementation = { path = \"$impl_path\", features = [\"standalone\"] }|" \
    "$here/mcpp.toml" > "$here/mcpp.toml.next"
mv "$here/mcpp.toml.next" "$here/mcpp.toml"
echo "pointed this package at $implementation's working tree"
sed -n '/^\[target/,/^$/p' "$here/mcpp.toml"

echo "$implementation"
