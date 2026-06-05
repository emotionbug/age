# VLE Follow-up History

## 기록 원칙

- 완료된 변경과 검증 결과를 날짜별로 짧게 남긴다.
- 상세 설계 판단은 `RESEARCH.md`, VLE 구조와 benchmark는 `VLE.md`에 둔다.
- 되돌린 시도는 이유와 다음 방향만 남긴다.

## 2026-06-04: VLE source policy reason 노출

- `VLE Edge Source` policy text에 방향별 선택 사유를 `reason=out:.../in:...` 형태로 추가했다.
- endpoint-btree 유지 사유는 `endpoint-work`, `age_adjacency` 전환 사유는 `work-exceeds-limit`로 드러나며,
  source availability fallback은 `endpoint-only`, `unknown-fanout`, `no-source`, `layout`으로 구분된다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: Move aggregate lower target planning

- typed collect와 `array_agg` property narrow path의 aggregate child required target planning을
  `cypher_property_paths.c`로 이동했다.
- group expr 보존, typed collect arg plan, array_agg slot-vector arg plan을 property descriptor module이
  만들고, `cypher_paths.c`는 path copy/projection/AggPath 조립만 맡는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move property aggregate rewrite

- `count(n.prop)` non-null rewrite와 aggregate expression walker를 `cypher_property_paths.c`로 이동했다.
- `count(any)`와 `agtype_object_field_exists_nonnull` OID cache도 property path invalidation에 포함해,
  collect/numeric/array_agg/count property aggregate rewrite가 같은 module API를 통과한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move property projection target planning

- simple property projection detection과 ordered property projection lower/final `PathTarget` construction을
  `cypher_property_paths.c`로 이동했다.
- `cypher_paths.c`는 CustomPath 추가와 deferred projection path 비용/삽입만 맡고, property source/key/type
  판정과 ctid/id refetch final expression 생성은 property descriptor module이 소유한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move property index rewrite planning

- property index restriction rewrite, index predicate/restriction canonicalization, index handoff expression
  reconstruction을 `cypher_property_paths.c`로 이동했다.
- `cypher_paths.c`는 hook orchestration과 `create_index_paths()` 재호출만 맡고, cached-property slot
  expression 선택과 property comparison rewrite는 property descriptor module이 소유한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move scalar final target planning

- scalar-to-agtype final output의 lower/final `PathTarget` construction을 `cypher_property_paths.c`로 이동했다.
- count/agtype deferred projection path는 더 이상 cached-property slot expression을 직접 만들지 않고,
  property descriptor module이 scalar final handoff, lower computed expression, final materializer rewrite를 함께 만든다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move typed collect arg planning

- `CypherTypedCollectArgPlan`과 typed collect lower argument construction, lower target insertion,
  aggregate target rewrite를 `cypher_property_paths.c`로 이동했다.
- `cypher_paths.c`의 aggregate path rewrite는 group expr 보존, child target 교체, aggregate path 조립만 맡고,
  cached-property slot expression 선택과 typed aggregate argument rewrite는 property descriptor module이 맡는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Move array aggregate property handoff

- `array_agg` property/map/list aggregate handoff detection, argument descriptor construction, aggregate target
  rewrite를 `cypher_paths.c`에서 `cypher_property_paths.c`로 이동했다.
- slots aggregate OID cache와 array const key-path parsing도 property path module이 소유하게 해,
  upper path hook은 lower target/aggregate path 조립만 맡도록 좁혔다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: VLE packed fallback policy skip

- fixed-source가 local edge-state, edge label, no-property constraint 조건에서 traversal 방향 전체를 처리하면
  packed adjacency fallback source를 만들지 않게 했다. 방향 source가 실제로 존재하는 경우에만 skip하므로
  unconstrained/global VLE와 property-constrained VLE는 기존 packed fallback을 유지한다.
- `VLE Source Runtime`의 `packed` counter를 `scans/candidates/empty-skips/policy-skips` 형태로 확장했다.
  small fan-out smoke에서는 `packed=0/0/0/9`처럼 covered packed setup이 runtime evidence에 직접 드러난다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `tools/vle_benchmark.sql` small smoke에서 policy skip 출력 확인.

## 2026-06-04: 800-label VLE fan-out benchmark harness

- `tools/vle_benchmark.sql`에 800-label fan-out workload를 추가했다. 기본 profile은 800개 unrelated
  `NoiseEdge_*` label과 `FanoutStart` -> `FanoutEdge` -> `FanoutTarget` constrained terminal-property VLE를 만든다.
- benchmark output은 `800-label-fanout-terminal` row count/timing과 PostgreSQL
  `EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)`에서 뽑은 `AGE VLE Stream`,
  `VLE Edge Source`, `VLE Source Runtime` lines를 함께 보여준다.
- label 생성과 cleanup은 `\gexec` autocommit 단위로 분리했다. 800 label을 단일 `DO`/`drop_graph` transaction에서
  처리하면 `max_locks_per_transaction`에 걸리기 때문이다.
- 기존 broad benchmark cases는 `run_standard_cases=1`일 때만 실행한다. 기본은 TODO 기준 workload인
  800-label targeted fan-out case다.
- 검증:
  - 작은 smoke: `label_fanout_labels=8`, `label_fanout_edges=8` temp PostgreSQL instance에서 통과.
  - 800-label profile: `label_fanout_labels=800`, `label_fanout_edges=64` temp PostgreSQL instance에서 통과.
    결과는 `rows_returned=64`, `fixed-source=out=age-adjacency/in=endpoint-btree`,
    `endpoint-work=sum(out:4160/6,in:2/6)`, `VLE Source Runtime` dominant `age-adjacency`.

## 2026-06-04: VLE runtime source density feedback

- `VLE Source Runtime` feedback에 source별 scan당 candidate density를 추가했다.
- output 예시는 `density=age_adjacency:0.89,endpoint-btree:0.00,packed:0.00` 형태다. 800-label fan-out
  benchmark처럼 source scan 수와 candidate 수가 모두 필요한 threshold 판단을 plan output에서 바로 읽기 위한
  근거다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `tools/vle_benchmark.sql` small smoke에서 density 출력 확인.

## 2026-06-04: VLE source policy handoff 적용

- `AGE VLE Stream` edge-source descriptor의 directed policy enum을 `AgeVLEInput`으로 전달하고,
  setup/apply/root source layout descriptor가 이 recommendation을 effective source로 읽게 했다.
- planner descriptor의 `fixed-source`도 executor가 실제로 쓰는 source와 맞추도록 policy 적용 후 값으로
  출력한다. `*1..2` fixture는 `age_adjacency` 대신 endpoint-btree 3회 scan/6 candidate를 사용하면서도
  result 6 rows를 유지한다.
- selector override는 후보 index가 있는 방향에만 적용해 descriptor 값이 source layout을 `none`으로 오염시키지
  않게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE source feedback refresh 누적

- cached `VLE_local_context` refresh가 initial setup에서 받은 source policy를 잃지 않도록 context에
  policy descriptor를 보관하고 refresh source layout input이 같은 recommendation을 읽게 했다.
- `AGE VLE Stream` executor의 `VLE Source Runtime` counter를 iterator 마지막 snapshot이 아니라 CustomScan
  execution 전체 누적값으로 바꿨다. nested-loop 5회 실행 fixture는 `endpoint-btree=5/6`을 출력한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE source policy work model 노출

- endpoint-btree source policy 설명을 `fanout-threshold=2`에서 depth-aware `endpoint-work=current/limit`
  model로 바꿨다.
- low fanout fixture는 `endpoint-work=out:4/4`로 endpoint-btree를 유지하고, fanout 3/depth 2 fixture는
  `endpoint-work=out:9/4`로 `out=age-adjacency`를 선택하는 visible regression을 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: cached-property projection slot reuse

- `AGE Property Projection` executor가 같은 key path를 여러 physical result type으로 출력할 때 properties path lookup을
  slot마다 반복하지 않고 첫 slot의 raw `agtype_value`를 재사용하게 했다.
- verbose explain의 `Cached Property Slots`는 중복 slot을 `source=slot-N`으로 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: Format VLE runtime source evidence

- `VLE Source Runtime` formatting을 executor에서 `age_vle_source_cost`로 옮겼다.
- runtime explain에 dominant source, dominant yield, payload replay, push/yield feedback을 추가해
  source cost module이 runtime counter evidence를 해석하게 했다.
- 검증:
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 통과: `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 통과: `git diff --check`

## 2026-06-04: Move VLE source cost evidence

- endpoint fanout/reltuples estimate를 `age_vle_source_cost` module로 분리했다.
- optimizer adjacency custom path costing과 runtime VLE local edge-state capacity estimate가 같은
  source cost evidence API를 사용한다.
- 검증:
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 통과: `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 통과: `git diff --check`

## 2026-06-04: Share VLE source selector

- `select_vle_traversal_source_layout()`을 추가해 planner `AGE VLE Stream` edge-source descriptor와
  runtime root layout이 endpoint-btree/`age_adjacency` source 선택을 공유하게 했다.
- runtime root builder는 shared selector의 source kind에 index OID만 붙이고, planner는 같은 decision을
  `AgeVLEStreamDirectedSourceKind`로 변환한다.
- 검증:
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 통과: `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 통과: `git diff --check`

## 2026-06-04: Move VLE terminal output boundary

- terminal scalar/full property output, direct property result cache, block prefetch, batch
  materialization decision을 `age_vle_terminal_output` module로 분리했다.
- `age_vle.c`의 DFS/search loop는 `VLETerminalOutputPolicy`와 `VLETraversalStep`을 넘기고,
  terminal output module이 final result target write와 terminal property cache/prefetch를 처리한다.
- 검증:
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 통과: `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 통과: `git diff --check`

## 2026-06-04: Move VLE iterator output dispatch

- materialization kind별 final output dispatch를 `age_vle_iterator_emit_result()`로 낮췄다.
- `age_vle.c`는 terminal property/full-properties/container builder callback을 제공하고, iterator
  materialization module이 output target write와 kind dispatch를 맡는다.
- 검증:
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - 통과: `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 통과: `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 통과: `git diff --check`

## 2026-06-04: Move VLE iterator output target

- iterator output의 `Datum *result`/`bool *is_null` pair를 `VLEIteratorOutputTarget` descriptor로 묶었다.
- terminal scalar property, terminal full properties, terminal-property batch, path/container output이 같은
  target setter를 사용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE source lifecycle ownership

- `VLEContextAgeAdjacencyPayloadSource`를 opaque context source로 바꾸고 begin/next/end lifecycle을 사용하게
  했다.
- packed adjacency source도 context-owned begin/next/end lifecycle로 바꿔 candidate source가 packed list,
  suppression flag, iterator index를 직접 열지 않게 했다.
- candidate source는 `age_adjacency` payload 또는 packed adjacency entry를 받아 validation과 traversal
  handoff만 수행한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE traversal step payload

- DFS consume 결과를 `VLETraversalStep` descriptor로 묶었다.
- terminal acceptance, upper-bound skip, expansion, terminal output cache가 `next_vertex_id`,
  `next_vertex_entry`, `path_length` out-param 묶음 대신 같은 step handoff를 읽게 했다.
- context consume API가 traversal consume과 consumed vertex entry cache update를 함께 처리한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE candidate match handoff

- `VLECandidateMatchResult`를 context descriptor로 이동하고, match result 적용을
  `age_vle_context_apply_candidate_match_result()`로 낮췄다.
- source module은 candidate와 match용 edge fetch result를 만들고, traversal needs-check/property-match/match-bit
  policy는 context/traversal boundary가 처리한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE candidate match result

- source별 candidate construction이 `VLECandidateMatchResult`를 만들고, 공통 helper가 traversal needs-check,
  edge property semantic match, match bit mark를 적용하게 했다.
- packed adjacency와 `age_adjacency` path는 property constraint가 있을 때만 match용 edge를 result에 싣고,
  endpoint-btree path는 constraint 없는 result로 같은 traversal handoff를 탄다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE missing expansion run

- missing vertex fallback도 `VLEContextExpansionSourceRun`을 사용하게 했다.
- fallback eligibility와 incoming self-loop skip policy를 context run descriptor가 계산한다.
- missing/normal expansion source cursor 초기화가 같은 helper를 사용하고, source module은 run 결과로
  missing-hit counter만 기록한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE expansion source run

- normal vertex expansion의 outgoing/incoming source 결과를 `VLEContextExpansionSourceRun`에 기록하게 했다.
- incoming source self-loop skip과 packed fallback suppression은 run descriptor를 통해 계산한다.
- `age_vle_candidate_source.c`의 `used_out_source`, `used_in_source`, `used_index_source` local 조합을 제거하고,
  source-kind dispatch는 공통 cursor dispatch helper로 공유한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE packed source descriptor

- packed fallback source plan을 `VLEContextPackedAdjacencySource` context descriptor로 이동했다.
- context API가 direction/label constrained packed list selection, out/in/self suppression, empty-source 판단을
  함께 수행한다.
- `age_vle_candidate_source.c`는 packed source list mutation을 하지 않고 실행 가능한 packed list descriptor만
  순회한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE source scan lifecycle

- `age_adjacency` payload replay/fresh scan/cache seed/discard lifecycle을 `VLEContextAgeAdjacencyPayloadSource`
  context API로 이동했다.
- endpoint-btree relation/index/slot/index-scan lifecycle과 tuple field extraction을 opaque
  `VLEContextEndpointIndexSource` API로 이동했다.
- `age_vle_candidate_source.c`는 payload 또는 endpoint tuple을 받아 candidate validation과 traversal handoff만
  수행한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE candidate validation descriptors

- candidate validation의 graph lookup/carry policy를 `VLECandidateGraphAccess`로 묶었다.
- endpoint-btree와 `age_adjacency`의 source vertex, direction, self-loop 검증을 `VLECandidateSourceIdentity`로
  공유하게 했다.
- source별 validation은 graph context와 source identity raw field를 직접 반복하지 않고 공통 descriptor helper를
  통해 vertex/edge lookup과 endpoint validation을 수행한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE candidate traversal handoff

