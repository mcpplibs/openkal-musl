#!/usr/bin/env bash
# Asserts `[c-abi.absent]` against the archive this package actually builds.
#
#   check-absent.sh <manifest> <archive-or-object>...
#
# The table in mcpp.toml states, for each facility this C library does not
# supply, the SHAPE in which the absence reaches a program. Two of the three
# shapes are checkable from the artefact alone, and they are checked in
# opposite directions:
#
#   form = "link"      the definition must NOT be in the archive. This is the
#                      shape openkal's capability model requires of an
#                      implementation (SPEC 0.14 clause 6.1), and a name that
#                      turned out to be defined would mean a program links and
#                      then meets the absence somewhere it cannot read it.
#   form = "enosys"    the definition MUST be in the archive. A name that
#                      turned out to be missing would fail the link instead,
#                      which is a different contract from the one stated --- and
#                      the one a caller prepared to read `ENOSYS` is not
#                      prepared for.
#
# `accepted-no-effect` is not checkable here: it is a statement about what a
# call does, not about whether it exists, and the conformance suite is where a
# behaviour is examined. It is listed so that "how many of these are there" has
# an answer; this script checks that such a name is defined, which is the most
# an artefact can say about it.
#
# THE DIRECTIONS ARE BOTH CHECKED ON PURPOSE. A script that only verified the
# `link` rows would pass against a manifest that had quietly moved every row to
# `enosys`, which is the change that would matter most.
set -euo pipefail

manifest="${1:?usage: check-absent.sh <manifest> <archive-or-object>...}"
shift
[ "$#" -gt 0 ] || { echo "no archive or object given" >&2; exit 2; }

NM="${NM:-nm}"

defined="$($NM --defined-only "$@" 2>/dev/null \
  | awk '$2=="T"||$2=="W"||$2=="R"||$2=="D"||$2=="B"||$2=="S"{print $3}' \
  | sed 's/^_//' | sort -u || true)"

# An empty surface is never a conforming one: nothing found means the objects
# were wrong or the symbols were not recognised, and reporting success would
# conceal both. The same rule tools/check-surface.sh states, for the same
# reason.
if [ -z "$defined" ]; then
    echo "no defined symbol was found; the objects or the symbol format are wrong" >&2
    exit 1
fi

status=0
rows=0
in_table=0
while IFS= read -r line; do
    case "$line" in
        '[c-abi.absent]'*) in_table=1; continue ;;
        '['*)              in_table=0; continue ;;
    esac
    [ "$in_table" -eq 1 ] || continue
    case "$line" in ''|'#'*) continue ;; esac
    name="${line%%=*}"; name="${name// /}"
    [ -n "$name" ] || continue
    case "$line" in
        *'form = "link"'*)               want=absent ;;
        *'form = "enosys"'*)             want=present ;;
        *'form = "accepted-no-effect"'*) want=present ;;
        *) echo "row '$name' names no form this script knows" >&2; status=1; continue ;;
    esac
    rows=$((rows+1))
    if grep -qxF -- "$name" <<< "$defined"; then found=present; else found=absent; fi
    if [ "$found" != "$want" ]; then
        if [ "$want" = absent ]; then
            echo "$name is declared absent at the link and IS defined in the archive" >&2
        else
            echo "$name is declared to report its refusal and is NOT defined in the archive" >&2
        fi
        status=1
    fi
done < "$manifest"

if [ "$rows" -eq 0 ]; then
    echo "[c-abi.absent] has no rows; this script asserted nothing" >&2
    exit 1
fi

[ "$status" -eq 0 ] && echo "every [c-abi.absent] row agrees with the archive: $rows row(s)"
exit "$status"
