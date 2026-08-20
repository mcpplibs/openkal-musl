#!/usr/bin/env bash
# Places the specification and the implementation for this system beside this
# package, and points every manifest at the working trees rather than at
# published versions.
#
#   working-trees.sh [<branch>]
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
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
beside="$(cd "$here/.." && pwd)"

case "$(uname -s)" in
    Linux)   implementation=openkal-linux   ;;
    Darwin)  implementation=openkal-macos   ;;
    MINGW*|MSYS*|CYGWIN*) implementation=openkal-windows ;;
    *) echo "no openkal implementation is known for $(uname -s)" >&2; exit 2 ;;
esac

fetch() {
    local repo="$1" at="$beside/$1"
    if [ -d "$at/.git" ]; then
        echo "$repo is already beside this package"
        return
    fi
    git clone --quiet "https://github.com/mcpplibs/$repo.git" "$at"
    if git -C "$at" rev-parse --verify --quiet "origin/$branch" > /dev/null; then
        git -C "$at" checkout --quiet "origin/$branch"
        echo "$repo is at $branch"
    else
        echo "$repo has no $branch; its default branch is used"
    fi
}

fetch openkal
fetch "$implementation"

# On one of the three systems the shell and the build tool disagree about what
# a path is. The translation exists there and is a no-op everywhere else.
native() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}
specification="$(native "$beside/openkal")"

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

echo "$implementation"