- candidate source의 local edge index 생성, match-check/mark, candidate push를 `age_vle_context.c` API로 이동했다.
- `age_vle_candidate_source.c`는 더 이상 `VLETraversalState *`를 저장하지 않고 `VLE_local_context` handoff만 받는다.
- source module은 source별 candidate construction/validation을 맡고, traversal state layout과 push policy는
  context/traversal boundary 안에 둔다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE source stats lifecycle

- VLE source runtime stats reset/snapshot/counter update를 `age_vle_context.c` API로 이동했다.
- `age_vle_candidate_source.c`는 `VLEContextSourceStatsKind`와 record helper를 사용하고,
  `AgeVLESourceStats` layout을 직접 갱신하지 않게 했다.
- cached context reuse의 stats reset과 iterator stats snapshot도 context API를 사용한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE context cleanup lifecycle

- VLE runtime cleanup과 full local context cleanup을 `age_vle_context.c` API로 이동했다.
- `age_vle.c`는 cleanup callback 해제와 `VLE_local_context` 객체 해제만 수행하고, adjacency scan,
  payload cache, property relation cache, terminal prefetch/batch state, graph/label identity, terminal key,
  edge constraint cache, traversal state cleanup은 context lifecycle API가 맡게 했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE edge property context

- VLE edge property constraint cache lifecycle을 `age_vle.c` callback에서 `age_vle_context.c` base apply로
  옮겼다.
- `VLETraversalApplyOps`에서 `cache_edge_property_constraints` callback을 제거해 apply module이 constraint
  cache callback을 요구하지 않게 했다.
- `VLEEdgePropertyMatchContext` type과 relation-cache handoff를 context API로 올리고,
  `age_vle_candidate_source.c`는 raw constraint field 대신 `age_vle_context_init_edge_property_match()`를
  사용하게 했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Split VLE traversal load policy

- `VLETraversalLoadPolicy`를 추가해 edge property metadata, edge metadata, vertex metadata load decision을
  `VLETraversalSetup` 내부 descriptor로 분리했다.
- `init_vle_traversal_setup()`은 graph/edge/index/endpoint/range/direction descriptor를 먼저 채우고,
  load policy helper가 metadata load 여부를 계산한다.
- `apply_vle_traversal_setup()`을 추가해 새 local context에 setup descriptor를 적용하는 graph/global
  context, endpoint/path function, edge constraint/index, range/direction, reverse-root 선택을 한 경계로
  묶었다.
- cached context refresh의 start/end endpoint handoff를 `VLEContextRefreshInput`으로 묶고,
  `apply_cached_vle_context_refresh()`가 traversal root 갱신과 terminal-property scratch reset을 처리하게
  했다.
- `VLETraversalRootDescriptor`를 추가해 새 context setup과 cached refresh가 path function, start/end
  root, direction, next vertex cursor, reverse flags를 같은 root apply helper로 적용하게 했다.
- `VLETraversalSourceLayout`을 추가해 age_adjacency out/in index, endpoint btree start/end index,
  direction, local edge-state, label/property constraint를 묶고, skeletal endpoint fallback과 normal
  vertex expansion이 같은 source selection helper를 사용하게 했다.
- source layout은 `VLE_local_context`에 저장하고 root apply와 cached index refresh 시점에 갱신해,
  expansion 시점에는 확정된 traversal source contract만 읽도록 했다.
- `VLETraversalSourceKind`와 `VLETraversalDirectedSource`를 추가해 `VLETraversalSourceLayout`이
  outgoing/incoming source kind와 index OID를 직접 보관하게 했다. age_adjacency 우선, endpoint-btree fallback
  선택은 root apply/index refresh 시점에 고정되고, expansion helper는 fixed source kind만 dispatch한다.
- `AGE VLE Stream` edge-source descriptor와 verbose explain에 outgoing/incoming fixed source kind를 추가했다.
  candidate age_adjacency/endpoint-btree index 존재 여부는 유지하되, 실제 dispatch source는 별도
  `fixed-source=out=.../in=...`로 출력해 visible plan regression이 runtime source contract를 보여주게 했다.
- `VLETraversalRootDescriptor`가 `VLETraversalSourceLayout`을 함께 보관하게 바꿨다. 새 context setup과 cached
  refresh는 root direction/root swap이 끝난 뒤 같은 source layout builder를 호출하고, root apply는 context의
  current layout을 복사한다. cached index refresh는 같은 builder로 current layout만 갱신한다.
- `VLETraversalSourceIndexes`를 추가해 age_adjacency out/in index와 endpoint start/end btree index OID를
  하나의 handoff로 묶었다. setup, cached index refresh, load policy, source layout builder는 같은 descriptor를
  읽고, context의 raw OID 필드 나열을 제거했다.
- `age_vle_container.c`와 `age_vle_container.h`를 추가해 `VLE_path_container` layout, `VLEContainerBuildInput`,
  path/reversed/zero/terminal container builder dispatch를 age_vle 본체에서 분리했다. `age_vle.c`는 traversal
  state에서 build input을 채우고 iterator emission에서 container module을 호출한다.
- `VLETraversalSourcePolicy`를 추가해 source index handoff와 metadata load decision을 같은 descriptor에 묶었다.
  targeted local-index 가능 여부는 source policy 안의 index availability를 보고 계산하고, setup/apply는
  source indexes와 load booleans를 별도 raw field 묶음으로 재조합하지 않는다.
- `VLETraversalGraphLoad`를 추가해 graph name/oid, edge label oid, source policy를 하나의 graph-load
  descriptor로 묶었다. global graph context load는 descriptor helper를 통해 수행되어 setup의 graph/source/load
  field가 긴 argument list로 다시 흩어지지 않는다.
- `VLETraversalApplyInput`을 추가해 setup descriptor, loaded graph context, cache flag, grammar node id를
  local context apply handoff로 묶었다. `apply_vle_traversal_setup()`은 context 외에는 이 descriptor 하나만
  받는다.
- `VLETraversalShape`를 추가해 endpoint validity, initial root graphid, range bound, direction을 하나의
  traversal semantic descriptor로 묶었다. metadata load policy, root descriptor, local context apply는
  setup의 raw field 나열 대신 이 shape를 공유한다.
- `age_vle_setup.c`와 `age_vle_setup.h`를 추가해 `VLETraversalSetup` 생성, graph/label lookup cache,
  adjacency/endpoint index discovery, graph-load helper를 `age_vle.c`에서 분리했다. 새 context apply와 cached
  root refresh는 아직 `VLE_local_context` layout을 직접 쓰므로 다음 구조 변경 대상으로 남겼다.
- `VLETraversalContextApply`를 추가해 새 context base apply의 graph/cache/edge constraint/source index/range
  mutation을 하나의 descriptor로 묶었다. root descriptor 생성은 base apply 뒤 fan-out/source layout을 계산하는
  단계로 남겨 다음 root/apply module 분리의 입력 표면을 줄였다.
- `VLETraversalRefreshApply`를 추가해 cached refresh의 root 갱신, source layout 갱신, terminal-property
  direct-result scratch reset을 하나의 refresh apply handoff로 묶었다.
- `VLETraversalSourceLayoutInput`을 추가해 source layout builder가 `VLE_local_context` 전체 대신 source index,
  local edge-state, label/property constraint descriptor만 받게 했다.
- `VLETraversalRootSelectionInput`을 추가해 start/end root fan-out 비교가 graph context, edge label oid,
  empty/zero range descriptor만 읽게 했다.
- `age_vle_root.c`와 `age_vle_root.h`를 추가해 root descriptor 생성, cached refresh root 갱신, source layout
  builder, initial fan-out count를 `age_vle.c`에서 분리했다.
- `VLETraversalRootApplyInput`을 추가해 root selection input, source layout input, current root descriptor를
  하나의 root apply handoff로 묶었다.
- `VLETraversalSetupApply`를 확장해 새 context base apply, root descriptor, terminal output policy,
  edge-state init을 하나의 setup apply handoff로 묶고, root descriptor와 edge-state capacity를 context
  mutation 전 descriptor에서 계산하게 했다.
- `VLETraversalActivationApply`와 `VLETraversalCachedReuseApply`를 추가해 새 context initial stack load,
  cached refresh 후 source index refresh/reload, next start-vertex reload를 같은 activation handoff로 묶었다.
- `age_vle_context.h`를 추가해 `VLE_local_context` layout과 setup/apply/refresh/activation descriptor type의
  ownership을 `age_vle.c` 밖으로 옮겼다.
- `age_vle_apply.c`와 `age_vle_apply.h`를 추가해 context/setup/refresh/activation apply와 source layout refresh
  구현을 `age_vle.c`에서 분리하고, graph-runtime side effect는 `VLETraversalApplyOps` callback으로 남겼다.
- `age_vle_context.c`를 추가해 context base/root/output/edge-state setter, terminal scratch reset, source/root
  input builder를 분리하고 `age_vle_apply.c`의 `VLE_local_context` raw field 접근을 제거했다.
- `VLEContextOutputState`를 추가해 output requirement, terminal emit policy, terminal property key/direct-result
  scratch, prefetch block state, terminal-property batch state를 하나의 output substate로 묶었다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: Split VLE iterator materialization selection

- `VLEIteratorMaterialization` kind/descriptor와 terminal-only output requirement 판정을
  `age_vle_iterator_materialization.c` 내부 API로 분리했다.
- `age_vle.c` iterator loop는 output requirement, terminal property emit 여부, zero-bound 여부를
  materialization descriptor로 낮춘 뒤 path/terminal output emit만 수행한다.
- traversal frame carry policy와 iterator materialization 선택이 같은 terminal-only semantic helper를
  공유하게 했다.
- `VLEIteratorContainerKind`를 descriptor에 추가해 path/reversed path/zero path/terminal vertex
  container 선택도 descriptor 모듈이 결정하게 했다. `age_vle.c`는 선택된 kind에 맞는 builder 실행만
  담당한다.
- `VLEContainerBuildInput`을 추가해 container builder들이 `VLE_local_context` 전체 대신 graph oid,
  start vertex id, reverse-output flag, path stack, path vertex stack만 받도록 좁혔다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: Trim VLE traversal frame payload

- `VLETraversalFrame`에서 반복 저장하던 `source_vertex_id`를 제거하고, source/root metadata는 traversal
  root/consume state contract로 유지했다.
- local edge-state traversal의 `vertex_entry` frame carry 여부를 output requirement로 정했다.
  terminal vertex/property/properties output은 indexed property helper와 terminal emit handoff 때문에
  entry를 보존하고, path/container output은 frame payload를 줄일 수 있게 했다.
- 첫 시도에서 `terminal-vertex` output의 indexed property helper가 `vertex_entry`를 기대해 assertion이
  발생했으나, guard를 추가하지 않고 terminal-only output policy로 frame handoff contract를 정리해
  focused regression을 통과시켰다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: Pass VLE output requirements through stream descriptors

- `AgeVLEOutputRequirement`를 추가해 `AGE VLE Stream` CustomScan output descriptor가
  path/terminal-vertex/terminal-property requirement를 명시적으로 보관하게 했다.
- executor는 requirement를 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 `nargs`와 grammar id
  재추론 대신 planner-derived requirement로 emit mode를 정한다.
- verbose `EXPLAIN`의 `VLE Output`에 `requirement=...`를 출력하고, terminal vertex consumer를
  `VLE Shape: terminal-vertex`와 `terminal-vertex(requirement=terminal-vertex, ...)` surface로 path와
  구분해 plan evidence에 남겼다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: Share VLE indexed property lookup

- `VLEVertexPropertyLookup`을 node/edge 공통 `VLEIndexedPropertyLookup`으로 넓혔다.
- `age_vle_node_property_at()`, `age_vle_edge_property_at()`,
  `age_vle_terminal_vertex_property_from_path()`가 같은 key descriptor와 relation cache handoff를 사용한다.
- edge entry에도 scalar property cache와 relation-based property fetch API를 추가해 indexed edge property가
  full properties object materialization을 반복하지 않게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Share VLE container index contract

- `VLE_path_container` edge/node count와 negative index normalization을
  `get_vle_container_edge_count()`, `get_vle_container_node_count()`,
  `normalize_vle_container_index()`로 통합했다.
- single edge/node materializer, node/edge id/label/properties direct helper, edge endpoint/property helper가
  같은 container count/index contract를 사용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Expose VLE materializer plan evidence

- `cypher_vle`의 VLE index probe와 compact materializer/output requirement plan 검증을 hidden `DO`
  assertion에서 visible `EXPLAIN (VERBOSE, COSTS OFF)` expected 출력으로 바꿨다.
- expected plan이 `age_vle_path_length`, list slice/count helper, tail-last id/endpoint helper,
  single edge/node materializer, endpoint join presence/absence를 직접 보여준다.
- `cypher_vle.sql` 안의 hidden plan assertion block을 제거했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Expose VLE terminal key descriptor metadata

- `AGE VLE Stream` output descriptor가 terminal-property key length와 char-fast metadata를 보관하도록
  확장했다.
- executor는 descriptor metadata를 `AgeVLEInput`에 싣고, known terminal key slot에서는
  `build_local_vle_context()`가 key length를 다시 계산하지 않고 planner-derived char-fast metadata를
  사용한다.
- verbose `EXPLAIN`의 `VLE Output`에 `len`과 `char-fast`를 출력해 terminal-property stream의 key/cache
  contract를 visible regression evidence로 남긴다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Share VLE terminal property key descriptors

- `VLEPropertyKeyDescriptor`를 추가해 terminal-property stream과 indexed property helper가 같은
  key/char-fast metadata contract를 보게 했다.
- `VLETerminalPropertyLookup`이 graph context, relation cache, block prefetch set, prefetch budget을
  함께 들고, `VLETerminalOutputPolicy`가 이 lookup을 소유하도록 바꿨다.
- direct terminal output, final property emit, terminal batch fetch가 `VLE_local_context`의 개별 필드를
  다시 읽지 않고 lookup descriptor를 통해 scalar cache/relation cache handoff를 공유한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Share VLE vertex property lookup descriptor

- `VLEVertexPropertyLookup`을 추가해 `VLE_path_container` candidate vertex metadata와 runtime
  property key를 하나의 lookup descriptor로 묶었다.
