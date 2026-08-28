#!/usr/bin/env bash
#
# THE ARTEFACT OF THE BUILD THAT JUST RAN, AND NOT ONE OF AN EARLIER
# CONFIGURATION.
#
#   one-artifact.sh <program-name>      # from the directory holding target/
#
# ⚠️⚠️ WHY THIS IS A SCRIPT AND NOT `find | head -1`.
#
# `target/` accumulates ONE DIRECTORY PER CONFIGURATION --- per toolchain, per
# target, and per version of a dependency, because the version is part of the
# fingerprint. A search across all of them followed by `head -1` answers for
# whichever the traversal reached first, which is not the one that was just
# built, and the difference is invisible: an old program runs, prints, and
# reports success.
#
# ⭐ MEASURED WHILE THIS PACKAGE WAS BEING CHANGED, twice in one session. The
# version moved from 0.5.0 to 0.6.0, `examples/subprocess/target` grew a second
# fingerprint directory, and two newly added observations did not appear in the
# output --- of a run that reported `-- failures: 0 --`. The criteria had not
# failed; they had not run, and nothing said so.
#
# ⚠️ IT DOES NOT BITE IN CONTINUOUS INTEGRATION, which is the reason it survives:
# a fresh checkout builds one configuration and there is nothing to choose
# between. It bites on the machine where the change is being written, which is
# where a criterion is trusted most.
#
# So the count is asserted before anything is read, and a tree with more than one
# is a report rather than a guess. The same assertion is written into the steps
# that examine this package's own objects, and it is written once here for the
# steps that run a program.
set -euo pipefail

name="${1:?the name of the program}"

fps=$(ls -d target/*/*/ 2> /dev/null | wc -l | tr -d ' ')
if [ "$fps" != 1 ]; then
    echo "::error::expected one fingerprint directory under $PWD/target, found $fps" >&2
    ls -d target/*/*/ 2> /dev/null | sed 's/^/        /' >&2
    echo "        each names one configuration --- a toolchain, a target, or a" >&2
    echo "        version of a dependency. Remove target/ and build once, or the" >&2
    echo "        program that runs is not the program that was just built." >&2
    exit 1
fi

# ⚠️ BOTH SPELLINGS. One of the three systems appends a suffix, and a search for
# the bare name there finds nothing and reports it as a build that did not
# happen.
binary="$(find target -type f \( -name "$name" -o -name "$name.exe" \) | head -1)"
[ -n "$binary" ] || { echo "::error::$name did not build in $PWD" >&2; exit 1; }

printf '%s\n' "$binary"
