# Fixed-MATCH performance verification

`verify_fixed_match.sh` creates a disposable graph with 64 outgoing edges per
source vertex and verifies the optimized fixed-length Cypher path. It checks:

- `WHERE id(source)=constant` selects an indexed edge path;
- anonymous terminal vertices retain correct local-ID semantics;
- terminal label and single-property semantics remain correct;
- an unbound source can be reordered around a one-row terminal property;
- sparse expression-index statistics drive one-row terminal access;
- bounded property postings can drive a local intersection, with a local-ID fallback;
- multiple terminal properties retain the ordinary vertex-scan fallback.

Run against an installed AGE build:

```sh
PGHOST=/tmp PGPORT=5432 PGDATABASE=postgres PGUSER="$USER" \
  PG_CONFIG=/path/to/pg_config tools/perf/verify_fixed_match.sh
```

Set `BENCHMARK_SECONDS=10` to append prepared-statement `pgbench` runs for both
the bound fixed-match path and the unbound sparse-terminal path. The script
intentionally reports latency rather than enforcing a machine-specific
absolute threshold; compare the latter with the pre-series result to verify the
targeted 10x improvement on the deployment hardware.
