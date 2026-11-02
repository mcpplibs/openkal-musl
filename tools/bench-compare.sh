#!/usr/bin/env bash
# Divides two reports of bench/ line by line and states which lines exceed a
# limit (the design's L1: this library against the system's, at most 1.15
# times as long; page faults at most twice as many).
#
#   bench-compare.sh [--report] <system.txt>[,<more>...] <this.txt>[,<more>...] [<limit> [<fault-limit>]]
#
# L1 IS A REPORT, AND THE GATES ARE L2 AND L3 (mcpp design 2026-11-02 part 3,
# revision of section 11). A primitive is timed in nanoseconds against a library
# whose functions are written for one system in its own assembly; a line over
# its limit by a cycle says less than the front end's workloads above this
# library (openkal-llvm-runtime's bench/, L2) and the engine's (warm-prime, L3),
# which are where the gates are. `--report' states the lines over their limit
# and exits 0; without it the script exits 1 when one is, as before.
#
# Each side may be several reports, separated by commas, taken alternately on
# one machine: the smallest value of each line is used, since what disturbs a
# timing --- another program on a shared machine --- only lengthens it.
#
# Exits 1 when a line exceeds its limit and names it. A line one report has
# and the other lacks is listed and not compared.
set -euo pipefail
report=0
if [ "${1:-}" = "--report" ]; then report=1; shift; fi
base="${1:?the reports of the system C library}"
this="${2:?the reports of this library}"
limit="${3:-1.15}"
flimit="${4:-2.0}"
merge() {   # the smallest value of each line across the reports
    local files
    IFS=, read -r -a files <<< "$1"
    awk '$1 == "bench" { if (!($2 in v) || $3 + 0 < v[$2] + 0) v[$2] = $3; u[$2] = $4 }
         END { for (k in v) print "bench", k, v[k], u[k] }' "${files[@]}"
}
awk -v limit="$limit" -v flimit="$flimit" -v report="$report" '
    FNR == NR && $1 == "bench" { b[$2] = $3; unit[$2] = $4; next }
    $1 == "bench" { t[$2] = $3 }
    END {
        bad = 0
        printf "%-22s %12s %12s %8s\n", "measurement", "system", "this", "ratio"
        for (k in b) order[++n] = k
        for (i = 1; i <= n; i++) {
            k = order[i]
            if (!(k in t)) { printf "%-22s %12s %12s %8s\n", k, b[k], "-", "absent"; continue }
            r = (b[k] > 0) ? t[k] / b[k] : 0
            lim = (unit[k] == "faults") ? flimit : limit
            mark = (r > lim) ? "  over " lim : ""
            if (r > lim) bad++
            printf "%-22s %12s %12s %8.2f%s\n", k, b[k], t[k], r, mark
        }
        printf "%d of %d measurements over their limit%s\n", bad, n, report ? " (reported, not a gate)" : ""
        exit (bad && !report) ? 1 : 0
    }' <(merge "$base") <(merge "$this")
