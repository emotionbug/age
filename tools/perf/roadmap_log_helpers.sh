#!/usr/bin/env bash

roadmap_print_log()
{
    local log_path=$1
    local fallback_lines=$2
    local print_full=${3:-1}

    if [[ "$print_full" == 1 ]]; then
        cat "$log_path" >&2 || true
    else
        tail -n "$fallback_lines" "$log_path" >&2 || true
    fi
}

roadmap_print_workload_logs()
{
    local log_dir=$1
    local fallback_lines=$2
    local print_full=${3:-1}
    local log_path

    if ! compgen -G "$log_dir/*.log" >/dev/null; then
        printf 'no raw workload logs found in %s\n' "$log_dir" >&2
        return
    fi

    for log_path in "$log_dir"/*.log; do
        printf '\nraw workload log: %s\n' "$log_path" >&2
        roadmap_print_log "$log_path" "$fallback_lines" "$print_full"
    done
}
