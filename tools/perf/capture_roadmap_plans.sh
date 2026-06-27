#!/usr/bin/env bash
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)

pg_config=${PG_CONFIG:-/Users/emotionbug/IdeaProjects/postgres_proj/pg18release/bin/pg_config}
database=${PGDATABASE:-agebench}
report_path=${ROADMAP_PLAN_REPORT:-}
log_dir=${ROADMAP_PLAN_LOG_DIR:-}
skip_setup=${ROADMAP_PLAN_SKIP_SETUP:-0}
print_report=${ROADMAP_PLAN_PRINT_REPORT:-1}

runs=${ROADMAP_PLAN_RUNS:-${WCOJ_ROADMAP_RUNS:-5}}
star_sources=${ROADMAP_PLAN_STAR_SOURCES:-${WCOJ_ROADMAP_STAR_SOURCES:-4}}
star_fanout=${ROADMAP_PLAN_STAR_FANOUT:-${WCOJ_ROADMAP_STAR_FANOUT:-4096}}
cycle_vertices=${ROADMAP_PLAN_CYCLE_VERTICES:-${WCOJ_ROADMAP_CYCLE_VERTICES:-1024}}
cycle_fanout=${ROADMAP_PLAN_CYCLE_FANOUT:-${WCOJ_ROADMAP_CYCLE_FANOUT:-64}}
semijoin_fanout=${ROADMAP_PLAN_SEMIJOIN_FANOUT:-${WCOJ_ROADMAP_SEMIJOIN_FANOUT:-128}}
semiring_fanout=${ROADMAP_PLAN_SEMIRING_FANOUT:-${WCOJ_SEMIRING_FANOUT:-500}}
semiring_speedup_fanout=${ROADMAP_PLAN_SEMIRING_SPEEDUP_FANOUT:-${WCOJ_ROADMAP_SEMIRING_SPEEDUP_FANOUT:-80}}
reduction_fanout=${ROADMAP_PLAN_REDUCTION_FANOUT:-${GENERIC_REDUCTION_FANOUT:-1024}}
preserve_cycle_size=${ROADMAP_PLAN_PRESERVE_CYCLE_SIZE:-${GENERIC_JOIN_PRESERVE_CYCLE_SIZE:-128}}

usage()
{
    cat <<'EOF'
usage: tools/perf/capture_roadmap_plans.sh [options]

Run the roadmap benchmark SQL files and save a Markdown artifact whose primary
sections are the complete raw EXPLAIN ANALYZE output for each workload. This
script does not grep for evidence counters or apply speedup thresholds; it
preserves the actual plans for inspection.

Options:
  --log-dir DIR       Directory for raw workload logs.
  --report FILE      Markdown report path. Defaults to LOG_DIR/roadmap-plans.md.
  --skip-setup       Reuse existing benchmark graphs.
  -h, --help         Show this help.

Environment:
  PG_CONFIG, PGDATABASE, PGHOST, PGPORT, PSQL
  ROADMAP_PLAN_LOG_DIR, ROADMAP_PLAN_REPORT, ROADMAP_PLAN_SKIP_SETUP=1
  ROADMAP_PLAN_PRINT_REPORT=0 to keep the full Markdown report out of stdout
  ROADMAP_PLAN_RUNS
  ROADMAP_PLAN_STAR_SOURCES, ROADMAP_PLAN_STAR_FANOUT
  ROADMAP_PLAN_CYCLE_VERTICES, ROADMAP_PLAN_CYCLE_FANOUT
  ROADMAP_PLAN_SEMIJOIN_FANOUT, ROADMAP_PLAN_SEMIRING_FANOUT
  ROADMAP_PLAN_SEMIRING_SPEEDUP_FANOUT
  ROADMAP_PLAN_REDUCTION_FANOUT, ROADMAP_PLAN_PRESERVE_CYCLE_SIZE
  ROADMAP_PLAN_ALLOW_DEBUG_PG=1
EOF
}