- `age_vle_node_property_at()`과 `age_vle_terminal_vertex_property_from_path()`가 같은 helper를 사용해
  candidate scalar-cache hit와 relation-cache fallback contract를 공유한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Expose VLE indexed property plans

- fixed path indexed node/edge property plan 검증을 DO block hidden assertion에서
  visible `EXPLAIN (VERBOSE, COSTS OFF)` expected 출력으로 옮겼다.
- expected output이 `age_vle_node_property_at`, `age_vle_edge_property_at`, `AGE VLE Stream`
  descriptor를 직접 보여준다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Reuse VLE vertex scalar cache for indexed property

- `age_vle_node_property_at()`이 full properties object를 먼저 materialize하지 않고
  `get_vertex_entry_property_with_cache()`를 사용하도록 바꿨다.
- indexed node property helper와 terminal-property direct output이 같은 vertex-entry scalar cache를
  우선 확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Gate VLE candidate seed on hydrated properties

- `get_vertex_entry_cached_properties()`를 추가해 global graph vertex entry의 full-property cache 여부를
  fetch 없이 확인할 수 있게 했다.
- materializer candidate vertex seed는 full properties가 이미 hydrate된 candidate만 object cache로
  승격한다.
- terminal-property batch hydrate나 lazy hydrate가 이미 읽은 vertex는 materialized vertex object에서
  재사용하지만, seed 단계 자체는 새 heap fetch를 만들지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Seed VLE materializer candidate vertices

- `VLE_path_container`와 `VLEMaterializerHandoff`에 materializer candidate vertex를 추가했다.
- normal path는 Cypher 출력 terminal vertex, reversed path는 traversal root이자 출력 terminal vertex,
  terminal-only/zero-bound output은 단일 출력 vertex를 candidate로 둔다.
- vertex object cache와 typed vertex cache가 object cache 사용 시 candidate vertex를 먼저 seed해,
  이후 같은 materialization에서 해당 vertex를 만나면 hash hit로 받도록 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Pass VLE materializer handoff into builders

- `VLEMaterializerBuildObject` callback이 `ggctx/id/relation_cache` 대신
  `VLEMaterializerHandoff`와 graphid를 받도록 바꿨다.
- materializer cache dispatch가 handoff를 다시 낱개 인자로 풀지 않으므로, vertex/edge object builder와
  typed builder 내부까지 output requirement와 traversal root metadata가 유지된다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Carry VLE materializer root metadata

- `VLE_path_container`에 traversal root id, root validity, materializer output requirement를 추가했다.
- container size 계산은 fixed header 추정 대신 `offsetof(..., graphid_array_data)` 기준으로 바꿔
  layout 변경을 명시적으로 따르게 했다.
- `VLEMaterializerHandoff`가 output requirement와 traversal root를 함께 들고, path/list/indexed typed
  materializer lookup이 이 handoff를 공유하도록 확장했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Pass VLE materializer handoff descriptors

- VLE materializer cache lookup이 `GRAPH_global_context`, relation cache, build callback을
  낱개 인자로 받지 않고 `VLEMaterializerHandoff` descriptor를 받도록 바꿨다.
- path output, `nodes(p)`, `relationships(p)`, indexed typed vertex/edge materialization
  호출부가 같은 handoff contract를 사용한다.
- 이 변경은 다음 root/source 기반 terminal hydrate와 output-requirement materialization 지연을
  materializer cache 경계에 붙이기 위한 구조 정리다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Remove public age_vle entry

- SQL-visible `ag_catalog.age_vle(...)` overload를 제거하고 Cypher lowering 전용 entry를
  `ag_catalog.age_vle_internal(...)`로 옮겼다.
- grammar의 VLE pseudo function은 `vle_internal`로 낮아지고, optimizer는
  `age_vle_internal` Function RTE만 `AGE VLE Stream` CustomScan 후보로 받는다.
- public function surface regression은 제거하고, VLE engine 검증은 Cypher marker stream의
  result/plan evidence 중심으로 전환했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Anchor Cypher VLE streams with a stable marker

- Cypher VLE lowering이 `FunctionScan` anchor 대신 stable marker를 담은 `VALUES` RTE를
  FROM clause에 추가하고, optimizer가 이를 `AGE VLE Stream` CustomScan으로 교체하게 했다.
- marker는 hidden text column으로 두고, stream output `edges`는 첫 column으로 유지해 기존
  scan tuple slot contract와 `Var(edges)` attno를 맞췄다.
- `edges` placeholder는 `NULL::agtype` const가 아니라 `agtype_volatile_wrapper(NULL::agtype)`로
  만들어 compact VLE consumer에서 PostgreSQL이 `age_vle_path_length(NULL)`처럼 plan-time folding하는
  것을 막았다.
- 직접 `age_vle_internal()`을 호출하던 regression은 제거하거나 Cypher marker stream result/plan
  검증으로 대체했다. 이 단계에서는 terminal-property OID lookup과 일부 internal transition path가
  다음 제거 대상으로 남았다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Retarget terminal property VLE through marker descriptors

- terminal vertex property output이 더 이상 9-arg `age_vle_internal()` OID lookup으로 stream을
  retarget하지 않는다. parser가 marker `VALUES` RTE row에 terminal-property key slot을 직접 추가하고,
  optimizer는 이 9-slot descriptor를 `AGE VLE Stream` CustomScan으로 계획한다.
- optimizer의 `age_vle_internal` Function RTE CustomScan fallback을 제거했다. VLE CustomScan 진입은
  stable marker RTE에만 의존한다.
- 9-arg `age_vle_internal(...)` SQL overload를 제거했다. 남은 internal SRF surface는 7/8-arg 전환
  경로로 제한된다.
- `cypher_vle`에 `EXPLAIN (VERBOSE, COSTS OFF)` regression을 추가해 `VLE Shape:
  terminal-property`, `VLE Arguments: 9`, terminal-property const slot을 expected에 직접 남겼다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Remove age_vle_internal SQL surface

- 7/8-arg `age_vle_internal(...)` SQL overload와 C SRF wrapper를 제거했다. `AGE VLE Stream`
  CustomScan은 marker `VALUES` descriptor만 scan anchor로 받는다.
- `AGE_VLE_STREAM_ARG_DIRECT_COUNT`와 `direct-sql` explain shape를 제거해 descriptor contract를
  Cypher marker stream과 terminal-property marker stream으로 좁혔다.
- `age_vle_iterator_create()`는 public header에서 제거하고 `age_vle.c` 내부 static adapter로 내렸다.
  남은 다음 단계는 이 adapter가 `FunctionCallInfo`를 재구성하지 않고 `AgeVLEInput` typed slot에서
  traversal context를 직접 만드는 것이다.

## 2026-06-04: Build VLE context from AgeVLEInput directly

- `AgeVLEInput`에서 `FunctionCallInfo`를 재구성하던 내부 adapter를 제거했다.
- `build_local_vle_context()`와 vertex/range/direction argument reader는 `AgeVLEInput` slot을 직접
  읽는다.
- `AgeVLEInput`과 `AGE VLE Stream` CustomScan private descriptor에서 function OID/input collation
  metadata를 제거했다. stream descriptor는 argument expression list, nargs, const slot flags,
  range/direction metadata만 보관한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Pass typed VLE range into traversal context

- `AGE VLE Stream` executor가 planner-derived range/direction descriptor를 `AgeVLEInput`에 복사한다.
- `build_local_vle_context()`는 known lower/upper/direction slot에서 agtype argument를 다시 파싱하지
  않고 typed int/null metadata로 `lidx`, `uidx`, `edge_direction`을 설정한다.
- dynamic range/direction slot fallback은 기존 agtype parser를 유지한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Pass VLE output descriptors into traversal

- planner가 grammar-node와 terminal-property key const를 output descriptor로 추출해
  `AGE VLE Stream` plan private에 싣는다.
- executor는 output descriptor를 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 known
  grammar-node/terminal-property key slot에서 agtype argument 재파싱을 건너뛴다.
- `EXPLAIN (VERBOSE)`는 실행마다 바뀔 수 있는 grammar-node numeric id 대신 `cached` 또는
  `terminal-only` marker와 terminal-property key를 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Pass VLE graph descriptors into traversal

- planner가 graph name const를 graph descriptor로 추출해 `AGE VLE Stream` plan private에 싣는다.
- executor는 graph descriptor를 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 known graph
  slot에서 agtype argument 재파싱 없이 graph oid lookup과 global graph load decision을 시작한다.
- `EXPLAIN (VERBOSE)`의 `AGE VLE Stream` output에 `VLE Graph`를 추가해 graph descriptor contract를
  expected output에 드러냈다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Pass VLE edge descriptors into traversal

- planner가 edge prototype const에서 label name, properties object, property count를 edge descriptor로
  추출해 `AGE VLE Stream` plan private에 싣는다.
- executor는 edge descriptor를 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 known edge
  slot에서 edge agtype 전체를 다시 파싱하지 않고 edge label lookup과 property constraint setup을
  진행한다.
- `EXPLAIN (VERBOSE)`의 `AGE VLE Stream` output에 `VLE Edge`를 추가해 edge descriptor contract를
  expected output에 드러냈다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: Normalize VLE endpoints before traversal

- `AGE VLE Stream` executor가 start/end expression evaluation 직후 vertex 또는 integer agtype을 typed
  graphid endpoint로 정규화해 `AgeVLEInput`에 보관한다.
- `build_local_vle_context()`의 cached context refresh와 새 context creation 경로는 start/end agtype
  slot을 다시 파싱하지 않고 typed endpoint만 읽는다.
- `EXPLAIN (VERBOSE)`의 `AGE VLE Stream` output에 `VLE Endpoints`를 추가해 start/end가 const/runtime
  typed-id contract로 traversal에 넘어가는 것을 expected output에 드러냈다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Split VLE input accessors

- `AgeVLEInput` semantic accessor와 dynamic fallback parser를 `src/backend/utils/adt/age_vle_input.c`로
  분리했다.
- `age_vle.c`는 graph/start/end/range/direction/output key를 직접 parsing하지 않고
  `age_vle_input_*` API를 통해 traversal context를 채운다.
- 이 분리는 다음 traversal state/cache/materializer 경계 변경을 `age_vle.c`의 input slot parsing과
  섞지 않기 위한 구조 정리다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Use typed endpoints in VLE load decision

- targeted edge-label VLE load decision이 typed start/end endpoint descriptor를 직접 보게 했다.
- bound end만 있는 paths-to shape는 reverse traversal root가 end로 확정되므로 vertex metadata list에서
  임시 start를 꺼내지 않고, targeted adjacency/lazy hydrate 경로의 vertex metadata scan 생략 후보로
  처리한다.
- `cypher_vle`의 right-bound paths-to explain을 `EXPLAIN (VERBOSE, COSTS OFF)`로 바꿔 `VLE Edge`와
  `VLE Endpoints` descriptor evidence를 expected output에 남겼다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE edge prototype fallback into input accessors

- edge prototype dynamic fallback parsing을 `age_vle_input.c`로 옮겼다.
- `age_vle.c`는 edge argument agtype을 직접 열지 않고 `AgeVLEInputEdgePrototype`으로 받은 label/property
  descriptor만 사용해 label lookup과 property constraint setup을 진행한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE stream descriptor module split

- `AGE VLE Stream`의 descriptor parsing과 verbose explain formatting helper를
  `src/backend/executor/cypher_vle_stream_descriptor.c`로 분리했다.
- `cypher_vle_stream.c`는 실행 루프, expression evaluation, iterator lifecycle 중심으로 남기고,
  range/direction/slot explain contract는 별도 모듈에서 관리한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE range and direction descriptor

- planner가 `AGE VLE Stream` CustomPath 생성 시 lower/upper range와 direction const를
  agtype expression tree가 아니라 typed int/null descriptor로 추출해 `custom_private`에 싣도록 했다.
- executor state는 새 range/direction descriptor를 읽어 verbose explain과 다음 iterator input 전환의
  기준 metadata로 보관한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE CustomScan verbose explain descriptor

- `AGE VLE Stream` CustomScan의 `ExplainCustomScan` hook을 채워 `EXPLAIN (VERBOSE)`에서
  VLE shape, argument count, slot const/dynamic layout, range, direction을 출력하게 했다.
- 기존 `Custom Scan (AGE VLE Stream)` 존재 여부만 보이던 regression은 scan descriptor 근거를
  expected output에 직접 남긴다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE planner const slot flags

- planner가 `AGE VLE Stream` CustomPath를 만들 때 VLE argument별 const 여부를 descriptor로
  `custom_private`에 싣도록 했다.
- executor는 argument expression tree를 직접 분류하지 않고 planner-provided const flag를 따라
  const slot은 plan-time Datum으로, dynamic slot은 `ExecInitExpr`/`ExecEvalExpr`로 처리한다.
- 이 변경은 VLE input을 SQL expression list에서 planner-derived typed descriptor로 옮기는 중간 단계다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE constant argument slot cache

- `AGE VLE Stream` semantic argument slot 중 `Const` slot은 `ExecInitExpr` 대상에서 제외하고,
  plan-time Datum/null descriptor로 CustomScan state에 고정한다.
- graph name, edge prototype, lower/upper range, direction, grammar node, terminal property key처럼
  상수인 VLE inputs는 iterator 생성 때 expression evaluator를 거치지 않고 `AgeVLEInput`에 바로 채운다.
- start/end vertex처럼 Var/Param일 수 있는 slot은 기존처럼 per-rescan expression evaluation을 유지한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE iterator input descriptor

- `AgeVLEInput` descriptor를 추가해 VLE iterator creation 입력을 fixed-size semantic argument
  vector로 받게 했다.
- `AGE VLE Stream` executor는 더 이상 `FunctionCallInfo`를 직접 만들지 않고, semantic slot 평가 결과를
  `AgeVLEInput`에 채운 뒤 `age_vle_iterator_create_from_input()`을 호출한다.
- SQL-visible `age_vle()` wrapper는 기존 `FunctionCallInfo` path를 유지하므로 direct SRF compatibility는
  보존하고, CustomScan path만 descriptor API로 한 단계 더 분리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE stream semantic argument slots

