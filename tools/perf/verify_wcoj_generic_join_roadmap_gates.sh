#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
current_gate="initialization"
remove_log_dir=0
capture_plans=${ROADMAP_GATES_CAPTURE_PLANS:-1}
print_plans=${ROADMAP_GATES_PRINT_PLANS:-1}
print_failure_log=${ROADMAP_GATES_PRINT_FAILURE_LOG:-1}

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
            if [[ "$capture_plans" == 1 ]]; then
                printf 'roadmap gate logs retained in %s\n' "$log_dir"
            else
                rm -rf "$log_dir"
            fi
        else
            printf 'roadmap gate logs retained in %s\n' "$log_dir" >&2
        fi
    fi
}
trap cleanup EXIT

print_gate_failure_log()
{
    local log_path=$1
    local fallback_lines=$2

    if [[ "$print_failure_log" == 1 ]]; then
        cat "$log_path" >&2 || true
    else
        tail -n "$fallback_lines" "$log_path" >&2 || true
    fi
}

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
    if [[ "$capture_plans" == 1 ]]; then
        printf 'roadmap gate logs: temporary, retained for full plan capture\n'
    else
        printf 'roadmap gate logs: temporary, retained on failure\n'
    fi
else
    printf 'roadmap gate logs: %s\n' "$log_dir"
fi

total=${#gates[@]}
for index in "${!gates[@]}"; do
    IFS='|' read -r label script_name <<< "${gates[$index]}"
    script_path="$script_dir/$script_name"
    log_path="$log_dir/${script_name%.sh}.log"
    gate_env=()
    current_gate="$label"

    if [[ ! -x "$script_path" ]]; then
        printf 'roadmap gate script is not executable: %s\n' "$script_path" >&2
        exit 1
    fi

    case "$script_name" in
        verify_wcoj_roadmap_gates.sh)
            gate_env=(
                "WCOJ_ROADMAP_COMPLETION_RAW_PLAN_LOG=$log_dir/raw-wcoj-completion-plans.log"
                "WCOJ_ROADMAP_SEMIJOIN_RAW_PLAN_LOG=$log_dir/raw-wcoj-semijoin-plans.log"
            )
            ;;
        verify_wcoj_semiring_gates.sh)
            gate_env=(
                "WCOJ_SEMIRING_RAW_PLAN_LOG=$log_dir/raw-wcoj-semiring-plans.log"
            )
            ;;
        verify_generic_join_preservation.sh)
            gate_env=(
                "GENERIC_JOIN_PRESERVE_RAW_PLAN_LOG=$log_dir/raw-generic-join-preservation-plans.log"
            )
            ;;
        verify_generic_reduction_matrix.sh)
            gate_env=(
                "GENERIC_REDUCTION_RAW_PLAN_LOG=$log_dir/raw-generic-reduction-plans.log"
            )
            ;;
    esac

    started_at=$(date +%s)
    printf '[%d/%d] %s: start\n' "$((index + 1))" "$total" "$label"
    if env "${gate_env[@]}" "$script_path" >"$log_path" 2>&1; then
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
        print_gate_failure_log "$log_path" 40
        exit "$status"
    fi
done

if [[ "$capture_plans" == 1 ]]; then
    capture_script="$script_dir/capture_roadmap_plans.sh"
    capture_log="$log_dir/capture_roadmap_plans.log"
    plan_log_dir="$log_dir/full-plans"
    plan_report="$log_dir/roadmap-full-plans.md"

    current_gate="full plan capture"
    if [[ ! -x "$capture_script" ]]; then
        printf 'roadmap plan capture script is not executable: %s\n' \
               "$capture_script" >&2
        exit 1
    fi

    printf 'full plan capture: start\n'
    if ROADMAP_PLAN_PRINT_REPORT=0 "$capture_script" \
        --log-dir "$plan_log_dir" \
        --report "$plan_report" \
        --skip-setup >"$capture_log" 2>&1; then
        summary=$(awk 'NF { line = $0 } END { print line }' "$capture_log")
        if [[ -n "$summary" ]]; then
            printf 'full plan capture: ok %s\n' "$summary"
        else
            printf 'full plan capture: ok\n'
        fi
        printf 'roadmap full plan report: %s\n' "$plan_report"
        if [[ "$print_plans" == 1 ]]; then
            printf '\n'
            cat "$plan_report"
        fi
    else
        status=$?
        printf 'full plan capture: failed (exit %s)\n' "$status" >&2
        printf 'log: %s\n' "$capture_log" >&2
        print_gate_failure_log "$capture_log" 40
        exit "$status"
    fi
fi

current_gate="complete"
printf 'all roadmap gates passed\n'
