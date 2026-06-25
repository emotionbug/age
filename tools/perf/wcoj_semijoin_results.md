# Acyclic Generic Join semijoin benchmark evidence

Measured on 2026-06-25 against PostgreSQL 18.4 with `COPT=-Werror`.  JIT and
parallel query were disabled.  Both cases used the same graph, session, warm-up
policy, enabled PostgreSQL join methods, and fixed written join order.  The
only path switch was `age.enable_wcoj`.

The fixture is a three-edge chain with fanout 128.  `F2` reaches C keys 1..128
and `F3` starts at C keys 128..255, leaving one common separator key.  A
left-deep binary plan therefore performs repeated adjacency probes before the
last relation rejects nearly all candidates.  Query-wide semijoin reduction
intersects endpoint domains first and compacts every provider before variable
DFS.

```sh
psql --set=ON_ERROR_STOP=1 --set=fanout=128 \
  --file=tools/perf/wcoj_semijoin_setup.sql
psql --set=ON_ERROR_STOP=1 --set=runs=5 \
  --file=tools/perf/wcoj_semijoin_benchmark.sql
```

The first execution was discarded.  The median of the next five was reported.

| Plan | Measured samples after warm-up | Median |
|---|---|---:|
| forced binary | 102.760, 100.132, 96.709, 101.653, 99.490 ms | 100.132 ms |
| auto Generic Join | 0.585, 0.553, 0.721, 0.683, 0.553 ms | 0.585 ms |

```text
speedup = 100.132 / 0.585 = 171.17x
```

Representative Generic Join telemetry:

```text
Provider Rows Materialized: 769
Semijoin Reduction Passes: 2
Semijoin Rows Removed: 508
Complete Bindings: 128
Candidate Bag Combinations: 128
Rows Emitted: 128
Peak Generic Join Memory: 169048 bytes
```

The regression fixture projects all four vertex IDs and all three edge IDs.
Forced binary and Generic Join each return 128 rows, and the bidirectional
`EXCEPT ALL` difference is zero.

This is an adversarial late-rejection chain intended to measure intermediate
reduction.  It does not imply a 171x improvement for input-bound chains whose
binary plan already avoids a large intermediate.