- `AGE VLE Stream` executor가 VLE argument를 단순 `List`로 보관하지 않고
  graph/start/end/edge/lower/upper/direction/grammar-node/terminal-property semantic slot array로
  초기화하게 했다.
- CustomScan descriptor에 nargs를 추가해 direct SQL `age_vle` 7-arg, Cypher lowered 8-arg,
  terminal property 9-arg shape를 같은 descriptor contract 안에서 구분한다.
- 이 단계는 아직 SQL argument expression을 평가하지만, executor state가 typed VLE call slot을 갖게
  되어 다음 단계에서 start/end/range/materialization descriptor로 치환할 수 있다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE CustomScan argument descriptor

- `AGE VLE Stream` planner path가 `FuncExpr` 전체를 `custom_exprs`에 싣지 않고,
  argument list는 `custom_exprs`, `funcid/inputcollid`는 `custom_private` descriptor로 넘기게 했다.
- executor는 `FuncExpr` node를 직접 보지 않고 descriptor로 `FunctionCallInfo`를 구성한 뒤
  `AgeVLEIterator`를 구동한다.
- `issue_1910`의 `EXISTS((n)-[*]-...)` query에 `EXPLAIN (VERBOSE, COSTS OFF)` 출력 regression을
  추가해 조기 종료 subplan에서도 `Custom Scan (AGE VLE Stream)`을 사용하는 plan evidence를 드러냈다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: property index descriptor 구조화

- PostgreSQL scalar `numeric` property descriptor를 추가했다. 새
  `agtype_object_field_numeric()`/`agtype_to_numeric()`/`numeric_to_agtype()` SQL object와
  `pg_numeric` Cypher typecast를 연결해 기존 `numeric` agtype semantic과 분리했다.
- `create_property_index(..., 'pg_numeric')`은 numeric Datum expression index를 만들고,
  `n.payload.gpa::pg_numeric >= 3.8::pg_numeric`이 `Index Scan`으로 연결되는 것을 `index`
  regression 출력으로 고정했다.
- `cypher_match`에 `n.payload.a::pg_numeric, count(*)` plan 출력을 추가해
  `HashAggregate`/`Sort` lower target이 `numeric + raw count`만 carry하고 final projection에서
  count만 agtype으로 감싸는 것을 고정했다.
- `age_collect_numeric(numeric)` aggregate를 추가하고 typed collect rewrite를 NUMERICOID까지
  확장했다. `collect(DISTINCT n.payload.a::pg_numeric)`은 generic `age_collect(agtype)` 대신
  `age_collect_numeric(DISTINCT agtype_object_field_numeric(...))`으로 낮아진다.
- typed collect aggregate 선택을 `(value_type, aggregate name, cached oid)` descriptor table로
  묶었다. `pg_float8`/`pg_bigint`/`pg_numeric`/`pg_text` rewrite와 DISTINCT collect 판정이
  같은 descriptor 경로를 사용해 새 scalar type 확장이 함수별 OID helper 나열로 번지지 않게
  했다.
- scalar property helper와 final `agtype` materializer 판정도
  `(value_type, field helper, field result type, final materializer)` descriptor table로
  묶었다. `pg_bigint`/`pg_float8`/`pg_numeric`/`pg_text` sort descriptor와 final projection
  materializer가 같은 metadata 루프를 사용한다.
- property access signature에 physical field result type을 추가하고 parser signature helper도
  descriptor table로 묶었다. `numeric` agtype helper와 `pg_numeric` Datum helper가 같은
  `(container, key path, NUMERICOID)`처럼 보이더라도 index matching에서 서로 섞이지 않는다.
- optimizer-local scalar descriptor에서 property field helper name/OID cache를 제거하고
  `cypher_property_signature`의 field descriptor API를 사용하도록 통합했다. field helper,
  expression index matching, terminal path append가 같은 physical result type metadata를
  공유한다.
- scalar-to-agtype final projection 경계를 `Node *arg` 반환 API에서
  `CypherScalarFinalHandoff` descriptor로 바꿨다. lower target canonicalization은 scalar expr,
  semantic value type, physical result type, final materializer OID, optional property
  signature를 함께 받는다.
- typed collect aggregate rewrite와 DISTINCT collect narrow path detection을
  `CypherTypedCollectHandoff` descriptor로 묶었다. aggregate input expr, value type, aggregate
  OID, optional property signature가 같은 경계를 통과한다.
- typed collect handoff lookup을 DISTINCT 전용 API에서 `require_distinct`를 받는 공통 API로
  넓혔다. non-DISTINCT `collect(n.payload.a::pg_numeric)`은 typed aggregate로 낮아지지만 아직
  child scan target이 `n.properties`를 carry하는 plan을 출력 regression으로 드러냈다.
  projection-only path 교체는 existing typed DISTINCT collect에서 setrefs 오류를 내므로 다음
  작업은 Aggref argument와 child projection target을 함께 묶는 별도 lower aggregate boundary다.
- typed collect AggPath rewrite가 copied path target의 Aggref argument도 slot expression으로
  바꾸도록 보강했다. DISTINCT collect는 기존 Sort boundary의 scalar lower target을 유지하고,
  plain collect는 아직 PostgreSQL base scan의 physical tlist 표시를 벗어나지 못했다.
- expression index matching을 `Node *index_expr` 반환 API에서 `CypherPropertyIndexHandoff`
  descriptor로 바꿨다. query expr, matched index expr, optional property signature가 restriction
  rewrite와 pre-planner index surface rewrite까지 함께 전달된다.
- scalar final, typed collect, property index handoff가 모두 `CypherPropertyHandoffDescriptor`를
  선택적으로 들도록 통합했다. property signature와 final materializer OID, typed aggregate
  OID, matched index expr이 같은 cached-property 후보 descriptor에 모인다.
- `CypherCachedPropertySlotDescriptor`를 추가하고 simple property projection CustomPath가
  key-only private data 대신 key, semantic value type, physical field result type metadata를
  CustomScan까지 넘기도록 했다. executor state도 이 slot metadata를 보관한다.
- AGE Property Projection CustomScan이 slot metadata의 physical field result type을 사용해
  `pg_bigint`/`pg_float8`/`pg_numeric`/`pg_text` scalar Datum을 직접 출력할 수 있게 했다.
  `RETURN n.i::pg_bigint` plan과 결과를 출력 regression으로 고정했다.
- AGE Property Projection CustomScan의 private data를 단일 key에서 key path list로 확장했다.
  executor는 nested object path를 직접 따라가고 terminal value만 physical field result type에
  맞춰 Datum으로 출력한다. `RETURN n.payload.a::pg_numeric` plan과 결과를 출력 regression으로
  고정했다.
- ordered property projection delay에서 output property와 sort property의
  semantic/physical descriptor가 같으면 ctid/id refetch final expression을 만들지 않고
  typed sort key를 final output으로 재사용하게 했다. nested `pg_bigint` ordered projection은
  lower path가 typed computed column 하나만 carry하는 출력 regression으로 고정했다.
- `CypherCachedPropertySlotDescriptor`에서 canonical property field expression을 재구성하는
  builder를 추가했다. scalar final handoff와 typed DISTINCT collect lower target은 원본 expr만
  복사하지 않고 slot descriptor를 거쳐 `(container, key path, semantic/physical type)` 기준의
  computed column을 만든다.
- expression index handoff도 matched index expression을 설정할 때 같은 cached-property slot
  expression builder를 먼저 사용한다. query/index surface가 같은 descriptor로 matching되면
  restriction rewrite와 pre-planner surface rewrite가 canonical property expression을 공유한다.
- restriction rewrite와 pre-planner index surface rewrite도 raw `index_expr`를 직접 복사하지
  않고 `CypherCachedPropertySlotDescriptor`를 통해 rewrite expression을 만든다. matched
  expression index surface가 cached-property slot handoff와 같은 boundary를 통과한다.
- partial index predicate와 `check_index_predicates()` 이후 `indrestrictinfo` clause를
  cached-property slot expression 기준으로 정규화한다. 실제 index surface가 raw
  `agtype_access_operator`인 경우는 보존하고, scalar/typed canonical surface와 이미 일치하는
  index/predicate expression만 정규화 대상으로 삼는다.
- VLE `age_adjacency` payload cache 결정 첫 scan을 cache seed로 재사용하게 했다. fan-out이
  2개 이상인 source vertex는 cache enable 판단과 DFS frame push를 위한 scan 결과를 버리지 않고
  같은 `(index_oid, source_vertex_id)` cache entry에 남겨 이후 반복 traversal에서 directory/main
  payload scan을 다시 하지 않는다.
- VLE terminal property direct output에서 traversal frame이 이미 `vertex_entry`를 들고 있는
  경우 id 기반 helper를 다시 타지 않고 entry-aware helper로 직접 내려가게 했다. cached result,
  block prefetch, lazy relation cache lookup은 `get_terminal_property_for_entry()`로 통합했다.
- VLE graph-name/label-name lookup cache를 단일 entry에서 작은 generation-aware cache로
  확장했다. label이 번갈아 나오는 반복 VLE workload에서도 graph OID와 edge label relation OID
  lookup이 매번 catalog/cache probe로 내려가지 않는다.
- VLE init의 outgoing/incoming `age_adjacency` index OID lookup을 label cache generation 기준
  pair cache로 바꿨다. targeted adjacency VLE가 load decision과 local context refresh에서 같은
  edge label relation의 index list를 방향별로 다시 열지 않는다.
- VLE edge property metadata load contract를 edge property constraint가 있을 때로 좁혔다.
  grammar node id가 있어 VLE local context를 cache하는 query라도 constraint가 없으면 cold
  edge scan에서 properties varlena를 읽어 count/size/hash를 계산하지 않는다.
- targeted edge-label fallback VLE에서 bound start, no-property-constraint, 8-argument cached
  path shape는 edge endpoint id로 traversal용 skeletal vertex entry를 만든다. vertex table
  full scan은 피하고, path/vertex/property materialization이 실제로 필요해지면 label relation의
  vertex id index로 TID/properties를 lazy hydrate한다. 첫 시도는 전체 edge-label 없는 VLE까지
  metadata를 생략해 dangling edge regression을 냈고, 최종 contract는 targeted edge label로
  좁혀 통과시켰다.
- skeletal vertex lazy hydrate 결과는 `vertex_entry.cached_properties`에 저장한다. 같은 path
  materialization 안에서 동일 vertex properties를 반복 요청해도 vertex id index probe와 heap
  fetch를 다시 하지 않는다.
- 검증: macOS `make -j16`, `make -j16 install`,
  `installcheck REGRESS='cypher_vle age_adjacency age_global_graph'`, `git diff --check`.
- `NestedPgNumericIndex` regression에 `pg_numeric` index만 있는 상태에서 `numeric` cast query가
  `Seq Scan`과 `agtype_object_field_cmp` surface로 남는 출력 plan을 추가해 physical descriptor
  mismatch가 index path로 잘못 연결되지 않음을 고정했다.
- `ORDER BY n.payload.a::pg_numeric LIMIT 1`은 nested ordered property projection delay에서
  lower path가 `ctid + numeric sort key`만 carry하고 final output에서 ctid refetch로 agtype
  property를 materialize하는 plan을 출력 regression으로 고정했다.
- fixed path/VLE materialization plan 검증을 `DO` assertion block에서
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력 regression으로 바꿨다. graph OID처럼 실행마다 달라지는
  literal은 `pg_temp.normalized_verbose_explain()`에서 정규화하고, `_agtype_build_path_raw`,
  direct property helper, `age_materialize_vle_edges` 같은 plan evidence는 expected 파일에
  그대로 노출한다.
- property access signature 비교를 `(container, key path)` descriptor 추출 API로 분리했다.
  expression index matching은 query expr에서 descriptor를 한 번 추출한 뒤 index expression
  surface와 비교한다.
- AGE hook에서 property restriction surface를 rewrite한 뒤 `create_index_paths()`만 다시
  호출하지 않고 `check_index_predicates()`도 다시 실행해 partial expression index의
  `predOK`/`indrestrictinfo` 상태를 갱신하도록 했다.
- `cypher_match`에 partial direct helper expression index plan assertion을 추가했다.
- `create_property_index(..., 'payload.score', 'pg_bigint')`가 nested typed scalar expression
  index를 만들도록 확장하고, `n.payload.score::pg_bigint >= 10`이 같은 surface로
  rewrite되어 `Index Scan`을 쓰는 regression을 추가했다.
- no-index nested property equality/range predicate를 prefix object와 terminal key direct
  helper로 낮춰 terminal agtype materialization을 피하도록 했다. prefix access의
  `funcvariadic` flag 누락으로 nested pattern matching이 깨진 실패를 확인했고,
  `VARIADIC ARRAY[...]` surface를 유지해 통과시켰다.
- `LIMIT`이 없는 grouped count plan도 lower path에서는 raw `count(*)`를 유지하고 final
  projection에서만 agtype으로 감싸도록 확장해 `HashAggregate`/`Sort` 입력 width를 줄였다.
- `int8_to_agtype` 전용 lower/final 추출기를 scalar-to-agtype final materialization
  descriptor로 일반화했다. 현재 count wrapper뿐 아니라 `float8_to_agtype`,
  `text_to_agtype` shape도 같은 handoff API로 인식한다.
- non-LIMIT grouped count assertion을 `pg_float8`, `pg_text`, `numeric` typed property key까지
  확장해 lower `HashAggregate`/`Sort`가 raw `count(*)`를 유지하는지 고정했다.
- deferred projection join child target을 기존 target 복사 대신 required expression과
  join key 중심으로 재구성했다. joined edge ordered property projection에서 inner join이
  `r.properties` 원본을 carry하지 않고 `r.id`, typed sort expression, join key만 유지하는
  plan을 `EXPLAIN (VERBOSE, COSTS OFF)` 출력 regression으로 고정했다.
- `agtype_object_field_exists_nonnull`의 입력 contract를 properties object에서 scalar
  vertex/edge entity까지 확장했다. `count(startNode(...).name)` 같은 VLE endpoint property
  count가 planner guard 없이 같은 descriptor helper를 쓰면서 semantic을 유지한다.
