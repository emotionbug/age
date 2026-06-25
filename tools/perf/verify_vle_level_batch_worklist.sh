#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/vle-worklist-bench.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

cc=${CC:-cc}
widths=${VLE_WORKLIST_WIDTHS:-"4096 8192 16384"}
minimum_speedup=${VLE_WORKLIST_MIN_SPEEDUP:-10}
binary="$tmpdir/vle-level-batch-worklist-bench"
results="$tmpdir/results.txt"

"$cc" ${CFLAGS:--O2} -std=c11 -Wall -Wextra -Werror \
    "$script_dir/vle_level_batch_worklist_bench.c" -o "$binary"

: > "$results"
for width in $widths; do
    "$binary" "$width" | tee -a "$results"
done

awk -v minimum="$minimum_speedup" '
BEGIN { checked = 0 }
{
    drain = 0
    sustained = 0
    for (i = 1; i <= NF; i++) {
        split($i, field, "=")
        if (field[1] == "drain_speedup")
            drain = field[2] + 0
        else if (field[1] == "sustained_speedup")
            sustained = field[2] + 0
    }
    if (drain < minimum || sustained < minimum) {
        printf "VLE level-batch speedup drain=%.3fx sustained=%.3fx " \
               "is below %.3fx\n", drain, sustained, minimum > "/dev/stderr"
        exit 1
    }
    checked++
}
END {
    if (checked == 0) {
        print "no benchmark rows were produced" > "/dev/stderr"
        exit 1
    }
}
' "$results"
