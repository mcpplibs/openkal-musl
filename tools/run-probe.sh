#!/usr/bin/env bash
#
# BUILD ONE PROBE, RUN IT UNDER A WATCHDOG, AND READ ITS OUTPUT BOTH WAYS.
#
#   run-probe.sh <directory> <program-name> [arguments...]
#
# with the target, where the row needs one, in `MCPP_TARGET`.
#
# ⚠️ WHY THIS IS A SCRIPT AND NOT A STEP. There are four probes now and each
# needs the same three things: a watchdog, because a program that does not
# return is as much a failure as one that returns wrongly and the job would
# otherwise spend its whole timeout finding out; a report of where a program
# that stopped was, because "exit code 139" answers nothing; and BOTH readings
# of the output, because asserting only that the program reported would pass for
# a program that printed its failures.
#
# Four copies of that would be four places for one of them to fall behind. The
# same reasoning is already written into openkal-llvm-runtime/tools.
set -euo pipefail

dir="${1:?the example directory}"
# ⚠️ NO APOSTROPHE IN THIS MESSAGE, AND THAT IS NOT STYLE. Bash parses `${2:?...}`
# with quoting active, so "the program's name" opens a single quote that never
# closes --- and the report arrives thirty lines later as
#
#     tools/run-probe.sh: line 53: syntax error near unexpected token `('
#
# naming a line that is correct. Measured on the first run of this script in
# continuous integration, on all four rows at once: nothing local had run it,
# because the probes were being run by hand as binaries.
name="${2:?the name of the program}"
shift 2

# ⚠️ RESOLVED BEFORE THE `cd' BELOW. `BASH_SOURCE' is the path this script was
# invoked by, which is relative in every caller here, and a relative path stops
# naming this directory the moment the working directory moves.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$dir"

extra=''
[ -n "${MCPP_TARGET:-}" ] && extra="--target $MCPP_TARGET"
# shellcheck disable=SC2086
mcpp build $extra

# ⚠️ THE ARTEFACT OF THE BUILD THAT JUST RAN. tools/one-artifact.sh records what
# a search across an accumulating `target/` answers instead, and what it cost.
binary="$(bash "$here/one-artifact.sh" "$name")"

# Written out rather than taken from `timeout', which two of the three systems
# have and one does not.
watch() {   # watch <seconds> <command>...
    local seconds="$1"; shift
    "$@" & local pid=$!
    ( sleep "$seconds"; kill -9 "$pid" 2> /dev/null ) & local guard=$!
    wait "$pid"; local status=$?
    kill "$guard" 2> /dev/null || true
    return $status
}

if watch 120 sh -c "\"$binary\" $* > run.log 2>&1"; then
    cat run.log
else
    status=$?
    echo "--- what the program printed before it stopped (status $status) ---"
    cat run.log

    # A program that stopped and a program that did not return need different
    # questions asked of them. The debugger is for the first; a stack sample of
    # a program that is still running is for the second, and a debugger asked to
    # run a program that hangs hangs with it.
    if [ "$status" -eq 137 ]; then
        echo "--- it did not return; where it was ---"
        "$binary" "$@" > /dev/null 2>&1 & hung=$!
        sleep 5
        if command -v sample > /dev/null 2>&1; then
            sample "$hung" 3 -mayDie 2>&1 | head -80 || true
        elif command -v eu-stack > /dev/null 2>&1; then
            eu-stack -p "$hung" 2>&1 | head -60 || true
        fi
        kill -9 "$hung" 2> /dev/null || true
    elif command -v lldb > /dev/null 2>&1; then
        watch 90 lldb --batch -o run \
             -k 'thread backtrace all' -k 'register read' -k quit \
             -- "$binary" "$@" > crash.log 2>&1 || true
        cat crash.log
    elif command -v gdb > /dev/null 2>&1; then
        watch 90 gdb -batch -ex run -ex 'bt' --args "$binary" "$@" > crash.log 2>&1 || true
        cat crash.log
    fi
    exit 1
fi

# ⚠️ BOTH DIRECTIONS. That the program reported, and that nothing it observed
# failed to hold. The first alone would pass for a program that printed its
# failures; the second alone would pass for a program that printed nothing.
grep -qE '^-- failures: 0 --$' run.log \
    || { echo "::error::$name did not report a count of failures"; exit 1; }
! grep -q '^FAIL:' run.log \
    || { echo "::error::$name reported a failure"; exit 1; }
echo "  ok  $name: every observation held"