- property access signature descriptor에 typed/scalar value type을 추가했다. 수동 chained
  prefix typed expression index와 query typed property surface가 같은 descriptor로 matching되어
  `Index Scan`을 쓰는 `index` regression 출력을 추가했다.
- partial typed expression index도 descriptor rewrite 뒤 `predOK`와 `Index Scan`을 얻도록
  출력 regression을 추가했다. cached/non-null terminal property handoff는
  `cypher_property_signature` API가 terminal object/key를 제공하게 바꿔 index predicate와
  no-index helper rewrite가 같은 prefix/terminal descriptor를 공유한다.
- nested ordered property projection의 final refetch가 key path descriptor를 받도록
  확장했다. `ORDER BY n.payload.a::pg_bigint LIMIT 1`은 lower path에서 prefix object를 carry하지
  않고 `ctid + typed sort key`만 유지하며, final output에서 ctid refetch 뒤 terminal key를
  materialize한다.
- parser의 scalar property typecast rewrite가 `cypher_property_signature` terminal descriptor를
  쓰도록 바꿨다. nested typed collect와 nested numeric collect 출력 plan에서 variadic prefix
  accessor가 direct object-field prefix로 낮아지는 것을 고정했다.
- `age_collect_numeric_path_property(properties, key_path_array)` aggregate를 추가했다.
  nested numeric collect는 prefix object를 aggregate input으로 만들지 않고 transition 내부에서
  key path를 순회한다. plan 출력과 실제 collect 결과 regression으로 고정했다.
- `array_agg` 단일 property, map2/map/list aggregate transition도 scalar key 또는 key-path
  agtype list descriptor를 받도록 확장했다. nested aggregate output은 root `properties`와
  path descriptor만 aggregate input으로 넘기고 prefix object를 carry하지 않는다.
- `cypher_match`에 nested `array_agg(n.payload.a)`, map2, 3-key map, list의
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력과 실제 result regression을 추가했다.
- `cypher_paths.c`에 남아 있던 `array_agg` property/map/list aggregate rewrite와 OID cache를
  `cypher_property_paths.c`로 옮겼다. collect/numeric/array_agg property aggregate rewrite가
  같은 property signature descriptor 모듈을 공유한다.
- scalar-to-agtype final output이 같은 lower scalar expression을 반복할 때 lower target에는
  한 번만 추가하도록 했다. duplicate grouped count plan에서 `HashAggregate`/`Sort`가 raw
  `count(*)` 하나만 carry하는 것을 출력 regression으로 고정했다.
- lower scalar target dedupe가 property signature match도 사용하도록 확장했다. final wrapper는
  canonical lower expression을 바라보게 rewrite해 setrefs contract를 유지한다.
- direct helper prefix와 variadic prefix가 같은 nested typed property signature로
  canonicalized되는 것을 `cypher_match` 출력 plan으로 고정했다.
- expression index matching helper를 `cypher_paths.c`에서 `cypher_property_paths.c`로 옮겼다.
  property restriction rewrite는 hook-local 함수가 아니라 property signature descriptor API가
  돌려주는 canonical index expression을 사용한다.
- property equality/range와 expression index surface regression을 `DO` assertion에서
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력으로 바꿨다. no-index direct helper, accessor index,
  direct helper index, partial direct helper index, nested direct/access index의 plan evidence가
  expected 파일에 그대로 남는다.
- scalar-to-agtype final projection의 lower scalar canonicalization helper를
  `cypher_property_paths.c`로 옮겼다. lower target 중복 제거는 property signature descriptor가
  제공하는 canonical expression을 사용한다.
- `create_property_index(..., 'numeric')`/`'pg_numeric'`을
  `agtype_object_field_numeric_agtype(prefix, key)` expression index로 연결했다.
  `n.payload.gpa::numeric >= 3.8::numeric`이 생성된 nested numeric descriptor index를 쓰는
  출력 regression을 추가했다.
- typed property index expression 생성에서 type helper 선택과
  `prefix object + terminal key` 분해를 별도 helper로 분리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='index cypher_match'`

## 2026-06-04: ctid property refetch descriptor

- ordered property projection finalization helper를 vertex properties attno 암묵 구조에서
  `relid/ctid/properties_attno/key` descriptor 구조로 일반화했다.
- vertex `RETURN n.i ORDER BY n.i::pg_bigint LIMIT 1` plan assertion은 새
  `agtype_ctid_property_field_agtype(..., 2, key)` surface를 확인하도록 갱신했다.
- 같은 ordered projection delay를 `pg_float8`, `pg_text`, `numeric` property sort helper까지
  확장해 lower target이 `ctid + typed sort key`만 들도록 했다.
- grouped count query도 `pg_float8`, `pg_text`, `numeric` typed property key에서 lower
  target이 `typed key + raw count`를 유지하고 final output에서만 count를 agtype으로
  materialize하는지 `cypher_match` assertion으로 보강했다.
- `collect(n.i::numeric)`은 `age_collect_numeric_property(properties, key)` descriptor
  aggregate로 rewrite하도록 활성화했다.
- typed DISTINCT collect coverage에 `pg_float8`를 추가해 int8/text와 같은 narrow lower
  target을 유지하는지 고정했다.
- `count(n.payload.a)`와 SQL wrapper `count(i)` nested property shape를 terminal non-null
  probe로 rewrite해 final nested value materialization을 피하도록 했다.
- `create_property_index(..., 'payload.a')`가 dotted path를 nested property key layout으로
  해석해 expression index를 만들고 `WHERE n.payload.a = ...`에서 `Index Scan`으로 쓰는
  regression을 추가했다.
- 4-arg `create_property_index(..., property_type)`를 추가해 `pg_bigint` scalar helper
  expression index를 만들고 typed property comparison의 literal side를 scalar로 낮춰
  `Index Scan`을 쓰도록 했다.
- edge ordered property projection은 endpoint join target handoff가 아직 부족해
  `variable not found in subplan target lists`로 실패했다. 적용 범위는 vertex로 유지하고
  실패 원인은 다음 join descriptor 작업 후보로 남겼다.

## 2026-06-03: 커밋 정리와 upgrade 정책

- `6183e8e8`부터 `af5200a6`까지의 많은 작은 최적화 commit을 세 묶음으로 정리했다.
  - `Add Cypher function metadata`
  - `Optimize VLE traversal and path output`
  - `Optimize agtype property projection`
- Workspace note 문서는 추적 대상에서 빼고 untracked 상태로 유지했다.
- `clean/master`에 force push했다.
- GCC `-Wshadow=compatible-local` 경고를 고쳐 VLE terminal batch의 중복 local
  declaration을 제거했다.
- `age_upgrade`는 regression 목록에는 남기되, extension upgrade catalog parity를
  검증하지 않는 정책 smoke test로 바꿨다.

## 2026-06-03: ORCA required target 정렬

- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca`의
  `CPhysicalComputeScalar`, `PexprPruneUnusedComputedCols`,
  `CScalarProjectElement`, `CLogicalGbAgg`를 조사해 required column과 computed column을
  분리하는 구조를 `RESEARCH.md`에 기록했다.
- collect/property aggregate 함수를 더 나열하는 방향을 중단하고, AGE 쪽 다음 구현
  기준을 `PathTarget` lower required expression과 final projection target 분리로
  재조정했다.
- `add_deferred_ordered_property_projection_path`와
  `add_deferred_count_agtype_projection_path`의 duplicated Limit + final ProjectionPath
  생성 로직을 `add_deferred_projection_paths`로 공통화했다.
- PostgreSQL final DISTINCT가 쓰는 `UpperUniquePath`는 project할 수 없지만 subpath target을
  그대로 통과시키므로, deferred projection target 복사 루틴이 `UpperUniquePath`를
  통과하도록 확장했다. DISTINCT/Unique 경계에서도 lower target 축소를 시도할 기반이다.
- `DISTINCT` property output은 ctid refetch를 LIMIT 위로 미루면 logical distinct key와
  lower descriptor가 갈릴 수 있어 ordered deferred projection에서 제외했다.
  `RETURN DISTINCT n.i ORDER BY n.i LIMIT 1` plan assertion으로 ctid refetch가 끼지
  않도록 고정했다.
- typed `collect(DISTINCT n.i::pg_bigint|pg_text)`가 generic `age_collect`로 남는 실패를
  확인한 뒤, 기존 `age_collect_int8/float8/text` aggregate OID로 rewrite하도록
  `rewrite_collect_typed_scalar_expr`를 구현했다. 활성 regression block으로 typed
  DISTINCT collect가 typed aggregate를 쓰고 `n.properties`를 carry하지 않는지 고정했다.
- 파일 책임이 계속 커지는 문제를 TODO 지침에 반영했다. 새 최적화가 helper/projection/
  aggregate/VLE 경계를 흐리면 같은 작업 단위에서 파일 또는 모듈 분리까지 진행한다.
- `cypher_paths.c`에 typed collect OID/cache와 DISTINCT collect arg detection이 더 쌓이지
  않도록 `cypher_property_paths.c`와 `cypher_property_paths.h`를 추가했다. hook orchestration은
  `cypher_paths.c`에 남기고, typed collect aggregate rewrite 책임은 새 모듈로 분리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`
  - 중간 커밋 우선 정책에 따라 이번 단위의 full clean/Werror 검증은 스킵했다.

## 2026-06-03: property path helper 추가 분리

- `cypher_paths.c`에 남아 있던 typed property sort arg 추출, `ctid` 기반 final
  `agtype` projection expression 생성, `int8_to_agtype` lower arg 추출을
  `cypher_property_paths.c`로 옮겼다.
- `cypher_paths.c`는 upper path hook orchestration과 path cloning을 유지하고,
  typed property projection helper/OID cache 책임은 `cypher_property_paths.c`가 맡도록
  경계를 더 분리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`
  - `git diff --check`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-03: generic property key direct helper

- GROUP/DISTINCT key에서 generic `n.i`가 `agtype_access_operator()`를 carry하던 surface를
  simple select target 생성 시점부터 `agtype_object_field_agtype()`로 낮췄다.
- ORDER/GROUP target matching과 optimizer simple property matcher가 기존
  `agtype_access_operator()`와 새 `agtype_object_field_agtype()`를 같은 simple property
  signature로 취급하도록 보강했다.
- upper path가 만들어진 뒤 reltarget/path target만 바꾸는 시도는 PostgreSQL
  `create_plan`의 subplan target lookup과 맞지 않아 폐기했고, 이 판단을 `RESEARCH.md`에
  남겼다.
- `/Users/emotionbug/IdeaProjects/neo4j`의 Cypher planner/physical planning/slotted runtime을
  다음 graph planning/materialization 참고 소스로 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-03: nested property key direct helper

- `n.payload.a` 같은 chained select-target access를 variadic
  `agtype_access_operator(VARIADIC ARRAY[...])` 대신
  `agtype_object_field_agtype()` 호출 chain으로 낮추도록 했다.
- `GROUP BY`/`DISTINCT` lower target에서 nested property key가 variadic array expression을
  싣지 않는지 `cypher_match` plan assertion을 추가했다.
- nested property signature helper를 분리해 direct helper chain과 variadic array access를
  같은 `(container, key path)` descriptor로 비교하게 했다.
- `RETURN n.payload.a, count(*) ORDER BY n.payload.a`와
  `RETURN DISTINCT n.payload.a ORDER BY n.payload.a`가 planner까지 도달하고 chained
  direct helper lower target을 쓰도록 고정했다.

## 2026-06-03: property expression index surface 선택

- property predicate rewrite가 expression index와 `equal()`인 surface만 보존하던 구조를
  semantic property signature 기반으로 확장했다.
- `agtype_access_operator(properties, key)`와
  `agtype_object_field_agtype(properties, key)` expression index를 같은 logical property
  descriptor로 인식하고, matching index가 있으면 clause의 property side를 해당 index
  expression surface로 맞춘다.
- `set_rel_pathlist_hook`은 core `create_index_paths()` 뒤에 호출되므로, AGE rewrite가
  일어난 relation에서는 `create_index_paths()`를 다시 호출해 새 surface의 index path를
  생성하도록 했다.
- `cypher_match`에 direct helper expression index가 `WHERE n.i = 1`에서 index scan을
  만들고 `agtype_object_field_equals` fallback으로 바뀌지 않는지 assertion을 추가했다.
- 같은 index surface 선택을 `n.payload.a` nested property에도 확장해 direct helper chain
  index와 variadic access index가 모두 index scan을 만들도록 고정했다.

## 2026-06-03: property predicate signature 보강

- predicate rewrite의 simple property matcher가 `agtype_access_operator(VARIADIC ARRAY[...])`
  뿐 아니라 `agtype_object_field_agtype(properties, key)` 2-arg surface도 같은
  properties/key signature로 인식하도록 보강했다.
- parser surface가 direct helper 쪽으로 더 이동해도 no-index direct predicate helper와
  expression-index 보존 guard를 같은 경계에서 유지하기 위한 기반이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-03: fixed/VLE property group key surface

- GROUP/DISTINCT 잔여 variadic shape를 재조사해 fixed path indexed property와 VLE boundary
  property가 남아 있음을 확인했다.
- `relationships(p)[0].payload.a`, `startNode(relationships(p)[0]).payload.a`,
  `last(relationships(p)).payload.a`, `last(nodes(p)).payload.a` group key를
  properties object 이후 direct helper chain으로 낮췄다.
- `cypher_match` plan assertion으로 fixed path/VLE boundary property group key가
  `agtype_access_operator(VARIADIC ARRAY[...])`를 쓰지 않도록 고정했다.

## 2026-06-03: compact VLE tail count consumer

- compact VLE path consumer 판정이 `size(tail(...))`/`size(reverse(...))` wrapper를 통과해
  내부 `nodes(p)`/`relationships(p)` source를 인식하도록 확장했다.
- `size(tail(relationships(p)))` plan assertion을 추가해 `AGE VLE Stream`과
  `age_vle_edge_tail_count`를 사용하고 terminal endpoint join을 노출하지 않는지 고정했다.
