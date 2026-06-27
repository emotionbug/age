#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_gate="initialization"
remove_log_dir=0

if [[ -n "${ROADMAP_GATES_LOG_DIR:-}" ]]; then
    log_dir=$ROADMAP_GATES_LOG_DIR
    mkdir -p "$log_dir"
else
    log_dir=$(mktemp -d "${TMPDIR:-/tmp}/wcoj-generic-roadmap-gates.XXXXXX")
    remove_log_dir=1
fi

on_error()
{
    local status=$?

    printf 'roadmap gate failed unexpectedly: %s (exit %s)\n' \
           "$current_gate" "$status" >&2
    exit "$status"
}
trap on_error ERR

cleanup()
{
    local status=$?

    if (( remove_log_dir )); then
        if (( status == 0 )); then
            rm -rf "$log_dir"
        else
            printf 'roadmap gate logs retained in %s\n' "$log_dir" >&2
        fi
    fi
}
trap cleanup EXIT

gates=(
    "WCOJ roadmap gates|verify_wcoj_roadmap_gates.sh"
    "WCOJ semiring consumer gates|verify_wcoj_semiring_gates.sh"
    "Generic Join preservation gates|verify_generic_join_preservation.sh"
    "Generic reduction matrix gate|verify_generic_reduction_matrix.sh"
)

printf 'roadmap gates: PG_CONFIG=%s PGDATABASE=%s PGHOST=%s PGPORT=%s\n' \
       "${PG_CONFIG:-<gate default>}" \
       "${PGDATABASE:-agebench}" \
       "${PGHOST:-<libpq default>}" \
       "${PGPORT:-<libpq default>}"
if (( remove_log_dir )); then
    printf 'roadmap gate logs: temporary, retained on failure\n'
else
    printf 'roadmap gate logs: %s\n' "$log_dir"
fi

total=${#gates[@]}
for index in "${!gates[@]}"; do
    IFS='|' read -r label script_name <<< "${gates[$index]}"
    script_path="$script_dir/$script_name"
    log_path="$log_dir/${script_name%.sh}.log"
    current_gate="$label"

    if [[ ! -x "$script_path" ]]; then
        printf 'roadmap gate script is not executable: %s\n' "$script_path" >&2
        exit 1
    fi

    started_at=$(date +%s)
    printf '[%d/%d] %s: start\n' "$((index + 1))" "$total" "$label"
    if "$script_path" >"$log_path" 2>&1; then
        elapsed=$(( $(date +%s) - started_at ))
        summary=$(awk 'NF { line = $0 } END { print line }' "$log_path")
        if [[ -n "$summary" ]]; then
            printf '[%d/%d] %s: ok (%ss) %s\n' \
                   "$((index + 1))" "$total" "$label" "$elapsed" "$summary"
        else
            printf '[%d/%d] %s: ok (%ss)\n' \
                   "$((index + 1))" "$total" "$label" "$elapsed"
        fi
    else
        status=$?
        printf '[%d/%d] %s: failed (exit %s)\n' \
               "$((index + 1))" "$total" "$label" "$status" >&2
        printf 'log: %s\n' "$log_path" >&2
        tail -n 40 "$log_path" >&2 || true
        exit "$status"
    fi
done

current_gate="complete"
printf 'all roadmap gates passed\n'