while (($#)); do
    case "$1" in
        --log-dir)
            if (($# < 2)); then
                echo "--log-dir requires a directory" >&2
                exit 2
            fi
            log_dir=$2
            shift 2
            ;;
        --report)
            if (($# < 2)); then
                echo "--report requires a file" >&2
                exit 2
            fi
            report_path=$2
            shift 2
            ;;
        --skip-setup)
            skip_setup=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! -x "$pg_config" ]]; then
    echo "PG_CONFIG is not executable: $pg_config" >&2
    exit 1
fi

configure=$("$pg_config" --configure)
if [[ ${ROADMAP_PLAN_ALLOW_DEBUG_PG:-0} != 1 ]] &&
   [[ "$configure" == *"--enable-debug"* ||
      "$configure" == *"--enable-cassert"* ||
      "$configure" == *"-O0"* ]]; then
    echo "refusing to capture roadmap plans against a debug/cassert/O0 PostgreSQL" >&2
    echo "$configure" >&2
    exit 1
fi

if [[ -z "$log_dir" ]]; then
    timestamp=$(date +%Y%m%d-%H%M%S)
    log_dir=$(mktemp -d "${TMPDIR:-/tmp}/roadmap-plans-$timestamp.XXXXXX")
else
    mkdir -p "$log_dir"
fi
if [[ -z "$report_path" ]]; then
    report_path="$log_dir/roadmap-plans.md"
fi

psql=${PSQL:-$("$pg_config" --bindir)/psql}
psql_base=("$psql" --set=ON_ERROR_STOP=1 --dbname="$database")
plan_logs=()
plan_labels=()

"${psql_base[@]}" --tuples-only --no-align --command='SELECT 1' >/dev/null

run_psql_file()
{
    local log_path=$1
    shift

    "${psql_base[@]}" "$@" 2>&1 | tee -a "$log_path"
}

capture_workload()
{
    local label=$1
    local log_name=$2
    local setup_sql=$3
    local benchmark_sql=$4
    shift 4

    local log_path="$log_dir/$log_name"

    plan_labels+=("$label")
    plan_logs+=("$log_path")
    : > "$log_path"
    {
        printf '### %s\n' "$label"
        printf 'started_at=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        printf 'benchmark_sql=%s\n' "$benchmark_sql"
        if [[ "$skip_setup" == 1 ]]; then
            printf 'setup=skipped\n'
        else
            printf 'setup_sql=%s\n' "$setup_sql"
        fi
        printf '\n'
    } >> "$log_path"

    printf '%s: capture start\n' "$label"
    if [[ "$skip_setup" != 1 ]]; then
        run_psql_file "$log_path" "$@" --file="$script_dir/$setup_sql" >/dev/null
    fi
    run_psql_file "$log_path" "$@" --file="$script_dir/$benchmark_sql" >/dev/null
    if ! grep -q 'Execution Time:' "$log_path"; then
        printf '%s: missing raw EXPLAIN ANALYZE output in %s\n' \
               "$label" "$log_path" >&2
        cat "$log_path" >&2
        exit 1
    fi
    printf '%s: capture ok (%s)\n' "$label" "$log_path"
}

capture_workload \
    "WCOJ completion and cycle plans" \
    "wcoj-completion-plans.log" \
    "wcoj_completion_setup.sql" \
    "wcoj_completion_benchmark.sql" \
    --set=runs="$runs" \
    --set=star_sources="$star_sources" \
    --set=star_fanout="$star_fanout" \
    --set=cycle_vertices="$cycle_vertices" \
    --set=cycle_fanout="$cycle_fanout"

capture_workload \
    "Acyclic semijoin plans" \
    "wcoj-semijoin-plans.log" \
    "wcoj_semijoin_setup.sql" \
    "wcoj_semijoin_benchmark.sql" \
    --set=runs="$runs" \
    --set=fanout="$semijoin_fanout"

capture_workload \
    "Semiring consumer plans" \
    "wcoj-semiring-plans.log" \
    "wcoj_semiring_setup.sql" \
    "wcoj_semiring_benchmark.sql" \
    --set=fanout="$semiring_fanout"

capture_workload \
    "Semiring count speedup plans" \
    "wcoj-semiring-speedup-plans.log" \
    "wcoj_semiring_setup.sql" \
    "wcoj_semiring_speedup_benchmark.sql" \
    --set=runs="$runs" \
    --set=fanout="$semiring_speedup_fanout"

capture_workload \
    "Generic Join GHD preservation plans" \
    "generic-join-preservation-plans.log" \
    "generic_join_preservation_setup.sql" \
    "generic_join_preservation_benchmark.sql" \
    --set=cycle_size="$preserve_cycle_size"

capture_workload \
    "Generic Join reduction matrix and lazy physical provider plans" \
    "generic-reduction-matrix-plans.log" \
    "generic_reduction_matrix_setup.sql" \
    "generic_reduction_matrix_benchmark.sql" \
    --set=fanout="$reduction_fanout"

{
    printf '# Roadmap Full Plan Capture\n\n'
    printf -- '- generated_at: `%s`\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf -- '- repository: `%s`\n' "$repo_root"
    printf -- '- git_commit: `%s`\n' "$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || printf unknown)"
    printf -- '- pg_config: `%s`\n' "$pg_config"
    printf -- '- pg_configure: `%s`\n' "$configure"
    printf -- '- database: `%s`\n' "$database"
    printf -- '- log_dir: `%s`\n' "$log_dir"
    printf -- '- skip_setup: `%s`\n\n' "$skip_setup"
    printf '## Raw Plan Artifact Index\n\n'
    printf 'Use these complete logs as the primary evidence. Gate summaries and '
    printf 'counter greps are secondary checks over the same plan text.\n\n'
    printf -- '- WCOJ payload, completion, and cycle disclosure: `%s`\n' \
           "$log_dir/wcoj-completion-plans.log"
    printf -- '- WCOJ semijoin/Yannakakis disclosure: `%s`\n' \
           "$log_dir/wcoj-semijoin-plans.log"
    printf -- '- WCOJ semiring consumer disclosure: `%s`\n' \
           "$log_dir/wcoj-semiring-plans.log"
    printf -- '- WCOJ semiring count speedup disclosure: `%s`\n' \
           "$log_dir/wcoj-semiring-speedup-plans.log"
    printf -- '- Generic Join GHD/count disclosure: `%s`\n' \
           "$log_dir/generic-join-preservation-plans.log"
    printf -- '- Generic reduction matrix/Yannakakis/lazy physical disclosure: `%s`\n\n' \
           "$log_dir/generic-reduction-matrix-plans.log"
    printf '## Parameters\n\n'
    printf -- '- runs: `%s`\n' "$runs"
    printf -- '- star_sources: `%s`\n' "$star_sources"
    printf -- '- star_fanout: `%s`\n' "$star_fanout"
    printf -- '- cycle_vertices: `%s`\n' "$cycle_vertices"
    printf -- '- cycle_fanout: `%s`\n' "$cycle_fanout"
    printf -- '- semijoin_fanout: `%s`\n' "$semijoin_fanout"
    printf -- '- semiring_fanout: `%s`\n' "$semiring_fanout"
    printf -- '- semiring_speedup_fanout: `%s`\n' "$semiring_speedup_fanout"
    printf -- '- reduction_fanout: `%s`\n' "$reduction_fanout"
    printf -- '- preserve_cycle_size: `%s`\n\n' "$preserve_cycle_size"
    printf '## Complete Raw EXPLAIN Plan Sections\n\n'
    printf 'The sections below are copied verbatim from the workload logs. No '
    printf 'grep-style evidence filters or threshold summaries are applied.\n\n'

    for index in "${!plan_logs[@]}"; do
        printf '### Full Raw Plan: %s\n\n' "${plan_labels[$index]}"
        printf -- '- source_log: `%s`\n\n' "${plan_logs[$index]}"
        printf '```text\n'
        cat "${plan_logs[$index]}"
        printf '```\n\n'
    done
} > "$report_path"

printf 'roadmap full plan report: %s\n' "$report_path"
if [[ "$print_report" == 1 ]]; then
    printf '\n'
    cat "$report_path"
fi