- 같은 compact consumer 판정에 `isEmpty(tail(...))`/`isEmpty(reverse(...))` wrapper를
  추가했고, `isEmpty(tail(relationships(p)))`가 `age_vle_list_is_empty`와
  `AGE VLE Stream`을 쓰는지 plan assertion으로 고정했다.
- 단일 slice indirection도 element access가 아니라 slice일 때만 compact list consumer로
  인정한다. `size(relationships(p)[1..])`,
  `isEmpty(relationships(p)[1..])`, `size(tail(relationships(p))[0..1])`가
  `AGE VLE Stream`과 slice direct helper를 쓰는지 plan assertion으로 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`
  - `git diff --check`

## 2026-06-03: clean build warning fixes

- `WRITE_INT64_FIELD`와 global graph warning message에서 `%ld` 대신 `INT64_FORMAT`을
  사용하도록 수정했다.
- macOS clang 기준 `make clean -j16` 후 `make -j16 COPT=-Werror`가 통과했다.
- GCC-only `-Wshadow=compatible-local`은 로컬 clang에서 직접 재현할 수 없었지만,
  보고된 shadow 원인을 제거했다.

## 2026-06-03: VLE tail-last count 입력 축소

- `count(last(tail(nodes(p))))`, `count(last(tail(relationships(p))))`가 tail-last entity를
  materialize하지 않고 각각 `age_vle_node_tail_last_id`,
  `age_vle_edge_tail_last_id`를 count 입력으로 쓰도록 parser aggregate rewrite를 추가했다.
- `count(startNode(last(tail(relationships(p)))))`와
  `count(endNode(last(tail(relationships(p)))))`는 generic
  `age_startnode/age_endnode(age_materialize_vle_edge_tail_last(...))` 대신
  `age_vle_tail_last_edge_endpoint`의 endpoint id mode를 count 입력으로 쓰게 했다.
- `count(label/properties/type(...))`와 endpoint `label/labels/properties` count도 nullness가
  entity 존재 여부와 같으므로 direct id/endpoint helper로 좁혔다.
- tail-last count aggregate rewrite가 `cypher_expr.c`에 계속 쌓이지 않도록
  `cypher_vle_agg.c`/`cypher_vle_agg.h`로 분리했다. `cypher_expr.c`는 aggregate 생성 뒤
  rewrite hook만 호출한다.
- `count(last(tail(reverse(e))))`, `count(head(reverse(tail(e))))`,
  `count(last(tail(tail(e))))`처럼 nested slice-boundary consumer도 count 입력에서는
  entity/field mode 대신 같은 boundary의 id mode를 쓰도록 확장했다.
- `cypher_vle` plan assertion에 tail-last count가 direct id/endpoint helper를 쓰고
  tail-last entity/field materializer를 사용하지 않는지, nested slice-boundary count가
  id mode를 쓰는지 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`

## 2026-06-03: fixed path raw builder 정리

- `_agtype_build_path_label5/7/9` 같은 arity-specific helper를 제거했다.
- fixed path lowering은 VLE materialization boundary 뒤 `_agtype_build_path_raw`를
  사용하도록 정리했다.
- path helper 함수를 수동으로 늘리는 방식은 금지 방향으로 정했다.
- focused installcheck `cypher_match age_upgrade`가 통과했다.

## 2026-06-03: installcheck regression fixes

- pgvector plan assertion은 external SQL wrapper가 아니라 internal Cypher EXPLAIN을
  확인하도록 고쳤다.
- full installcheck with extras에서 발견된 expected drift를 정리했다.

## 2026-06-03: property projection/agtype scalar 최적화

- Property projection custom scan을 추가했다.
- 단순 property predicate equality/range/order는 expression index가 없을 때 direct helper로
  낮추고, expression index가 있으면 기존 accessor surface를 보존하도록 index-aware
  rewrite를 적용했다.
- Scalar agtype hash/sort/equality fast path를 int/bool/numeric/text/string 계열로
  확장했다.
- typed collect finalization을 int8/float8/numeric/text/DISTINCT 계열로 확장했다.
- ordered projection finalization에서 ctid refetch helper와 scalar writer를 공유했다.

## 2026-06-04: VLE targeted load label scan 축소

- targeted edge-label VLE load에서 `load_vertex_metadata=false`이면 전체 vertex label
  목록을 채우지 않도록 global graph load contract를 좁혔다.
- `age_adjacency` index payload가 traversal endpoint를 공급하는 경로에서는 vertex
  hashtable을 만들지 않으므로, vertex label catalog scan도 함께 생략한다.
- 이 변경은 800-label fan-out 같은 label-unrelated cold-load 비용을 줄이는 방향이다.
- targeted edge label의 `age_adjacency` outgoing/incoming index 존재 판정을 label cache
  generation 기준으로 캐시했다. 반복 VLE load에서 같은 edge label의 relation/index list를
  다시 열어 검사하지 않는다.

## 2026-06-04: plain typed collect lower tlist 고정

- lldb로 `add_narrow_typed_collect_paths()`가 만든 새 `AggPath`가 실제
  `create_agg_plan()`까지 선택되는 것을 확인했다.
- 실패 위치는 path 선택이 아니라 `create_projection_plan()`이 `CP_LABEL_TLIST` 요청에서
  lower projection을 physical tlist로 되돌리는 분기였다.
- non-DISTINCT typed collect lower target에 label을 부여해 `Seq Scan`이 `n.id, n.properties`
  physical tlist 대신 scalar property expression만 출력하도록 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: grouped typed collect lower tlist 축소

- non-DISTINCT typed collect rewrite를 `AGG_HASHED`/`AGG_SORTED` grouped aggregate까지
  확장했다.
- lower target에 group key sortgroupref expression을 먼저 보존하고 collect input scalar
  expression을 추가해 `HashAggregate` child scan이 group key와 scalar collect input만 출력하게 했다.
- `MATCH ... RETURN n.payload.b::pg_bigint, collect(n.payload.a::pg_numeric)` plan/result를
  `cypher_match` regression에 출력으로 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: multi typed collect lower tlist 축소

- typed collect handoff detector를 단일 aggregate 반환에서 list 반환으로 확장했다.
- 여러 non-DISTINCT typed collect가 같은 aggregate node에 있을 때 lower target에 각 scalar
  property expression을 별도 slot으로 싣고, aggregate target의 각 `Aggref` argument를 같은
  canonical slot expression으로 rewrite한다.
- `collect(n.payload.a::pg_numeric), collect(n.payload.b::pg_bigint)` plan/result를
  `cypher_match` regression에 출력으로 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: 단일 property array_agg lower tlist 축소

- `age_array_agg_property(properties, key_path)` aggregate target을 path 단계에서
  `array_agg(canonical_property_expr)`로 되돌리는 lower/final handoff를 추가했다.
- agtype path const를 key list로 복원하는 helper를 추가해 cached-property slot expression builder를
  array aggregate rewrite에서도 재사용하게 했다.
- `array_agg(n.payload.a)` plan은 `Seq Scan Output: n.id, n.properties` 대신
  direct scalar property expression 하나만 출력하도록 바뀌었다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: map2/list array_agg lower tlist 축소

- `age_array_agg_map2_property`와 `age_array_agg_list_property`도 path 단계에서
  `array_agg(agtype_build_map_nonull(...))`, `array_agg(agtype_build_list(...))` lower input으로
  되돌렸다.
- child scan은 full `n.properties` 대신 direct property field expression을 포함한 row expression만
  출력한다.
- 3개 이상 map aggregate는 아직 descriptor aggregate를 유지한다.
- lldb로 같은 backend에 attach해 `create_upper_paths(UPPERREL_GROUP_AGG)` ->
  `add_narrow_array_agg_property_paths()` -> `find_array_agg_property_handoff()` ->
  `make_array_agg_map2_property_arg()`/`make_array_agg_list_property_arg()` 호출 경로를 확인했다.
- 비활성화된 `$property_collect_plan$` DO assertion 블록은 제거하고, 앞쪽 visible
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력이 plan regression 역할을 하도록 정리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: 3-key map array_agg lower tlist 축소

- `age_array_agg_map_property(properties, text[], agtype[])`로 남아 있던 3개 이상 map
  aggregate도 path 단계에서 output key와 property key path array const를 풀어
  `array_agg(agtype_build_map_nonull(...))` 입력으로 되돌렸다.
- child scan은 full `n.properties`를 aggregate state로 carry하지 않고, map row expression에 필요한
  direct property field expression만 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: map/list array_agg multi-slot aggregate

- map/list `array_agg` lower input에서 per-row `agtype_build_map_nonull(...)`와
  `agtype_build_list(...)` materialization을 제거했다.
- 새 generic variadic aggregate `age_array_agg_map_slots(variadic any)`와
  `age_array_agg_list_slots(variadic any)`를 추가해 arity-specific 함수를 늘리지 않고 key/value
  또는 value slot들을 aggregate input으로 직접 받게 했다.
- `CypherArrayAggPropertyHandoff`는 단일 `arg_expr` 대신 aggregate OID, argument expression list,
  argument type list를 넘기는 multi-slot handoff로 바뀌었다.
- visible `EXPLAIN` regression은 child scan `Output`이 map/list row expression이 아니라
  scalar property slot들로 분리되는 것을 보여준다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: VLE CustomScan iterator boundary

- `age_vle()`의 traversal loop를 `AgeVLEIterator` API로 분리했다.
- `AGE VLE Stream` CustomScan executor가 `ExecMakeFunctionResultSet()`으로 SRF를 감싸지 않고
  `FuncExpr` 인자를 직접 평가한 뒤 VLE iterator를 호출하도록 바꿨다.
- CustomScan rescan/end 경로에서 multi-call context cleanup을 SRF shutdown semantics와 맞췄다.
  조기 종료된 cached VLE context는 clean으로 표시하지 않아 다음 재사용 시 깨진 DFS stack을 쓰지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE cache module boundaries

- `age_adjacency` payload cache의 방향별 전역 `decided/enabled` flag를 제거했다.
- cache seed/replay 판단을 `(index_oid, source_vertex_id)` entry 단위로 바꿔, 첫 root의 낮은
  fan-out이 같은 방향의 이후 dense source cache까지 막지 않도록 했다.
- fan-out이 2개 이상인 source만 payload 배열을 유지하고, fan-out이 1개 이하인 source는 배열을
  비워 기존 no-cache 경로를 유지한다.
- payload cache key/entry lifecycle을 `age_vle_adjacency_cache.c`로 분리했다. `age_vle.c`에는
  payload를 DFS frame으로 바꾸는 callback만 남긴다.
- VLE materializer object cache의 graph별 function-context cache와 vertex/edge typed/object lookup
  lifecycle을 `age_vle_materializer_cache.c`로 분리했다. `age_vle.c`는 semantic object builder callback만
  제공한다.
- DFS candidate frame stack과 path/edge-index/vertex stack mutation을 `age_vle_traversal.c`로 분리했다.
  DFS policy는 그대로 두고 stack layout/lifecycle만 별도 내부 API로 낮췄다.
- dense edge-state flags와 local edge id -> dense index hash lifecycle을 `VLELocalEdgeState`로 묶어
  `age_vle_traversal.c`로 이동했다. `age_vle.c`는 edge-state bit policy만 남긴다.
- DFS 함수들의 공통 backtrack/consume/path-push 반복을 `age_vle_consume_next_frame()`으로
  `age_vle_traversal.c`에 낮췄다. `age_vle.c`는 cached terminal vertex handoff와
  paths-between/from/terminal-property acceptance/output policy만 남긴다.
- DFS next-vertex expansion 조건과 found terminal result cache handoff를 helper로 통합했다. 일반
  paths-between/from DFS는 acceptance 조건만 남기고 terminal-property 전용 DFS는 direct scalar cache
  policy만 유지한다.
- length/end acceptance 조건을 `accept_vle_path_*` helper로 분리했다. accepted path 반환 전에 next
  depth candidate를 push하는 기존 traversal order는 유지했다.

## 2026-06-04: Move VLE DFS acceptance into traversal descriptor

- length/end/end-vertex acceptance 조건을 `VLETraversalAcceptance` descriptor로 묶어
  `age_vle_traversal.c`로 이동했다.
- `dfs_find_a_path_between`, `dfs_find_a_path_from`, terminal-property traversal은 같은 descriptor API를
  사용하고, `age_vle.c`는 `VLE_local_context`에서 acceptance descriptor를 구성하는 역할만 맡는다.
- ORCA의 required-column descriptor 전달과 Neo4j var-length expand의 slot/writer 분리 구조를 참고해
  helper 나열 대신 policy descriptor를 traversal boundary로 넘기는 방향으로 정리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Group VLE terminal output policy

- terminal-property direct cache, 1-byte key fast path, final result handoff 조건을
  `VLETerminalOutputPolicy`로 묶었다.
- `dfs_find_a_path_between`, `dfs_find_a_path_from`, terminal-property 전용 DFS는 accepted terminal을
  같은 output policy로 cache하고, iterator는 같은 policy로 direct cached result와 fallback terminal
  property build를 선택한다.
- batch materialization path는 유지하되, 다음 분리 후보를 terminal-property id/result/null array와 label
  scan state 모듈 경계로 좁혔다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Group VLE terminal batch state

- terminal-property batch materialization의 terminal id list, fetched property result array, NULL bitmap,
  emit cursor, materialized flag를 `VLETerminalPropertyBatchState`로 묶었다.
- `append_terminal_property_id`, `materialize_terminal_property_results`, `batch_fetch_terminal_properties`,
  iterator batch emit 경로는 낱개 `VLE_local_context` 필드 대신 batch state를 사용한다.
- cached context cleanup과 free 경로에서 batch state를 reset해 다음 module boundary가 lifecycle helper를
  기준으로 갈라질 수 있게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Narrow VLE terminal batch fetch boundary

- terminal-property batch fetch에 `VLETerminalPropertyBatchFetch` descriptor를 추가했다.
- `batch_fetch_terminal_properties`, `scan_terminal_property_label_batch`,
  `cache_batch_terminal_property_tuple`은 `VLE_local_context` 전체 대신 batch state와 graph/key fetch
  descriptor를 받는다.
- 다음 파일 분리에서 terminal-property batch scan helper가 traversal context 전체에 묶이지 않도록
  graph oid, global graph context, property key surface를 명확히 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Split VLE terminal batch module

