#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/wcoj-progressive-bench.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

cc=${CC:-cc}
sources=${WCOJ_PROGRESSIVE_SOURCES:-"1024 4096 8192"}
fanout=${WCOJ_PROGRESSIVE_FANOUT:-32}
iterations=${WCOJ_PROGRESSIVE_ITERATIONS:-10}
minimum_speedup=${WCOJ_PROGRESSIVE_MIN_SPEEDUP:-10}
maximum_dense_overhead=${WCOJ_PROGRESSIVE_MAX_DENSE_OVERHEAD:-1.25}
binary="$tmpdir/wcoj-progressive-intersection-bench"
results="$tmpdir/results.txt"

"$cc" ${CFLAGS:--O2} -std=c11 -Wall -Wextra -Werror \
    "$script_dir/wcoj_progressive_intersection_bench.c" -o "$binary"

: > "$results"
for source_count in $sources; do
    "$binary" "$source_count" "$fanout" "$iterations" | tee -a "$results"
done

awk -v minimum="$minimum_speedup" -v maximum_dense="$maximum_dense_overhead" '
BEGIN { checked = 0 }
{
    speedup = 0
    dense_overhead = 0
    for (i = 1; i <= NF; i++) {
        split($i, field, "=")
        if (field[1] == "progressive_speedup")
            speedup = field[2] + 0
        else if (field[1] == "dense_overhead")
            dense_overhead = field[2] + 0
    }
    if (speedup < minimum) {
        printf "WCOJ progressive speedup %.3fx is below %.3fx\n", \
               speedup, minimum > "/dev/stderr"
        exit 1
    }
    if (dense_overhead > maximum_dense) {
        printf "WCOJ dense fallback overhead %.3fx exceeds %.3fx\n", \
               dense_overhead, maximum_dense > "/dev/stderr"
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