- terminal-property batch fetch, label scan, tuple property extraction 구현을
  `age_vle_terminal_property_batch.c`로 분리했다.
- `age_vle.c`는 `VLETerminalPropertyBatchFetch` descriptor를 초기화하고 traversal에서 찾은 terminal id를
  batch state에 쌓는 orchestration만 남긴다.
- `Makefile`에 새 object를 추가하고 `age_vle_terminal_property_batch.h`로 batch state/fetch API를
  노출했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Pass VLE adjacency root descriptors

- `age_adjacency` prefetch/cache handoff에 `VLEAdjacencyRootDescriptor`를 추가했다.
- missing-vertex fallback과 loaded-vertex adjacency expansion 모두 source vertex, index oid, direction,
  self-loop skip policy를 같은 descriptor로 넘긴다.
- payload scan source와 payload cache key가 같은 root descriptor를 보도록 정리했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: map/list array_agg slot-vector state

- `age_array_agg_map_slots`와 `age_array_agg_list_slots`의 transition state를 `ArrayBuildState`에서
  slot-vector state로 바꿨다.
- transfn은 row별 agtype map/list element를 만들지 않고 key/value 또는 value slot Datum만 복사해
  저장한다. finalfn에서만 map/list element를 만들고 최종 `agtype[]`를 구성한다.
- 이 변경은 이전 commit의 visible plan shape를 유지하면서 materialization boundary를 aggregate
  final 단계로 한 칸 더 늦춘다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-04: map/list slots aggregate combine

- `age_array_agg_slots_combine(internal, internal)`을 추가하고
  `age_array_agg_map_slots`, `age_array_agg_list_slots`의 `combinefunc`으로 연결했다.
- partial aggregate state merge에서도 row별 agtype map/list element로 되돌리지 않고 slot-vector
  layout을 유지한다.
- `age_array_agg_slots_serialize(internal)`와
  `age_array_agg_slots_deserialize(bytea, internal)`를 추가해 parallel transfer에서도 같은
  slot-vector state layout을 유지한다. bytea format은 version, map/list flag, slot/row count,
  map key strings, flattened agtype slot values를 담는다.
- fresh install catalog에서 두 aggregate가 같은 combine function을 참조하는 것을 확인했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - fresh install catalog probe:
    `age_array_agg_list_slots:age_array_agg_slots_combine:age_array_agg_slots_serialize:age_array_agg_slots_deserialize`,
    `age_array_agg_map_slots:age_array_agg_slots_combine:age_array_agg_slots_serialize:age_array_agg_slots_deserialize`
  - `git diff --check`

## 2026-06-04: VLE terminal batch vertex cache hydrate

- terminal property batch fetch가 vertex label table tuple을 읽을 때 global graph의 skeletal
  `vertex_entry`도 같이 채우도록 연결했다.
- `cache_vertex_entry_tuple_scalar_property()`를 추가해 TID, full properties, terminal scalar
  property cache를 한 번에 저장한다.
- 이후 같은 terminal vertex가 materialized entity로 필요해질 때 vertex id index 기반 lazy hydrate를
  반복하지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE output requirement descriptor 확대

- `AGE VLE Stream` output descriptor가 path, terminal vertex, terminal scalar property뿐 아니라
  terminal full properties requirement도 표현하도록 넓혔다.
- `properties(n)` 단독 terminal consumer는 marker row의 NULL terminal-key slot으로
  `terminal-properties` shape를 만들고, executor는 `VLE_path_container` 없이 terminal vertex
  properties Datum을 직접 반환한다.
- `EXPLAIN (VERBOSE)`는 `VLE Shape`, `VLE Output`, `VLE Terminal Output Slot`에 requirement와 slot
  source를 드러내 hidden assertion 없이 descriptor contract를 검증한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE iterator output 경계 분리

- `age_vle_iterator_next()` 안의 terminal property batch emit, terminal full-properties emit,
  VPC build, cleanup 분기를 `VLEIteratorOutputState` 기반 helper로 묶었다.
- DFS path-function dispatch와 next-start advance도 `VLEIteratorSearchState` 기반 helper로 묶었다.
  iterator loop는 batch emit, search, output emit, finish 순서만 조율하고 output requirement별
  materialization은 별도 helper 경계에서 처리한다.
- output materializer 선택은 `VLEIteratorMaterialization` descriptor로 낮춰 path, terminal vertex,
  terminal scalar property, terminal full properties, zero-bound 여부를 한 handoff로 넘긴다.
- `EXPLAIN (VERBOSE)`에 `VLE Materialization`을 추가해 output requirement가 path container,
  terminal vertex container, terminal scalar direct, terminal full-properties direct 중 어떤 handoff로
  실행되는지 expected에 드러낸다.
- 새 context의 graph/edge/range/direction/initial endpoint/load policy는 `VLETraversalSetup` descriptor로
  묶었다. setup이 찾은 adjacency index OID를 새 context에 직접 적용해 label index list 재조회를 피한다.

## 2026-06-04: VLE materializer vertex hydrate 선행 처리

- `prefetch_vertex_entry_properties_by_ids()`를 추가해 path 안의 vertex id를 중복 제거하고 label relation별
  scan으로 `vertex_entry` full properties cache와 TID를 채우도록 했다.
- `VLEMaterializerHandoff`를 사용하는 path/node-list materializer가 vertex object를 만들기 전에 이
  prefetch를 호출한다. relation cache 수명은 materializer handoff가 이미 가진 cache에 묶고, 같은 label
  relation에 충분한 uncached 후보가 모인 경우에만 scan을 선택한다. single typed vertex helper처럼 cache가
  없는 호출은 prefetch하지 않는다.
- `AGE VLE Stream` output descriptor가 path-container materialization의 label-batch vertex prefetch
  policy와 min relation candidate threshold를 보관하고, verbose `EXPLAIN` expected에 이를 출력하도록 했다.
- 이 변경은 작은 helper 단위로 쪼개지 않고 global graph cache API, VLE materializer handoff caller,
  CustomScan descriptor/EXPLAIN evidence, 문서 근거, focused regression을 하나의 VLE
  materialization-heavy hydrate 구조 변경으로 묶었다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: VLE fallback edge endpoint index source

- terminal-only, label-constrained, property-constraint-free VLE에서 필요한 방향의 edge endpoint btree index가
  있으면 global edge metadata load를 생략하고, DFS vertex expansion 시점에 `start_id`/`end_id` index를
  endpoint id로 probe한다.
- 이 local source는 `age_adjacency` payload source와 같은 dense local edge-state path를 사용한다. full
  edge metadata context를 재사용한 경우에는 btree source를 선택하지 않고 기존 in-memory adjacency list를
  사용한다.
- local edge source가 만나는 vertex는 `ensure_vertex_entry_skeleton()`으로 skeletal cache entry를 만든다.
  vertex metadata를 lazy로 둔 context에서는 on-demand skeleton insertion을 위해 vertex hash freeze를
  생략한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`

## 2026-06-04: VLE edge source descriptor 출력

- `AGE VLE Stream` verbose EXPLAIN에 `VLE Edge Source`를 추가했다. source는 `global-metadata`,
  `local-index-candidate`, `dynamic`으로 구분하고, `age_adjacency`/endpoint btree 후보와
  dense-local edge-state contract를 함께 출력한다.
- `cypher_vle` regression에 label `R` + endpoint btree index가 있는 terminal-property VLE EXPLAIN을
  추가해 `local-index-candidate, state=dense-local`이 expected output에 남도록 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-04: VLE dense-local edge-state sizing

- local index source를 쓰는 VLE edge state가 고정 1024 entry로 시작하지 않고, edge label relation의
  `reltuples`, endpoint `stadistinct`, bounded upper depth를 이용해 hash table과 flag array 초기 capacity를
  잡도록 바꿨다.
- 통계가 없거나 endpoint distinct estimate가 흔들리면 1024 fallback을 유지하고, distinct estimate는
  `[1, reltuples]`로 clamp한다. 초기 allocation은 1M entry로 cap하고, 실제 traversal이 더 많은 edge를
  만나면 기존 growth path로 확장한다.
- `vle_index_probe` regression fixture는 edge label `R`도 `ANALYZE`해 stats-backed local source path를
  준비한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: VLE cycle-check dense flag 통일

- in-memory adjacency, `age_adjacency` payload, endpoint btree provider가 short path stack을 선형 scan하지
  않고 dense `VLE_EDGE_STATE_USED` flag로 cycle-check하도록 통일했다.
- `is_edge_in_path()`와 path-depth 전달 wrapper, `VLEAgeAdjacencyScanState.path_stack_size`를 제거했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: VLE root/source context state 분리

- `VLEContextTraversalRootState`를 추가해 VLE root vertex, range bound, path function,
  direction/reverse policy, next-root cursor, source indexes/layout, `age_adjacency` scan/cache state를
  `VLE_local_context`의 별도 substate로 묶었다.
- output policy는 `VLEContextOutputState`, DFS stack mutation은 `VLETraversalState`, root/source/scan lifecycle은
  `VLEContextTraversalRootState`가 맡도록 context layout 책임을 분리했다.
- 이번 변경은 작은 helper 단위 commit으로 끊지 않고, context layout 변경, caller 정리, 문서 근거,
  focused 검증을 하나의 구조 변경 단위로 묶는다.

## 2026-06-04: VLE context handoff API 정리

- cleanup/source-index refresh/traversal start reset/range acceptance/start-root cursor/terminal cached-entry
  handoff를 `age_vle_context.c` API로 낮췄다.
- `age_vle.c` iterator search와 terminal property/materializer path가 root field 조합과 cached vertex slot을
  직접 해석하지 않고 context contract를 호출하게 했다.
- 다음 구조 단위는 adjacency expansion provider의 source layout, direction, scan/cache state를 cursor/cache
  descriptor로 묶는 것이다.

## 2026-06-04: VLE source cursor handoff 분리

- `VLEContextSourceCursor`를 추가해 adjacency expansion provider의 source vertex, direction, self-loop policy,
  fixed source kind/index, property constraint state를 묶었다.
- `age_adjacency` visible scan과 payload cache entry lookup을 `age_vle_context.c` API로 낮춰 `age_vle.c`의
  expansion path가 `VLE_local_context.root` layout을 직접 읽지 않게 했다.

## 2026-06-04: VLE candidate push traversal API 정리

- `VLETraversalCandidate`와 traversal candidate API를 추가해 edge-state match flag 확인/갱신, local edge index
  생성, frame push를 `age_vle_traversal.c`로 낮췄다.
- frame vertex carry 여부를 `age_vle_context.c` API로 낮춰 expansion provider가 output layout을 직접 읽지
  않게 했다.
- `VLEEdgePropertyMatchContext`를 추가해 edge property validation helper가 `VLE_local_context` 전체 대신
  constraint/cache descriptor를 받게 했고, source dispatch도 raw relation cache 대신 이 descriptor를 공유한다.
- `VLEAgeAdjacencyCandidateValidation`을 추가해 `age_adjacency` payload의 edge-entry fetch와 label/source
  validation을 candidate init helper로 묶었다.
- `VLEPackedAdjacencyCandidateValidation`을 추가해 packed adjacency path의 edge-entry lookup, property match,
  candidate construction도 같은 validation vocabulary로 맞췄다.

## 2026-06-04: Move VLE match descriptor lifecycle

- `VLEEdgePropertyMatchContext` 초기화를 `age_vle_candidate_source.c` expansion API 내부로 이동했다.
- source module이 edge property relation cache handoff를 수행하고, `age_vle.c`는 property constraint semantic
  matcher만 제공한다.
- `age_vle.c` provider는 expansion 전에 match descriptor를 직접 만들지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Move VLE source selection policy

- missing vertex fallback 조건과 out/in source dispatch를 `age_vle_candidate_source.c`로 낮췄다.
- normal vertex expansion의 out/in source selection과 packed fallback suppression도 source module에서 계산하게 했다.
- `age_vle.c` provider는 missing vertex 또는 vertex entry expansion API만 호출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Split VLE candidate source module

- `age_vle_candidate_source.c`와 `age_vle_candidate_source.h`를 추가해 packed adjacency, `age_adjacency`,
  endpoint-btree source descriptor와 iterator helper를 `age_vle.c`에서 분리했다.
- `age_vle.c` provider는 context source push와 packed fallback push API만 호출하고, source별 relation/index,
  payload cache replay, packed list iteration 구현을 직접 열지 않는다.
- property match semantic은 `age_vle.c`의 public helper로 유지해 source module이 Cypher property constraint
  해석을 복제하지 않게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Unify VLE candidate source push

- `VLECandidateSource`와 `push_candidates_from_source()`를 추가해 packed adjacency, `age_adjacency`,
  endpoint-btree source가 같은 `next_candidate` contract를 사용하게 했다.
- packed adjacency source도 `VLEPackedAdjacencySourceScan`으로 묶고, property match와 edge-state match marking을
  source 내부로 낮췄다.
- `age_adjacency` source는 payload를 provider에 노출하지 않고 source 내부에서 `VLETraversalCandidate`로 변환한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: Add age_adjacency payload source iterator

- `age_adjacency_visible_payload_scan_begin_key()`와 `age_adjacency_visible_payload_scan_next()`를 추가해
  visible payload scan을 callback-only API에서 pull-style key scan API로 넓혔다.
- 기존 callback scan은 새 pull API 위에서 동작하게 하고, old cached foreach helper는 제거했다.
- `VLEAgeAdjacencySourceScan`을 추가해 payload cache replay와 fresh `age_adjacency` scan을 같은 source
  descriptor에서 처리하게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE endpoint source descriptor

- `VLEContextSourceCursor`에 `edge_label_oid`를 포함해 source cursor가 index OID와 label OID를 함께 제공하게
  했다.
- `VLEEndpointIndexSourceScan`과 `VLEEndpointIndexCandidateValidation`을 추가해 endpoint-btree relation/index
  scan lifecycle, tuple field extraction, local edge index handoff, skeleton vertex handoff를 source boundary로
  묶었다.
- endpoint-btree candidate provider는 source scan에서 `VLETraversalCandidate`를 받아 traversal module에 push하는
  역할만 수행한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE label source handoff

- `VLEContextPackedAdjacencyLists`를 추가해 label-constrained packed adjacency list 선택을
  `age_vle_context.c`로 낮췄다.
- candidate provider의 직접 `edge_label_name_oid` 접근을 context edge-label accessor로 바꿔 packed adjacency와
  endpoint-btree source가 같은 label handoff boundary를 보게 했다.
- 이 변경은 relation cache lifecycle까지 별도로 커밋하지 않고, source/label handoff 구조 단위로 묶었다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE source runtime descriptor

- `AgeVLESourceStats`를 추가해 VLE expansion source의 missing-vertex fallback, `age_adjacency`,
  endpoint-btree, packed adjacency scan/candidate/push/payload replay/suppression counter를 누적했다.
- `AgeVLEIterator`와 `AGE VLE Stream` scan state가 source stats snapshot을 보존하고,
  `EXPLAIN (ANALYZE, VERBOSE)`는 `VLE Source Runtime`으로 source별 runtime 값을 출력한다.
- regression은 assertion block 대신 `EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF)`
  plan output을 그대로 남긴다.
- struct layout 변경 뒤 incremental object가 예전 offset을 쓰며 crash한 실패를 확인했고,
  `make clean` 후 전체 Werror rebuild로 layout mismatch를 제거했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

## 2026-06-04: VLE packed fallback source plan

- `VLEPackedAdjacencySourcePlan`을 추가해 packed fallback이 실제 실행 가능한 adjacency list를 가진 경우에만
  `VLECandidateSource`를 생성하도록 바꿨다.
- endpoint-btree source가 담당한 direction과 비어 있는 packed list를 plan 단계에서 제거하고, 빈 fallback은
  `packed_empty_skips` runtime counter로 남긴다.
- `EXPLAIN (ANALYZE, VERBOSE)` regression의 `VLE Source Runtime`은 endpoint-btree 1건 뒤 packed scan 0건,
  empty skip 1건을 출력한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: VLE age_adjacency payload cache seed

- `age_adjacency` source scan이 첫 payload를 바로 cache array에 append하지 않고 scan-local pending slot에
  보관하도록 바꿨다.
- 같은 source에서 두 번째 payload가 확인될 때 pending payload와 current payload를 함께 cache entry에 seed한다.
  fan-out 1 source는 cache allocation/append/discard를 피한다.
- `AgeVLESourceStats`와 `VLE Source Runtime`에 `age_adjacency_payload_cache_seeds`를 추가했고,
  `cypher_vle` regression은 endpoint-btree와 age_adjacency source를 모두 `EXPLAIN ANALYZE` 출력으로 남긴다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: VLE output materialization input context API

- terminal-property batch fetch descriptor 초기화를 `age_vle_context.c` API로 낮췄다.
- `VLEContainerBuildInput` 초기화를 context API로 옮겨 output materialization caller가 graph oid, start vertex,
  reverse-output flag, path stack, path vertex stack을 직접 조립하지 않게 했다.
- container/batch module에는 context가 만든 descriptor만 전달되므로 다음 output/materializer boundary 분리의
  입력 surface가 더 좁아졌다.

## 2026-06-04: VLE terminal batch lifecycle context API

- terminal-property batch id append, result/null allocation, fetch, materialized flag, emit cursor를
  `age_vle_context.c` API로 낮췄다.
- terminal property/properties builder와 batch materialization은 현재 terminal vertex id를 context API에서 받아
  traversal path stack과 reverse-output layout을 직접 열지 않는다.
- batch emission은 `age_vle_context_next_terminal_property_batch_result()`를 사용해
  `VLEContextOutputState`의 array layout을 caller 밖으로 숨긴다.

## 2026-06-04: VLE terminal output policy context API

- `VLEPropertyKeyDescriptor`, `VLETerminalPropertyLookup`, `VLETerminalOutputPolicy`를 `age_vle.c` local typedef에서
  context vocabulary로 올렸다.
- terminal-property lookup 초기화, output policy 초기화, direct terminal result scratch get/set/clear를
  `age_vle_context.c` API로 낮췄다.
- DFS/output hot path는 key descriptor, relation cache pointer, prefetched block state, prefetch budget,
  direct result validity를 직접 조립하지 않고 context-provided terminal output descriptor를 사용한다.

## 2026-06-04: VLE context build lifecycle 분리

- `build_local_vle_context()`의 새 context 생성, cached context refresh, graph load, setup apply, activation
  orchestration을 `age_vle_apply.c`의 `build_vle_local_context_for_input()`으로 옮겼다.
- `age_vle.c`는 `VLETraversalApplyOps`와 `VLETraversalContextCacheOps` callback을 통해 edge property cache,
  source index refresh, initial stack load, local context cache get/put만 제공한다.
- 이 변경은 VLE CustomScan 실행 구조가 typed setup/apply descriptor를 직접 소비하도록 `age_vle.c` 본체의
  setup orchestration을 줄이는 작업이다.

## 2026-06-04: VLE single-step source 선택

- VLE source layout input에 upper bound를 추가해, property constraint 없는 local dense edge-state traversal 중
  `*1..1` 범위는 endpoint-btree source를 우선 선택하도록 했다.
- `*1..2` 이상은 `age_adjacency` payload cache/replay 이점이 생기므로 기존처럼 `age_adjacency` source를
  우선 유지한다.
- `cypher_vle` regression은 `age_adjacency` 인덱스가 존재하는 상태에서 `*1..1`과 `*1..2`의
  `VLE Edge Source`/`VLE Source Runtime` 차이를 plan output으로 확인하도록 확장했다.

## 2026-06-04: VLE source cost policy descriptor

- `AGE VLE Stream` edge-source descriptor에 planner-only source cost policy text를 추가했다.
- local-index-candidate plan은 `reltuples`/endpoint fanout evidence와 함께 endpoint-btree/`age_adjacency`
  recommendation을 `VLE Edge Source` 출력에 드러낸다.
- policy recommendation은 text뿐 아니라 directed source enum으로도 CustomScan private descriptor에 들어간다.
  runtime selector input에 바로 주입하지 않고, executor/source layout handoff가 읽을 수 있는 안정적인 marker를
  먼저 만든다.
- runtime `VLETraversalSourceLayoutInput`에 fanout field를 직접 추가하는 시도는 deep property VLE lifecycle과
  맞지 않아 제거했다. selector input layout을 건드리지 않고 CustomScan private descriptor에 cost policy를
  먼저 싣는 방향으로 돌파구를 잡았다.

## 2026-06-04: VLE source fanout evidence 공통화

- `VLESourceFanoutEvidence` descriptor를 추가해 edge label `reltuples`, `start_id` fanout,
  `end_id` fanout을 한 번에 계산하고 전달한다.
- `AGE VLE Stream` planner descriptor, `age_vle_apply` local edge-state capacity 산정,
  `age_adjacency` custom path costing이 같은 fanout evidence API를 읽도록 전환했다.
- 기존처럼 각 caller가 `estimate_vle_edge_endpoint_fanout()`과 `get_vle_relation_estimated_tuples()`를 직접
  조합하지 않게 하여 다음 endpoint-btree/`age_adjacency` cost decision 변경의 입력 boundary를 정리했다.

## 2026-06-04: VLE source cost evidence descriptor

- `AGE VLE Stream` edge-source descriptor에 label relation `reltuples`와 endpoint
  fanout evidence를 추가했다.
- planner가 label/index 후보를 확인할 때 계산한 source-cost 근거를 CustomScan private descriptor에
  직렬화하고, executor explain은 runtime relcache/statistics 접근 없이 이 값을 출력한다.
- `VLE Edge Source` formatter를 `age_vle_source_cost`로 옮겨 planner cost evidence와 runtime source feedback
  formatting을 같은 source/cost module family에 묶었다.
- `cypher_vle` expected output은 local-index-candidate VLE plan에서
  `cost=reltuples=... fanout=start:.../end:...`를 직접 드러내도록 갱신했다.

## 2026-06-04: VLE source policy bounded-depth 확장

- endpoint-btree/`age_adjacency` source policy에서 depth 2까지만 costed로 보던 cap을 제거했다. bounded upper와
  property-constraint-free 조건이면 누적 branch work 모델을 모든 finite depth에 적용한다.
- `vle_fanout_policy` regression에 `*1..3` explain fixture를 추가했다. fanout 3, depth 3은
  `endpoint-work=sum(out:39/14,in:3/14)`를 출력하고 outgoing source를 `age-adjacency`로 유지한다.
- 이 변경은 큰 fan-out workload에서 source 선택을 layout fallback이 아니라 planner-computed descriptor decision으로
  설명하기 위한 전진이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE source policy 누적 work 모델

- `age_vle_source_cost`의 planner policy API를 선택 enum과 explain text를 따로 계산하는 형태에서
  `VLEStreamSourceCostInput`/`VLEStreamSourceCostDecision` 단일 decision contract로 바꿨다.
- endpoint-btree source policy 비용을 `fanout^depth` leaf work가 아니라 depth까지의 누적 branch work로
  계산한다. `*1..2` 같은 bounded VLE에서 fanout 2는 `2 + 4 = 6`, fanout 3은 `3 + 9 = 12`로 비교된다.
- `AGE VLE Stream` verbose explain은 source 선택 근거를 `endpoint-work=sum(out:current/limit,in:current/limit)`
  형태로 출력한다. 이는 plan assertion 없이 planner source decision과 executor fixed-source handoff를
  regression expected에 드러내기 위한 것이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-04: VLE traversal state 통합

- `VLETraversalState`를 추가해 DFS frame stack, path edge-id stack, path edge-index stack, path vertex stack,
  dense edge state, cached path depth를 한 boundary로 묶었다.
- `age_vle_consume_next_frame()`은 개별 stack 포인터를 받지 않고 `VLETraversalState`를 받아 used-edge flag,
  path stack push/pop, path depth update를 한 state transition으로 처리한다.
- traversal state init/reset/free를 `age_vle_traversal.c`로 옮겨 `age_vle.c`의 stack lifecycle 직접 조작을
  줄였다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match age_global_graph age_adjacency'`

## 2026-06-04: cached-property handoff slot expression 공통화

- scalar final projection, typed collect, expression index handoff가 같은 property descriptor에서
  cached-property slot metadata를 만들도록 정리했다.
- `CypherScalarFinalHandoff`, `CypherTypedCollectHandoff`,
  `CypherPropertyIndexHandoff`가 `CypherCachedPropertySlotDescriptor`를 직접 들고 다니도록 넓혔다.
  lower target, aggregate input, index restriction rewrite는 handoff 안의 cached slot metadata를 재사용하고,
  descriptor field 자체를 검사해야 하는 simple projection/canonicalization만 좁은 expression-to-slot API를 쓴다.
- `AGE Property Projection` CustomScan의 verbose explain hook을 추가해 cached slot의 source, key path,
  semantic value type, physical field result type을 plan에 직접 출력한다. simple/nested typed property
  projection regression은 CustomScan 이름뿐 아니라 slot metadata contract도 expected output으로 고정한다.
- `AGE Property Projection` executor state를 single slot에서 typed/scalar slot vector로 넓혔다. 여러
  scalar property output은 heap `properties` Datum을 한 번 읽고 descriptor별 terminal value를 채운다.
  untyped `agtype` multi-output은 별도 final materialization descriptor 전까지 기존 projection 경로를 유지한다.
- ordered property projection delay도 output/sort property access를 raw `(properties, keys)` tuple이 아니라
  `CypherCachedPropertySlotDescriptor`로 판정한다. typed sort key reuse와 ctid/id refetch final
  materialization이 cached-property slot metadata와 같은 descriptor boundary를 공유한다.
- typed collect lower target rewrite에 `CypherTypedCollectArgPlan`을 추가했다. cached slot descriptor에서
  만든 computed arg, handoff, sortgroupref를 한 번에 보관하고 lower target insertion과 aggregate target
  rewrite가 같은 planned arg를 재사용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`
  - `git diff --check`
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_vle_followup age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-03: VLE boundary property direct helper

- `head/last(nodes|relationships).field` 단일 property access를
  `age_vle_*_properties_at(...) -> agtype_access_operator(...)` 대신
  `age_vle_*_property_at(vle, index, key)`로 낮췄다.
- 전체 properties object materialization을 피하도록 했다.
- plan assertion은 `cypher_vle`에 추가했다.
- synthetic traversal timing은 traversal 비용이 지배적이라 runtime 차이를 분리하지 못했고,
  이 변경은 plan/materialization 제거 근거로만 유지한다.

## 2026-06-02: VLE traversal/path output 최적화

- Terminal-only VLE property cache와 terminal endpoint direct output contract를 적용했다.
- VLE adjacency payload replay/cache, visibility map check cache, edge TID mapping,
  vertex entry reuse를 적용했다.
- Count/length/pathless consumer에서 endpoint/path/property materialization을 건너뛰는
  lowering을 확대했다.
- Raw path/node/edge writer를 통해 VLE path materialization 비용을 줄였다.
- 단일 edge, no-property, count consumer에서는 불필요한 edge property fetch와 path cache
  setup을 생략했다.

## 2026-06-02: AGE built-in semantic metadata

- AGE built-in function semantic metadata catalog를 추가했다.
- parser/planner가 SQL function name/OID만 보지 않고 result kind, argument kind,
  fast-path 가능성을 확인할 수 있는 기반을 마련했다.
- `age_auto_column` marker signature 기반 자동 컬럼 변환 regression을 추가했다.

## 2026-06-01 이전 요약

- VLE rewrite dispatch, endpoint raw property projection, no-property VLE edge metadata
  생략, terminal endpoint join 제거 후보를 실험했다.
- `gin_agtype_path_ops`를 추가하고 nested containment recheck fast path를 적용했다.
- fixed MATCH/adjacency provider와 packed adjacency/dense VLE state 방향을 조사했다.
- 여러 micro fast path 중 benchmark 개선 폭이 작거나 semantic/indexability를 깨는 시도는
  되돌렸다. 반복 micro comparator 개선보다 구조 변경을 우선하기로 했다.
