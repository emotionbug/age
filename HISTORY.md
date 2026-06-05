# VLE Follow-up History

## 기록 원칙

- 완료된 변경과 검증 결과를 날짜별로 짧게 남긴다.
- 상세 설계 판단은 `RESEARCH.md`, VLE 구조와 benchmark는 `VLE.md`에 둔다.
- 되돌린 시도는 이유와 다음 방향만 남긴다.

## 2026-06-05: 조인오더 후보 테이블 방향 정리

- ORCA `CJoinOrderDPv2`의 property별 expression 보존과 Neo4j `expandSolverStep`/`ComponentConnectorPlanner`의
  ExpandAll/ExpandInto, value join, cartesian/apply connector 분리를 다시 확인했다.
- 다음 실행 단위를 `AGEGraphJoinComponent`/`AGEGraphJoinConnector` 후보 테이블로 좁혔다. 선택된 후보는 기존
  `Adjacency Join Order`/`VLE Join Order` EXPLAIN line에 계속 드러내고, 후보 비교 전에는 property별
  row/cost를 보존한다.
- PostgreSQL `partial_pathlist`/`Gather`/parallel join path, ORCA distribution/Motion property, Neo4j
  parallel repeat heuristic을 조사해 candidate table의 parallel metadata로 정리했다.
- `TODO.md`는 600줄대 배경 설명을 제거하고 현재 우선순위, 다음 실행 단위, 검증 기준만 남겼다.
- `cypher_graph_join` planner module을 추가해 `AGEGraphJoinComponent`/`AGEGraphJoinConnector` 후보 구조와
  serializable descriptor를 만들었다. fixed adjacency descriptor와 VLE join hook은 이 후보 entry를 소비하기
  시작했다.
- fixed adjacency candidate는 endpoint id restriction을 const key로 낮출 수 있으면 unparameterized CustomPath로
  등록한다. 후보 table의 `required_outer`와 `parallel-safe` metadata가 실제 runtime slot 의존성을 반영한다.
- Citus `CustomPath`/`CustomScan`/`DistributedPlan` wrapper와 `multi_explain` task evidence 구조를 확인해,
  AGE도 planner-only 후보와 executor-visible payload를 분리하는 방향을 유지하기로 했다.
- fixed adjacency graph join candidate와 `CustomScan` plan이 선택된 `CustomPath`의 `PATH_REQ_OUTER()`와
  `parallel_safe`를 그대로 보존하게 했다. const endpoint 후보는 unparameterized/parallel-safe로 남고, posting run
  partition contract가 없는 `parallel-aware` partial path는 아직 열지 않았다.
- `AGEGraphJoinCandidateTable`을 추가해 VLE와 fixed adjacency가 후보 table에 planner-only candidate를 등록한 뒤
  선택된 후보 descriptor만 executor-visible `CustomScan` private payload에 싣게 했다. 아직 후보는 각각 하나지만,
  다음 단계의 node/property seek, value join, `ExpandAll`/`ExpandInto` 비교가 같은 table에 들어갈 수 있다.
- candidate table에 `Path` 기반 등록 helper를 추가하고 VLE/fixed adjacency가 이를 사용하게 했다. graph join-order
  walker는 parameterized `IndexPath`와 bitmap index-backed path를 `index-anchored` 후보로 인식해, 실제 executor
  source가 index path일 때만 node/property seek를 graph 후보로 본다.
- bound endpoint VLE candidate table에 `ExpandInto` primary와 `ExpandAll` fallback 후보를 함께 등록했다. fallback은
  같은 executor source를 쓰지만 verification penalty를 더해 기존 선택과 EXPLAIN surface는 유지한다.
- fixed adjacency에서 terminal property source가 있으면 graph join-order property를 `index-anchored`로 올리고,
  connector도 `adjacency-value-join` vocabulary를 사용하게 했다.
- FalkorDB GraphBLAS 구조를 확인했다. relation/label `Delta_Matrix`, algebraic expression, batch filter matrix는 AGE
  VLE의 큰 frontier 후보 설계에 유용하지만, PostgreSQL extension에서는 GraphBLAS 직접 링크보다 native sparse
  frontier candidate를 먼저 두는 방향으로 정리했다.

## 2026-06-05: VLE join-order evidence surface 추가

- `AGE VLE Stream` verbose EXPLAIN에 `VLE Join Order` line을 추가했다.
- connector는 `vle-expand`, `vle-bidirectional-expand`, `vle-composite-expand`로 나누고, order property는
  `query-order`, `vle-frontier-anchored`, `index-anchored` vocabulary로 정규화했다.
- ORCA `CLogicalNAryJoin`/`CJoinOrderDPv2`와 Neo4j `ComponentConnectorPlanner`/VarExpand heuristic을 다시 확인해,
  VLE를 함수 호출 비용이 아니라 graph component connector 후보로 올리는 다음 방향을 문서화했다.

## 2026-06-05: VLE join-order evidence를 join hook에 연결

- adjacency 전용 join path walker를 graph expansion walker로 넓혀 `AGE Adjacency Match`와 `AGE VLE Stream`을
  같은 row adjustment surface에서 본다.
- join hook은 bound VLE `CustomPath`의 edge-source descriptor에서 `index-anchored`와
  `vle-frontier-anchored` property를 읽고, nested join row estimate를 VLE expansion path row 폭으로 조정한다.
- 조정된 row ratio를 nested join run cost에도 반영해 PostgreSQL `set_cheapest()`가 graph expansion evidence를
  실제 join path 선택에 사용할 수 있게 했다. `age_adjacency` regression의 directory-anchored shape는 plan이
  nested loop에서 hash join으로 바뀌어 expected plan surface에 이 변화를 드러낸다.
- VLE base CustomPath도 marker `Values Scan` rows=1 wrapper에서 벗어나 edge-source fanout, finite upper depth,
  composite prefilter, materialization weight로 rows/cost를 산정한다. `cypher_vle` expected의 `VLE Join Order rows`는
  fanout/depth 기반 cardinality로 갱신했다.
- 양 endpoint가 모두 제공된 VLE는 `vle-expand-into`/`vle-composite-expand-into` connector와
  `expand-into-verification` property로 출력한다. `cypher_match` expected는 runtime start/end endpoint shape가
  ExpandInto verification으로 분리되는 것을 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: 조인 오더 리서치 방향 정리

- ORCA `CLogicalNAryJoin`/`CJoinOrderDPv2`와 Neo4j `IDPQueryGraphSolver`/component connector 구조를
  기준으로 AGE graph pattern join order 방향을 정리했다.
- 다음 큰 후보를 node/property index seek, fixed `AGE Adjacency Match`, VLE stream, ExpandInto 검증을
  같은 component/connector descriptor로 비교하는 구조로 잡았다.
- VLE/`age_adjacency`의 directory fanout, terminal label/property selectivity, value identity matched count를
  scan-local evidence가 아니라 join order cardinality/connector cost 입력으로 올리는 방향을 문서화했다.

## 2026-06-05: Adjacency join-order descriptor surface 추가

- fixed `AGE Adjacency Match` EXPLAIN에 `Adjacency Join Order` line을 추가했다.
- component alias, connector kind, bound endpoint, order property, row/fanout evidence를 같은 line에 묶어
  ORCA/Neo4j식 graph component connector 탐색으로 확장할 첫 surface를 만들었다.
- join hook이 bound adjacency path의 order property를 읽어 index/directory/adjacency anchored expansion의
  nested join row estimate를 조정하게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: mixed directional family source split 적용

- `active=both` VLE가 exact `both` threshold cache를 찾지 못하고 `out`/`in` family feedback을 합쳐 소비할 때,
  한쪽 방향의 empty completion이 지배적이면 empty 쪽은 `age_adjacency` lifecycle을 유지하고 반대 productive 방향은
  `endpoint-btree`로 분리한다.
- policy reason은 `directional-family-productive`로 출력하고, planned class는 기존 `adjacency-empty-batch` 같은
  threshold lifecycle class를 유지해 runtime class match를 깨지 않는다.
- `tools/vle_benchmark.sql`은 split이 적용된 row를 `directional-family-split-applied` /
  `keep-directional-split`으로 분류한다. smoke에서 `800-label-fanout-family-path`는
  `out=age-adjacency/in=endpoint-btree`, `source_match=true`, `class_match=true`로 바뀌었다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_directional_pg_21649`, port 55467
  - `psql -p 55467 -d postgres -v graph=age_vle_bench_directional_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: anonymous edge label source handoff 정리

- VLE edge label이 anonymous인 경우 planner source lookup에서 빈 label이 아니라 graph default edge relation
  `_ag_label_edge`를 확인하도록 했다.
- 단, `_ag_label_edge`에 `age_adjacency` index가 없으면 endpoint-btree 후보만으로 dense-local source를 만들지 않고
  기존 global metadata path를 유지한다. 이 제한으로 기본 regression graph의 anonymous VLE가 invalid packed source로
  들어가지 않는다.
- `unknown-fanout`은 source family class가 아니라 age_adjacency 선택 사유이므로 planner policy class는
  `adjacency-stream`, recommendation은 `collect-endpoint-stats`로 정규화했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: composite missing-vertex feedback 정규화

- planned `adjacency-composite-prefilter` source에서 missing-vertex evidence만 남아도 dominant source가 planned
  `age_adjacency`와 일치하면 runtime class를 `adjacency-composite-prefilter`로 유지한다.
- property candidate가 reachable endpoint가 아닌 negative probe는 source handoff 실패가 아니므로
  `class-mismatch/tune-source-policy`로 올리지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_composite_missing_pg_55450`, port 55450
  - `psql -p 55450 -d postgres -v graph=age_vle_bench_composite_missing_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: benchmark source policy outcome 추가

- `tools/vle_benchmark.sql` final summary에 `source_policy_outcome`과 `source_policy_next_action`을 추가했다.
- benchmark row가 source mismatch, class mismatch, value-posting headroom 적용/승격, directional family split 후보,
  empty lifecycle 유지, payload replay 유지를 먼저 분류하므로 threshold 숫자 보정 전에 깨진 contract와 tuning 후보를
  분리할 수 있다.
- 검증:
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_policy_outcome_pg_55449`, port 55449
  - `psql -p 55449 -d postgres -v graph=age_vle_bench_policy_outcome_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: benchmark value-posting headroom table 확장

- `tools/vle_benchmark.sql` planner/runtime join summary에 `value_posting_headroom_expected`와
  `value_posting_headroom_applied`를 추가했다.
- materialization weight 기준 expected headroom은 path 18, object 20, scalar 25다. 이 column은
  `value_posting_policy_decision=policy-value-posting`이 실제 source profile headroom으로 이어졌는지 threshold
  table에서 바로 비교하기 위한 surface다.
- 검증:
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_headroom_table_pg`, port 55438
  - `psql -p 55438 -d postgres -v graph=age_vle_bench_headroom_table_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: value-posting feedback headroom 직접 반영

- `adjacency-composite-value-posting` feedback이 replay/cache seed의 보조 signal일 때만 endpoint headroom을 낮추던
  구조를 바꿨다. value-posting observed class 자체도 source policy input으로 보고 materialization weight별
  headroom을 선택한다.
- terminal-scalar value-posting observed plan은 후속 `VLE Source Payload Input`과 `VLE Source Profile`에서
  `headroom=25` / `endpoint-headroom=0.25`를 출력한다. path/object consumer는 기존 strong replay headroom과 같은
  materialization weight 기준을 공유한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_value_headroom_pg`, port 55437
  - `psql -p 55437 -d postgres -v graph=age_vle_bench_value_headroom_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: benchmark value-posting policy reason 연결

- `tools/vle_benchmark.sql` final summary가 `VLE Source Policy`의 `policy_reason`을 전달하게 했다.
- reason에 `composite-value-posting`이 포함되면 `value_posting_policy_decision=policy-value-posting`으로 분류해,
  source availability, planner policy adoption, identity cache hit, runtime value-posting hit을 같은 benchmark row에서
  비교할 수 있게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_policy_reason_pg`, port 55436
  - `psql -p 55436 -d postgres -v graph=age_vle_bench_policy_reason_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`

## 2026-06-05: composite value-posting policy reason

- 후속 planning에서 composite payload feedback이 `adjacency-composite-value-posting`이면 directed source policy reason도
  `composite-value-posting`으로 출력한다.
- source kind는 그대로 `age-adjacency`지만, `VLE Source Policy`가 `reason=out:composite-value-posting`을 보여
  endpoint headroom/source decision이 value-posting lifecycle feedback을 소비했음을 plan surface에 드러낸다.

## 2026-06-05: composite value-posting runtime class match

- planned `adjacency-composite-prefilter` 위에서 label-slice value-posting pruning이 실제 관측되면 runtime feedback을
  `adjacency-composite-value-posting` / `keep-value-posting`으로 승격한다.
- `VLE Source Plan`은 planned class가 `adjacency-composite-prefilter`여도 runtime
  `adjacency-composite-value-posting`을 stronger provided property로 보고 `class-match=true`를 출력한다.
- payload replay도 cache-seeded lifecycle뿐 아니라 composite source lifecycle을 만족하는 stronger property로 본다.

## 2026-06-05: fixed-chain value-posting replay regression

- `vle_fixed_chain_value_replay` regression fixture를 추가했다. 같은 `N` label fixed chain이 두 경로로 중간
  vertex에 합류한 뒤 terminal property fanout을 타도록 만들어 `AGE VLE Stream` fixed-chain rewrite, payload replay,
  cache seed, label-slice value-posting feedback을 같은 shape에서 확인한다.
- 첫 `EXPLAIN ANALYZE`는 `runs=scan:4/replay:1/seed:2`와 `cache-filter=16/16/0`을 출력한다. 후속
  `EXPLAIN`은 같은 payload feedback key에서
  `replay-runs=1 seed-runs=2 value-posting=label-slice/observed:16
  class=adjacency-composite-value-posting`을 소비한다.
- mixed-label explicit chain은 fixed-chain VLE rewrite 대상이 아니므로 별도 fixture로 남기지 않았다. 같은 label
  chain query와 데이터 decoy label을 분리해 rewrite contract와 runtime pruning evidence를 동시에 고정했다.

## 2026-06-05: all-depth terminal property value-posting feedback 정리

- VLE payload feedback이 source-aware value-posting pruning counter를 사용하게 했다. 기존 vertex-set value counter는
  그대로 유지하고, `value-posting=none` source에서는 property/cache filter를 value-posting evidence로 승격하지 않는다.
- `value-posting=label-slice` source가 terminal-property cache label filter를 실제로 사용하면 후속 plan의
  `VLE Source Payload Input`에 `scan-runs`, `seed-runs`,
  `value-posting=label-slice/observed:N`, `class=adjacency-composite-value-posting`이 함께 남는다.
- `vle_value_posting_feedback` regression은 `n.i = 59` 후속 plan만 value-posting payload input을 소비하고,
  `n.i = 1` 후속 plan은 feedback을 공유하지 않는 identity-safe behavior를 유지한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: fixed label-chain terminal property VLE handoff

- `vle_fixed_label_chain` regression에 `age_adjacency (start_id, id, end_id)` index와 `ANALYZE`를 추가해
  explicit fixed label-chain collapse가 endpoint btree fallback이 아니라 `age-adjacency` local source와
  cache-seeded profile을 선택하는 surface를 고정했다.
- fixed-chain rewrite가 마지막 named terminal node를 보존하게 해서 `WHERE n.i = 5 RETURN n.i`가 일반 join tree로
  풀리지 않고 `AGE VLE Stream` terminal-property output으로 retarget된다.
- planner는 VLE가 소비한 terminal property predicate를 marker child plan qual에서도 제거한다. terminal-property
  direct output이 path container가 아니므로 child `Values Scan` filter에 같은 predicate를 남기면 실제 result가
  사라지는 문제를 막는다.
- terminal property source filter id는 final vertex expansion에서만 채운다. fixed-chain all-depth label pruning은
  유지하지만, property value prefilter가 final 직전 expansion의 vertex를 검사하지 않게 분리했다.
- local `age_adjacency` payload candidate는 terminal output이 frame vertex entry를 요구할 때 skeleton vertex entry를
  보장한다. direct terminal predicate lookup도 현재 stack terminal 대신 `VLETraversalStep`의 vertex를 우선한다.
- regression은 `terminal-label=3/all-depth`, `planned=property-prefilter`,
  `class=adjacency-composite-prefilter` plan과 실제 `terminal_i = 5` 결과를 같이 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

## 2026-06-05: VLE terminal property source metadata

- VLE terminal property predicate key를 `AGE VLE Stream` edge-source descriptor에 추가했다. const predicate와
  runtime-slot predicate 모두 `AgeVLEInput`과 context apply/root state를 통해 executor로 전달된다.
- planner는 descriptor가 소비한 terminal property predicate qual을 CustomScan residual filter에서 제거한다.
  이번 fixed-chain terminal-property direct path 이후 marker child plan의 같은 predicate도 제거한다. 실제 VLE
  CustomScan output acceptance는 executor가 terminal vertex property lookup과 `agtype_eq` 비교로 수행한다.
- planner terminal property source prefilter도 all-depth terminal label mode에서만 source-applicable로 본다.
  variable range endpoint-only terminal label은 `endpoint-label-acceptance` reason과 `metadata-only` composite fanout으로
  출력해 1-hop value-posting feedback을 깊이 있는 endpoint-only query가 잘못 소비하지 않게 했다.
- DFS path emission, terminal-property direct DFS, zero-bound emission이 같은 terminal predicate acceptance helper를
  거치게 했다. 이로써 variable range terminal predicate가 expansion guard로 중간 vertex traversal을 자르지 않고,
  emitted endpoint만 거른다.
- `vle_index_probe`와 `vle_value_posting_feedback` regression expected는 residual CustomScan filter가 사라진 상태에서
  `EXPLAIN ANALYZE` actual rows가 유지되는 것을 고정한다.
- `vle_value_posting_feedback`에는 1..3 endpoint-only terminal property source/counter fixture를 추가했다. 이 fixture는
  label table 직접 insert 기반이라 result semantic보다 source descriptor와 runtime feedback surface를 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

- variable range VLE의 endpoint label marker를 traversal acceptance predicate로 분리했다. endpoint-only label은
  emitted path acceptance에서만 확인하고, expansion continuation은 같은 label로 중간 vertex를 자르지 않는다.
- source/cursor label pruning은 exact fixed range처럼 endpoint가 곧 expansion terminal인 경우에만 유지한다.
  variable range에서는 source pruning보다 semantic 보존을 우선하고, label pruning은 acceptance와 zero-bound
  emission 조건으로 제한한다.
- optimized right-bound `PATHS_TO`처럼 reverse output path에서는 outer terminal label scan이 endpoint label을
  이미 보장하므로 DFS step tail에 endpoint label acceptance를 중복 적용하지 않는다.
- endpoint-only label marker가 edge property predicate를 terminal vertex property prefilter로 잘못 켜지 않도록,
  terminal property prefilter는 all-depth terminal label mode에서만 source contract로 연결한다.
- global metadata에 vertex entry가 없는 local-source/materialized path에서도 terminal vertex property lookup이
  label relation fallback을 사용할 수 있게 skeleton vertex entry를 보장했다.
- regression expected는 `EXPLAIN (VERBOSE)`에 `terminal-label=N/endpoint`와 marker child slot을 드러내도록
  갱신했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

- `VLEContextSourceCursor`와 `VLEContextExpansionSourceRun`에 path depth evidence를 추가했다. payload source는
  `target_path_length`가 finite upper bound에 도달하는 expansion에서만 terminal property filter id를 cache key와
  payload prefilter에 넣는다.
- terminal property prefilter 준비도 filter identity가 있는 payload source에만 연결한다. final depth가 아닌
  expansion은 `terminal_property_filter_id=0`인 상태로 label-only composite filter만 유지하므로, 깊이 있는 VLE의
  중간 vertex expansion을 terminal property predicate로 잘못 자르지 않는다.
- VLE terminal label descriptor에 mode를 추가해 기존 fixed/exact label-chain marker는 `all-depth`, 새 endpoint
  marker는 `endpoint`로 구분한다. executor context는 all-depth label constraint와 endpoint-only label constraint를
  별도 필드로 보관하고, source cursor/candidate push는 target depth helper를 통해 label id를 고른다.
- variable range VLE에서 terminal node label은 upper depth 하나가 아니라 lower..upper의 모든 emitted endpoint에
  적용된다는 점을 regression으로 확인했다. 따라서 이번 단계의 parser marker 제거는 exact finite range에만 유지하고,
  variable range는 terminal acceptance filter와 expansion continuation을 분리하는 다음 구조 변경으로 넘겼다.
- terminal property predicate extractor는 output key가 없는 경우에도 VLE terminal property access에서 predicate key를
  읽을 수 있게 분리했다. 현재 path materialized shape의 terminal relation join predicate는 아직 VLE baserestrictinfo로
  내려오지 않으므로, 다음 단계는 parser marker를 terminal-only endpoint constraint로 안전하게 분리하는 것이다.
- bounded VLE에 terminal label marker를 무조건 일반화하는 시도는 중간 vertex에도 terminal property value를 요구해
  path semantic을 깨뜨리는 것으로 확인했다. 이 시도는 코드에 남기지 않고, terminal-depth-aware cursor contract를
  먼저 만든다.

- payload feedback headroom 계산을 helper로 묶어 `adjacency-composite-value-posting` class가 유지된 cache entry도
  같은 entry 안의 replay/cache seed evidence를 source profile decision에 반영하게 했다.
- `tools/vle_benchmark.sql`에 `value-posting-replay-seed` / `value-posting-replay` shape를 추가했다. 같은
  `ValuePostingEdge` label에 1-hop value-posting reject fixture와 3-hop hub replay fixture를 같이 두되, replay start
  root는 `i=299`로 분리해 `age_adjacency` posting run append 한계를 넘지 않게 했다.
- 256-edge smoke에서 `value-posting-replay`는 `payload_input_replay_runs=255`,
  `payload_input_seed_runs=2`, `endpoint-headroom=0.18`, `empty-batch=257`을 출력했다. 깊이 있는 terminal
  property query는 아직 value-posting과 replay가 같은 profile key로 동시에 묶이지 않아, 다음 과제는 planner
  start/end handoff를 바꿔 이 조합을 같은 lifecycle로 만드는 것이다.

- property source index OID와 predicate value image hash를 섞는 value identity 생성을
  `age_adjacency_property_filter_id()` 공용 helper로 이동했다.
- VLE payload cache와 fixed `AGE Adjacency Match` terminal property lookup이 같은 helper를 사용하고, fixed MATCH도
  prefetch matched set을 composite request로 넘길 때 `property_filter_id`를 채운다.
- fixed `AGE Adjacency Match` runtime EXPLAIN은 value-keyed request handoff를
  `value-identity=present|none`으로 출력한다. 숫자 hash를 expected에 고정하지 않고, value identity request가 있는지
  여부만 드러낸다.
- 이 변경은 새로운 on-disk value posting summary는 아니지만, 다음 directory/block value summary key가 VLE와 fixed
  MATCH에서 같은 identity를 소비할 수 있게 하는 contract 정리다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

- `age_adjacency` directory layout을 v16으로 올리고 homogeneous terminal label run의 48-bit compressed
  `next_entry_id` vector를 directory entry에 추가했다.
- exact graphid directory slot이 overflow된 run도 compressed directory vector가 property matched set과 교차하지 않으면
  main block을 열지 않고 `set-directory-filter=.../compressed:N`으로 접는다.
- `age_adjacency_debug_composite_probe()`는 `set_directory_compressed_filter`를 반환한다. wide bloom fixture는 같은
  19 posting skip을 compressed directory summary, wide bloom, value-summary request evidence로 함께 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

- block-level vertex-set pruning도 property value identity request 축을 별도 계측하게 했다.
- range/exact/compressed/bloom/posting block summary가 skip을 만들면 기존 counter는 유지하고, composite request의
  `property_filter_id`가 만든 value summary가 활성화된 경우 `set-block-filter=.../value-summary:N` suffix와
  `age_adjacency_debug_composite_probe().set_block_value_filter`에도 같은 posting 수를 기록한다.
- regression은 directory-only skip에서는 block value counter가 0이고, block range/compressed/posting skip에서는
  각각 `20`, `19`, `199`가 value identity request skip으로 겹쳐 잡히는 것을 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

- composite request의 `property_filter_id`가 있으면 `AgeAdjacencyVertexSetFilter`가 matched vertex id set의
  request-side value summary bloom을 준비하게 했다.
- `age_adjacency` directory pruning은 terminal label/range/label bloom 뒤 이 value summary를 64-bit/wide
  next-vertex bloom과 교차하고, negative이면 main block을 열기 전에 접는다.
- fixed `AGE Adjacency Match`와 `AGE VLE Stream` runtime output은 이 경계를
  `set-directory-filter=.../value-summary:N` suffix로 출력한다. `age_adjacency_debug_composite_probe()`도
  `set_directory_value_filter`를 반환해 wide bloom fixture에서 같은 19 posting skip을 value-summary evidence로
  고정한다.
- 이번 변경은 on-disk layout을 올리지 않고 request descriptor/value-summary handoff를 먼저 만든 단계다. 다음 단계는
  같은 evidence surface를 terminal value identity별 directory posting summary 또는 block-local value posting vector로
  승격하는 것이다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - `git diff --check`

- `AgeAdjacencyCompositeTerminalFilter`를 visible payload scan target에 보존하고, directory begin-key /
  known-empty 판단이 label/property mismatch를 같은 composite target helper에서 받도록 정리했다.
- property source summary가 있는 composite request가 directory range pruning까지 내려가면
  `composite-directory-filter=N`을 출력한다. 기존 `set-directory-filter=N`은 vertex-set range pruning 총량으로
  유지하고, 새 counter는 label+property composite request가 directory boundary에서 소비된 경우만 세어 다음
  value-summary index layout의 기준 evidence로 둔다.
- directory entry와 property source matched count를 비교해 label 후보 폭의 value-summary 상한을
  `composite=request:N/dir-estimate:N`으로 출력한다. 이는 `property-prefilter` candidate 폭을 덮어쓰지 않고,
  ORCA식 index descriptor처럼 index가 제공할 수 있는 후보 폭 estimate를 별도 physical property로 올린 것이다.
- composite request에서 발생한 block-level skip을 `composite=.../block-filter:N`으로 분리했다. 새
  `age_adjacency_debug_composite_probe()` regression은 matched vertex id 하나가 있는 request에서 compact block 하나를
  통째로 건너뛰고 `1:5:5:2:0:3:0:3:0:1` counter를 출력한다.
- `age_adjacency` directory layout을 v8로 올리고 endpoint run의 `next_vertex_id` 64-bit bloom summary를 추가했다.
  matched vertex set이 min/max range 안에 있어도 bloom에 없으면 main block을 열기 전에 safe-negative directory skip으로
  접는다. regression은 absent vertex `_graphid(1, 13)`에서 `0:6:6:0:0:0:6:0:6:1`을 고정한다.
- `age_adjacency` directory layout을 v9로 올리고 endpoint run 안의 작은 terminal label별
  `next_vertex_id` bloom slot을 추가했다. global bloom이 다른 terminal label의 vertex 때문에 positive여도
  request terminal label의 label-local bloom이 negative면 main block을 열지 않는다. regression은 label 1 request와
  label 2 vertex `_graphid(2, 10)` 조합에서 같은 safe-negative directory skip을 고정한다.
- `age_adjacency` directory layout을 v10으로 올리고 small-run exact `next_vertex_id` vector를 추가했다.
  distinct terminal vertex가 slot 안에 들어오면 bloom false positive를 main/block scan 전에 확정 negative로 접는다.
  regression은 global bloom이 positive가 될 수 있는 `_graphid(1, 87)` request를 directory skip counter로 고정한다.
- `age_adjacency` main block layout을 v11로 올리고 block-level 256-bit `next_vertex_id` bloom을 추가했다.
  property matched vertex set과 block summary가 교차하지 않으면 compact/full posting payload를 읽기 전에 block을 접는다.
  `age_adjacency_debug_composite_probe()`는 `set_block_bloom_filter`를 출력해 bloom summary가 만든 block skip을
  기존 `set_block_filter` 총량과 분리한다.
- `age_adjacency` main block layout을 v12로 올리고 block-local exact `next_vertex_id` vector를 추가했다. distinct
  terminal vertex가 slot 안에 들어오면 bloom보다 먼저 exact intersection으로 block을 접고, debug probe는
  `set_block_exact_filter`와 `set_block_bloom_filter`를 나란히 출력한다.
- `age_adjacency` main block layout을 v13으로 올리고 block-local `min/max next_vertex_id` range summary를 추가했다.
  directory range가 넓어 통과한 request도 block range와 property matched set range가 겹치지 않으면 exact/bloom 전에
  접는다. regression은 `set_block_range_filter=20`으로 이 boundary를 고정한다.
- compact main block의 payload-order exact posting intersection을 별도 pruning boundary로 분리했다. range, exact,
  bloom summary가 모두 통과한 뒤에도 sorted matched vertex set과 compact `next_entry_id`가 교차하지 않으면 cache fill
  전에 block 전체를 접고, `age_adjacency_debug_composite_probe()`와 EXPLAIN runtime은
  `set_block_posting_filter` 또는 `set-block-filter=.../posting:...`으로 이 residual exact skip을 드러낸다.
  regression은 199개 compact postings 중 absent `_graphid(1, 100)` request가 range/bloom을 통과한 뒤
  `set_block_posting_filter=199`로 접히는 케이스를 고정한다.
- `age_adjacency` directory layout을 v14로 올리고 endpoint run의 256-bit wide `next_vertex_id` bloom을 추가했다.
  기존 64-bit directory bloom이 false positive인 large run도 wide bloom이 negative면 main block을 열기 전에 접는다.
  EXPLAIN runtime은 `set-directory-filter=.../wide-bloom:...` suffix를 선택적으로 출력하고, regression은
  19개 posting run에서 `_graphid(1, 2)` request가 `set_directory_wide_bloom_filter=19`로 접히는 케이스를 고정한다.
- `age_adjacency` main block layout을 v15로 올리고 compact block-local 48-bit `next_entry_id` exact vector를 추가했다.
  graphid exact slot이 overflow된 block도 compressed entry vector가 negative면 bloom과 payload-order posting scan 전에
  접는다. EXPLAIN runtime은 `set-block-filter=.../compressed:...` suffix를 출력하고, regression은 directory가 통과한
  request에서 edge-label compact block 19개 posting이 `set_block_compressed_filter=19`로 접히는 케이스를 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency cypher_match'`

- VLE `age_adjacency` payload property prefilter runtime에 property source index가 만든 matched vertex set 폭을
  `prefetch-matches=N`으로 추가했다. 이 값은 0이면 출력하지 않아 기존 runtime line 폭을 늘리지 않고, prefilter가
  실제로 준비된 경우에만 `property-prefilter=runs/candidates/filtered`와 나란히 보인다.
- `vle_index_probe` const/runtime-slot regression은 property source matched set 2개, terminal label candidate 7개,
  filtered 6개를 같은 `VLE Payload Runtime` line에 고정한다. 이는 다음 label+value composite request가 adjacency
  posting fetch 폭을 어디서 줄여야 하는지 raw EXPLAIN surface에 남기기 위한 evidence다.
- VLE payload cache key에 terminal property filter identity를 포함했다. 같은 source vertex와 terminal label이라도
  property predicate value가 다르면 filtered payload cache와 known-empty cache를 공유하지 않는다.
- property source index prefetch 결과가 empty matched set이면 `age_adjacency` posting scan을 열지 않고 source를
  empty completion으로 접는다. `vle_index_probe` no-match ANALYZE fixture는 `property-prefilter=1/7/0`,
  `runs=scan:0`, `suppressed=out:age-adjacency`, `class-match=true`를 고정한다.
- VLE payload runtime에 `cache-filter=total/label/property`를 추가했다. property-prefilter fixture는
  `cache-filter=6/0/6`을 출력해 matched vertex set callback이 heap fetch 뒤 residual filter가 아니라
  `age_adjacency` main cache fill 전에 payload cache 폭을 줄였다는 사실을 고정한다.
- terminal property prefetch 결과를 callback ABI 대신 `AgeAdjacencyVertexSetFilter` descriptor로
  `age_adjacency` visible payload scan에 넘긴다. VLE와 fixed `AGE Adjacency Match`는 같은 descriptor를 사용하며,
  VLE runtime은 `vertex-set=1`로 prefetch set이 scan target에 직접 전달됐는지 출력한다.
- `AgeAdjacencyVertexSetFilter`에 min/max vertex id range summary를 추가했다. `age_adjacency` scan은 hash lookup 전에
  range miss를 먼저 버리고, VLE runtime은 `set-range-filter=N`을 출력한다. `vle_index_probe`는
  `set-range-filter=6`으로 property matched set range가 main cache fill 단계에서 6개 후보를 hash lookup 없이 제거함을
  고정한다.

- fixed `AGE Adjacency Match`에도 VLE와 같은 composite policy vocabulary를 추가했다.
  `Adjacency Composite Policy`는 planned class/recommendation을 출력하고, `EXPLAIN ANALYZE`에서는
  runtime class/recommendation과 `class-match`를 함께 출력한다.
- small terminal fanout에서 property source prefetch 대신 id cache를 쓰는 shape는
  `class=adjacency-composite-id-cache recommendation=keep-id-cache`로 고정했다. property source를 실제 prefilter로
  쓰는 VLE shape의 `adjacency-composite-prefilter`와 같은 class family를 공유하지만, 작은 fixed MATCH slice를
  source-prefetch로 과대 해석하지 않는다.

- VLE composite fanout을 source policy typed input으로 연결했다. planned prefilter가 있으면 endpoint fanout은 그대로
  보여주되 `age_adjacency` payload source가 해결할 수 있는 `composite-work=planned(out:1,in:0)`을 별도 costing
  evidence로 출력한다.
- const/runtime-slot terminal property predicate regression은 이제 `reason=out:composite-prefilter`,
  `class=adjacency-composite-prefilter`, `recommendation=keep-property-prefilter`를 고정한다.
- runtime feedback도 `property-prefilter=1/7/6` counter를 보면 같은 class/recommendation으로 정규화한다. 따라서
  `VLE Source Plan`은 `planned-class=adjacency-composite-prefilter class-match=true`를 출력한다.
- `tools/vle_benchmark.sql` summary는 `composite_work`, `composite_work_out`, `composite_work_in`을 읽어
  benchmark에서 property prefilter costing handoff를 따로 비교할 수 있다.
- 참고 근거는 `RESEARCH.md`의 ORCA `CPhysicalIndexScan` residual predicate와 Neo4j relationship index seek의
  index-compatible predicate/hidden selection 분리를 기준으로 삼았다.

- VLE composite descriptor에 terminal label slice 기준 `candidate` fanout, prefilter 적용 뒤 `composite` fanout,
  planner lifecycle인 `planned`를 추가했다.
- `EXPLAIN (VERBOSE)`는 기존 `VLE Composite Source`를 더 길게 만들지 않고 `VLE Composite Fanout` line을 별도로
  출력한다. `property-tuples`는 metadata relation 규모로만 남기고, value predicate가 있을 때만
  `planned=property-prefilter`와 축소된 composite fanout을 보여준다.
- `vle_index_probe` expected는 const/runtime-slot predicate에서 `candidate=7 composite=1 planned=property-prefilter`를
  고정해 `age_adjacency` payload prefilter와 terminal label 후보 축소가 같은 descriptor vocabulary로 연결되는지
  확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

- VLE terminal label marker 판정이 parser `vle_internal` argument layout을 stream slot enum과 혼동하던 부분을
  정리했다. label-only marker가 붙은 고정 1-hop VLE terminal node를 `n` label table join으로 다시 풀지 않고,
  hidden raw `edges` target을 통해 terminal property helper로 낮춘다.
- `MATCH (s:N) ... MATCH (s)-[:R*1..1]->(n:N) RETURN n.i` regression은 이제 `N_pkey`로 terminal vertex를
  refetch하지 않고 `age_vle_terminal_vertex_property_from_path(edges, "i")`를 출력한다. 익명 terminal label
  count fixture도 terminal label join 없이 VLE stream 결과만 집계한다.
- 이번 변경은 label candidate pruning과 terminal property projection의 refetch 제거까지 완료했다.
- 같은 terminal marker 단위에서 fallback도 끝까지 정리했다. parser retarget은 label-only marker의 AGTYPE integer
  label id를 terminal-label slot으로 옮기고, executor descriptor는 10-arg marker를 허용한다.
  `MATCH (s:N) ... MATCH (s)-[:R*1..1]->(n:N) RETURN n.i`는 이제 `VLE Arguments: 10`,
  `terminal-property=const, terminal-label=const`, `VLE Composite Source: status=eligible
  reason=terminal-label-property`를 출력한다.
- 아직 composite source metadata는 descriptor/evidence surface다. 다음 후보는 이 eligible descriptor를 실제
  label+typed property composite seek나 adjacency payload pruning request로 소비해 frontier 후보 수를 더 줄이는 것이다.
- terminal-property output descriptor가 알려진 key를 갖고 있으면 CustomScan qual의
  `age_vle_terminal_vertex(edges).key` property access를 VLE output slot 직접 비교로 rewrite한다.
  `MATCH ... (n:N) WHERE n.rare = true RETURN n.rare` regression은 CustomScan `Filter`를
  `edges = true` 형태로 출력해 terminal vertex materialization 없이 predicate가 같은 scalar slot을 소비하는지
  보여준다.
- 이 변경은 filter/materialization handoff를 연결한 단계이며 아직 값 선택도 기반 frontier 축소는 아니다.
  현재 property source 후보 수는 btree source relation 통계라 `rare=true` 같은 value predicate 선택도를 뜻하지
  않는다. 다음 단계는 key+value predicate request를 VLE source policy나 adjacency payload pruning으로 넘길
  value-selective metadata contract를 설계하는 것이다.
- const terminal-property predicate는 `age_adjacency` payload scan prefilter로도 연결했다. fanout을 늘린
  `vle_index_probe` regression은 `VLE Composite Source: ... predicate=const prefilter=eligible threshold=2`와
  `VLE Payload Runtime: ... property-prefilter=1/7/6 tuples=scan:1`을 출력해 7개 terminal label 후보 중
  6개가 property source prefilter에서 제거되고 1개만 VLE traversal 후보로 흘러가는지 보여준다.
- runtime-slot terminal-property predicate도 같은 request 경로를 탄다. `n.rare = s.rare` regression은
  descriptor를 plan 단계 `clauses`에서 다시 materialize해 `predicate=runtime-slot prefilter=eligible`을 출력하고,
  VLE CustomScan `custom_exprs`에 runtime value expr을 싣는다. executor는 iterator 생성 직전에 outer slot의
  value를 평가해 같은 `AgeAdjacencyMatchTerminalPropertyLookup` prefilter에 전달한다.
- 이 과정에서 10-arg terminal-property+label marker를 담는 `AgeVLEInput.args` 배열 경계도 10으로 맞췄다.
  header layout 변경 뒤 stale object가 섞여 memory context corruption이 재현되어 `make clean` 후 재빌드로
  확인했다.
- 추가 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

- `ag_graph_index` property metadata lookup을 parser 전용 static helper에서 공용 함수로 올려 fixed MATCH와 VLE가
  같은 graph index registry를 읽게 했다.
- `AGE VLE Stream` edge-source descriptor에 terminal property source label, provider, type, 후보 count를 싣고,
  verbose EXPLAIN에 `VLE Terminal Property Source`로 출력한다.
- VLE marker enum은 `terminal-property`와 `terminal-label`을 분리했다. 기존 label-only marker와 property-only marker는
  유지하고, property retarget이 label-only marker를 만나면 10-arg marker로 바꿀 수 있는 구조를 만들었다.
- `VLE Composite Source`는 property source metadata가 있는데 terminal label slot이 없으면
  `status=ineligible reason=missing-terminal-label`을 출력한다. 이는 composite seek를 실행하지 말아야 하는
  planner boundary를 raw EXPLAIN에 드러내기 위한 surface다.
- `cypher_vle` fixture는 Neo4j식 `CREATE INDEX n_i_source FOR (n:N) ON (n.i)`를 사용해 VLE terminal-property
  output이 graph metadata source를 보는지 고정한다.
- 이 변경은 아직 property source로 frontier를 직접 줄이지 않는다. 다음 단계의 `label+property` composite source
  request를 위한 planner-visible evidence surface다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: VLE repeated empty threshold feedback

- backend-local VLE source threshold cache가 반복 empty completion을 `root-empty-repeat-observed`와
  `root-empty-repeat-saturated`로 승격하게 했다.
- 첫 saturated feedback은 기존 headroom 35%를 유지하고, 반복 observed feedback은 headroom 30%, 반복 saturated feedback은
  headroom 25%와 확장 batch를 후속 plan에 제공한다.
- planner policy feedback이 `adjacency-empty-lifecycle` threshold input class를 직접 소비하게 해
  `VLE Source Threshold Input`, `VLE Source Policy`, runtime class match vocabulary를 맞췄다.
- `cypher_vle` regression은 같은 empty-cache fixture를 한 번 더 실행해 후속 `EXPLAIN`에
  `observed=2 saturated=1 relaxed=1`, `reason=root-empty-repeat-observed`, `class=adjacency-empty-lifecycle`을
  드러낸다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`
  - `tools/vle_benchmark.sql` smoke (`label_fanout_labels=20`, `label_fanout_edges=8`, `replay_branches=4`,
    `replay_leaves=4`)

## 2026-06-05: VLE threshold feedback direction family

- backend-local VLE source threshold cache lookup에 `out`/`in` directional family fallback을 추가했다.
  이전에는 `active=both` 실행 결과를 `out`/`in` key로 투영했지만, 반대로 directed 실행에서 얻은
  repeated empty completion evidence를 후속 undirected planning이 소비하지 못했다.
- `active=both` request가 exact `both` cache를 찾지 못하면 같은 graph/label/edge-label/consumer의 `out`과
  `in` feedback을 합쳐 endpoint headroom은 더 낮은 값, empty batch는 더 큰 값, source class는 더 강한
  lifecycle class를 사용한다.
- `vle_empty_cache_policy` regression에 후속 undirected `EXPLAIN`을 추가해
  `VLE Source Threshold Input: source=runtime-cache ... direction=out`과
  `VLE Source Policy: ... class=adjacency-empty-lifecycle`을 raw plan surface에 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

## 2026-06-05: VLE benchmark directional family surface

- `tools/vle_benchmark.sql`에 `800-label-fanout-family-path` shape를 추가했다. directed fan-out 실행이 만든
  `out`/`in` threshold feedback을 먼저 남긴 뒤, undirected `EXPLAIN ANALYZE`가 exact `both` cache 없이
  direction family input을 읽는지 확인한다.
- planner summary와 planner/runtime join summary에 `threshold_directional_family` boolean column을 추가했다.
  `active=both` plan이 `threshold_input_source=out|in|mixed`를 소비하면 true가 되어 mixed-direction feedback을
  별도 threshold 보정 대상으로 볼 수 있다.
- 검증:
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE mixed directional family headroom

- exact `both` feedback이 아니라 directed `out`/`in` family feedback을 합친 undirected request는 endpoint
  headroom을 최소 40%로 완화한다. repeated empty lifecycle class와 empty batch는 유지하되, 한쪽 방향의 empty
  completion이 다른 방향의 productive expansion까지 과도하게 누르지 않게 하기 위한 policy boundary다.
- `cypher_vle` regression은 directed exact `out` feedback이 30%를 유지하고, 후속 `active=both` family input만
  `headroom=40 direction=out`으로 출력하는지 고정한다.
- benchmark smoke의 `800-label-fanout-family-path`도 `threshold_directional_family=t`,
  `threshold_input_source=mixed`, `threshold_input_headroom=40`을 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE directional family ratio benchmark

- `tools/vle_benchmark.sql` planner/runtime join summary에 direction-family 전용 계산 column을 추가했다.
  `threshold_directional_family=true`인 shape는 `directional_family_productive_density`,
  `directional_family_empty_completion_ratio`, `directional_family_empty_out_ratio`,
  `directional_family_empty_in_ratio`를 함께 출력한다.
- small smoke의 `800-label-fanout-family-path`는 `threshold_input_source=mixed`, productive density `1.60`,
  empty completion ratio `0.6923`, out/in split `0.8889/0.1111`을 보여줬다. 다음 policy split은 이 값을 기준으로
  mixed family를 방향별 partial policy로 나눌지 판단한다.
- 검증:
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: Property projection reuse summary

- `AGE Property Projection` verbose EXPLAIN에 `Cached Property Summary`를 추가했다.
  summary는 slot 수, 실제 heap properties lookup 수, reused slot 수, final materialization weight 합계,
  heap lookup을 수행하는 slot의 weight 합계, max final weight를 출력한다.
- `MATCH (n:v) RETURN n.i::pg_bigint, n.i::pg_float8` regression은 같은 key path를 두 physical type으로
  출력할 때 `slots=2 heap-lookups=1 reused=1 final-weight=2 heap-final-weight=1`을 raw plan에 고정한다.
  이는 같은 property path의 raw lookup은 한 번만 하고, final materialization만 각 output type으로 나뉜다는
  descriptor contract다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: Aggregate property reuse descriptor

- `CypherArrayAggPropertyHandoff` DEBUG descriptor에 `heap-lookups`, `reused`, `heap-final-weight`를 추가했다.
- aggregate map/list argument vector는 output semantic 때문에 repeated slot을 유지하지만, 같은 container/key source를
  공유하는 slot은 lower raw lookup 관점에서 하나로 볼 수 있게 했다.
- row-scaled aggregate materialization credit은 `reuse-weight`를 `cost-weight`에 더해 repeated source lookup 제거
  효과를 실제 cost 입력으로 소비한다. total final output weight는 semantic 때문에 유지하되, unique heap lookup
  boundary를 별도 credit으로 반영한다.
- slot-vector aggregate serialize format을 v4로 올리고 source group vector를 partial state header에 추가했다.
  transition state는 `AggGetAggref()`의 value argument expression을 비교해 같은 expression을 같은 source group으로
  묶는다. `cypher_match`는 `age_array_agg_list_slots_summary(i, i)`가 `source-groups=1 reused-slots=1`을 출력하는지
  고정한다.

## 2026-06-05: Adjacency composite source descriptor

- fixed MATCH `AGE Adjacency Match` descriptor에 `composite-fanout`을 추가했다.
- `Adjacency Composite Source` EXPLAIN line을 추가해 terminal label slice, property source eligibility, value source,
  planned lifecycle, threshold reason을 별도 surface로 분리했다.
- property-source prefetch가 label constraint와 함께 계획되면 cost candidate width는 terminal label fanout에 더 강한
  composite selectivity를 적용한다. 작은 terminal slice는 `planned=id-cache-small`로 남겨 global property source prefetch를
  피한다.

## 2026-06-05: aggregate benchmark scan signal split

- `tools/aggregate_index_benchmark.sql` summary에서 child relation scan 선택을 `child_uses_index`로, aggregate
  slot-vector rewrite surface를 `aggregate_uses_slot_vector`로 분리했다.
- threshold boundary column도 `last_natural_child_seqscan_rows`, `first_natural_child_index_rows`,
  `first_forced_child_index_rows`로 바꿔 child `Index Scan` threshold와 aggregate index-domain width credit을
  혼동하지 않게 했다.
- `TODO.md`, `RESEARCH.md`, `VLE.md`에 같은 판단을 반영했다. 다음 cost 보정은 child scan boundary를 관찰값으로
  쓰되, 실제 조정 대상이 aggregate payload width credit인지 base scan cost surface인지 먼저 나눠 진행한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-05: aggregate index-domain width credit

- `cypher_array_agg_property_materialization_credit()`의 index-domain credit을 matched slot count만 보지 않고,
  slot당 payload/final/type-vector width weight를 반영하는 `index-width-weight` 기반으로 보정했다.
- single-slot indexed aggregate는 기존보다 강한 row-scaled width credit을 받고, multi-slot aggregate는 matched slot
  수에 비례해 평균 slot width만 소비하므로 일부 index-domain match에서 전체 aggregate weight를 과대 적용하지 않는다.
- planner DEBUG2 handoff descriptor에 `index-width-weight`를 추가해 `index-matched`, payload/final/materialization
  weight와 같은 vocabulary로 비용 입력을 볼 수 있게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: aggregate benchmark slot descriptor summary

- `tools/aggregate_index_benchmark.sql`이 실제 `age_array_agg_slots_descriptor()` result를 shape별로 수집해
  `slot_count`, typed/agtype slot count, payload/final/materialization weight, `slot_types`, `aggregate_rows`를 summary에
  출력하게 했다.
- `threshold_rows=20,100,250` smoke에서 natural child index boundary가 100과 250 사이로 다시 보였고, indexed single-slot
  `agtype`과 typed two-slot `numeric,int8`이 모두 `materialization-weight=4`를 출력하는 것을 확인했다.
- 다음 비용 보정은 단순 materialization weight 상수가 아니라 slot count/type mix와 aggregate input rows가 partial
  aggregate state width에 주는 영향을 기준으로 진행한다.
- 검증:
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100,250 -f tools/aggregate_index_benchmark.sql`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `git diff --check`

## 2026-06-05: aggregate slot serialized width evidence

- `age_array_agg_*_slots_summary`와 `age_array_agg_slots_descriptor()` summary vocabulary에 `serialized-bytes`,
  `null-bitmap-bytes`, `value-bytes`를 추가했다.
- summary byte fields는 실제 serialize layout의 header, type vector, null bitmap, map key bytes, value payload bytes를
  계산한다. descriptor는 rows=0 기준이라 byte fields를 0으로 둔다.
- `cypher_match` parallel regression에서 scalar `agtype` 1-slot, typed 3-slot list/map, typed property 2-slot state의
  byte width가 raw expected output에 드러난다. benchmark parser도 `slot_serialized_bytes`,
  `slot_null_bitmap_bytes`, `slot_value_bytes`를 뽑는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`

## 2026-06-05: aggregate state width cost input

- `CypherArrayAggPropertyHandoff`에 slot type별 estimated payload wire width를 누적하고, null bitmap overhead를 포함한
  `state-width-weight`를 aggregate narrow path cost credit에 반영했다.
- `index-domain` width credit도 materialization weight뿐 아니라 state width weight를 함께 소비한다.
- planner DEBUG2 handoff descriptor에 `wire-width`와 `state-width-weight`를 추가했다. benchmark의 실제
  `slot_value_bytes`와 비교해 estimate 보정 여지를 볼 수 있다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100,250 -f tools/aggregate_index_benchmark.sql`

## 2026-06-05: aggregate width estimate benchmark surface

- `age_array_agg_slots_descriptor()`와 runtime summary aggregate가 planner-compatible
  `estimated-wire-width`, `estimated-state-width-weight`를 출력하게 했다.
- `tools/aggregate_index_benchmark.sql`은 이를 `slot_estimated_wire_width`,
  `slot_estimated_state_width_weight`로 파싱해 실제 `slot_value_bytes` 옆에 둔다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`

## 2026-06-05: aggregate benchmark slot-state ratio

- `tools/aggregate_index_benchmark.sql`이 descriptor row와 별도로 label table direct slot-state summary를 수집한다.
- summary는 `slot_state_value_bytes`, `slot_state_serialized_bytes`, `slot_state_value_bytes_per_row`,
  `slot_value_estimate_ratio`를 출력한다.
- `threshold_rows=20,100` smoke에서 single-slot `agtype`은 28 bytes/row 대 estimate 52, typed `numeric,int8`은
  24 bytes/row 대 estimate 40으로 보였다.
- 검증:
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`

## 2026-06-05: aggregate wire estimate calibration

- benchmark slot-state ratio를 기준으로 planner-compatible wire estimate를 낮췄다.
- `agtype` scalar slot은 28 bytes, `numeric`은 12 bytes, short `text`는 20 bytes를 기본값으로 둔다.
- 같은 estimate를 planner handoff와 SQL descriptor summary 양쪽에 적용했다.
- `threshold_rows=20,100` smoke에서 single-slot `agtype`과 typed `numeric,int8` 모두 `slot_value_estimate_ratio=1.00`으로
  맞춰졌다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`
  - `psql -p 61959 -d postgres -v graph=age_agg_threshold -v threshold_rows=20,100 -f tools/aggregate_index_benchmark.sql`

## 2026-06-05: VLE source explain surface split

- `AGE VLE Stream` verbose EXPLAIN에서 길어진 `VLE Edge Source` 한 줄을 source summary, `VLE Source Cost`,
  `VLE Source Profile`, `VLE Source Threshold Input`, `VLE Source Payload Input`, `VLE Source Policy`로 분리했다.
- planner/runtime feedback cache와 payload replay input은 더 이상 source summary line에 섞이지 않고 각각
  threshold input과 payload input line에서 확인한다.
- `tools/vle_benchmark.sql` parser도 새 property line을 shape별로 join하도록 바꿔 기존 benchmark summary column을
  유지한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match'`

## 2026-06-05: adjacency pruning residual surface

- `AGE Adjacency Match`의 `Adjacency Pruning` EXPLAIN에 `index-solved=N`과 `residual-count=N`을 추가했다.
- ORCA `CPhysicalIndexScan::ResidualPredicateSize()`와 Neo4j relationship index seek의 index-compatible
  predicate/hidden selection 분리를 참고했다.
- label block pruning과 terminal property source prefetch가 source side에서 해결한 predicate 수와, join/heap
  verification으로 남는 predicate 수가 raw regression output에 드러난다.
- `cost_adjacency_match_custom_path()`도 같은 count를 소비해 residual-heavy path의 CPU verification weight와
  source-pruned path의 credit을 분리한다.
- `Adjacency Cost Input` EXPLAIN line을 추가해 fanout, residual/index-solved count, residual weight, index credit,
  planned prefetch threshold/reason, payload mode를 plan surface에서 함께 확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`

## 2026-06-05: aggregate slot state header evidence

- `age_array_agg_map_slots_summary`와 `age_array_agg_list_slots_summary`를 추가해 slot-vector aggregate의
  internal state header를 SQL-visible text summary로 확인할 수 있게 했다.
- summary aggregate는 기존 map/list transition, combine, serialize, deserialize contract를 공유하고 final function만
  summary surface로 바꾼다.
- slot-vector state summary는 `shape`, `slots`, `rows`, `typed`, `agtype`, `payload-weight`, slot별 value type vector를
  출력한다.
- slot-vector state summary에 `final-weight`와 `materialization-weight`를 추가해 runtime aggregate state header가
  planner의 cached-property final materialization weight와 같은 기준을 드러낸다.
- varlena slot payload는 aggregate state에 보관할 때 flat detoast copy로 고정해 partial serialize 경계가 compressed
  toast pointer에 의존하지 않게 했다.
- `cypher_match` regression은 list/map partial aggregate state가 `int8,numeric,text` header를 보존하는지 출력한다.
- 같은 graph property fixture에서 planner가 출력하는 typed property helper vector와
  `age_array_agg_list_slots_summary` runtime state header의 `types=numeric,int8`를 나란히 고정해 cached-property
  descriptor와 aggregate state header mismatch를 raw regression output에서 볼 수 있게 했다.
- transition/combine 단계의 slot type mismatch error도 slot index, phase, expected/actual type name과 OID를
  detail에 포함하도록 바꿔 partial aggregate state header drift를 generic error 없이 좁힐 수 있게 했다.
- `age_array_agg_slots_descriptor(variadic any)`를 추가해 resolved aggregate argument type-vector도 runtime summary와
  같은 vocabulary로 출력하게 했다. `cypher_match`는 descriptor와 runtime state header를 한 query output에 나란히
  고정한다.
- aggregate property handoff formatter를 추가해 planner DEBUG2에 index-matched slot count와 payload/final/materialization
  weight를 출력하게 했다. index-backed aggregate regression은 descriptor call과 `Index Scan` lower target을 같은
  `EXPLAIN VERBOSE` output에 고정한다.
- index-domain matched aggregate slot count를 일반 materialization credit과 분리해 row-scaled width credit으로
  cost에 반영했다.
- `tools/aggregate_index_benchmark.sql`을 추가해 index-backed aggregate descriptor, child `Index Scan`, execution time을
  함께 측정할 수 있게 했다. `rows=20` smoke에서 indexed selective aggregate가 expression index scan을 출력하는 것을
  확인했다.
- benchmark에 natural selective shape와 forced index shape를 분리해 추가했다. `rows=20` smoke에서 natural은 seq scan,
  forced는 expression index scan을 출력해 threshold 보정 기준을 분리했다.
- `rows=100/250/500` smoke를 실행해 natural selective aggregate가 100에서는 seq scan, 250과 500에서는 expression
  index scan을 선택하는 것을 확인했다. benchmark summary에 `row_count`를 추가했다.
- `threshold_rows` 옵션을 추가해 여러 row count graph를 한 번에 만들고 natural/forced/typed aggregate summary를
  row count별로 출력하게 했다. `threshold_rows=20,100` smoke를 통과했다.
- aggregate benchmark의 descriptor expression을 shape별로 받게 해 typed baseline이 `numeric,bigint` descriptor를
  출력하게 했다. `threshold_rows=20,100` smoke로 확인했다.
- threshold sweep summary에 `last_natural_seqscan_rows`, `first_natural_index_rows`, `first_forced_index_rows`를 추가했다.
  `threshold_rows=20,100` smoke를 통과했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: aggregate final materialization cost

- typed collect와 `array_agg` property rewrite의 final materialization credit을 상수 slot count가 아니라
  aggregate input row estimate와 final materialization weight를 함께 보는 cost로 바꿨다.
- 작은 fixture에서는 기존 base weight credit을 유지하고, 큰 input row에서는 `cpu_operator_cost` 기반 row-scaled
  credit이 커지게 했다. 이는 큰 hash aggregate/partial aggregate shape에서 lower typed slot 유지가 더 유리하다는
  planner evidence를 만들기 위한 조정이다.
- ORCA cost model이 aggregate/compute scalar 비용을 child/input cardinality와 결합하는 구조를 참고했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='expr cypher_match age_adjacency cypher_vle'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`
  - `git diff --check`

## 2026-06-05: typed aggregate slot-vector input

- `age_array_agg_list_slots`/`age_array_agg_map_slots` transition state가 value slot을 항상 `agtype` Datum으로
  보관한다는 contract를 명시하고, non-`agtype` scalar input은 transition 시점에 `agtype`으로 정규화하게 했다.
- 기존 combine/serialize/final layout은 유지했다. typed Datum을 state에 직접 저장하지 않으므로 partial aggregate
  serialize/deserial contract를 새 타입 배열로 넓히기 전에도 scalar slot input을 안전하게 받을 수 있다.
- `cypher_match` regression에 `bigint`/`numeric`/`text` typed list slot fixture를 추가했다.
- slot-vector aggregate state header에 slot별 original value type OID 배열을 추가하고 serialize format을 v3로 올렸다.
  state value payload는 계속 `agtype` Datum으로 보관하지만, partial aggregate combine/serialize/deserial이 slot type
  vector를 보존하고 mismatch를 검출한다.
- `cypher_match` parallel regression에 `bigint`/`numeric`/`text` typed table slot aggregate를 추가해 value type header가
  partial aggregate serialize/deserial path를 통과하는지 확인한다.
- typed map/list property `array_agg`는 더 이상 early property aggregate rewrite에서 AGTYPE-only key array로 접지
  않는다. original `array_agg` map/list expression에서 property signature를 읽어 `age_array_agg_*_slots` lower target을
  직접 만들고, slot별 typed field helper를 보존한다.
- `cypher_match` EXPLAIN regression은 `[n.payload.a::pg_numeric, n.payload.b::pg_bigint]`와
  `{a: n.payload.a::pg_numeric, b: n.payload.b::pg_bigint, c: n.payload.c::pg_text}`가
  `age_array_agg_*_slots` 안에서 typed field helper로 내려가는 것을 고정한다.
- slot-vector aggregate payload도 AGTYPE-only 저장에서 typed Datum 저장으로 넓혔다. `int8`/`float8`는 fixed-width
  Datum으로, `numeric`/`text`/`agtype`은 varlena Datum으로 aggregate context에 복사하고, final materializer는 slot별
  value type을 `add_agtype()`에 넘긴다.
- serialize/deserial도 value type별 payload를 보존한다. parallel typed slot regression과 typed list/map result
  regression으로 partial aggregate와 final materialization 경로를 함께 확인한다.
- typed 2-field map shape도 `map2` legacy intermediate aggregate로 접히지 않고 original `array_agg`에서
  `age_array_agg_map_slots` typed helper target으로 내려가게 regression으로 고정했다.
- typed property signature 판정은 공통 helper로 모아 map2/map/list early rewrite skip이 같은 기준을 쓰게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='expr cypher_match age_adjacency cypher_vle'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`
  - `git diff --check`

## 2026-06-05: VLE directional feedback cache projection

- `age_adjacency` directory entry에 endpoint run의 terminal `next_label_id` min/max summary를 추가했다.
  terminal label constraint가 summary range 밖이면 visible payload scan은 main run block을 열지 않고 run 전체를
  label/cache filtered로 처리한다.
- `AGE Adjacency Match`의 `Adjacency Payload Runtime`은 `directory-label=N`을 출력한다. `(:N {i:0})-[:R]->(:Z)`
  regression은 `visible=0 directory-label=3`으로 directory-level pruning evidence를 raw EXPLAIN에 고정한다.
- directory entry layout 변경을 `age_adjacency` v5로 올리고, `age_adjacency_debug_stats()`가 `index_version`을
  반환하게 했다. smoke regression은 fresh index의 `index_version:directory_entries`를 `5:2`로 고정한다.
- `age_adjacency_visible_payload_scan_key_known_empty()`도 directory label summary를 소비하게 했다. no-delta index에서
  endpoint run은 있지만 terminal label range 밖이면 known-empty로 취급한다.
- `age_adjacency_debug_key_known_empty()`를 추가해 label range hit/miss를 `false:true`로 고정했다. VLE frontier
  empty queue는 queue 시점에 payload cache known-empty도 즉시 mark한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`

- VLE runtime threshold feedback cache update를 helper로 분리하고, `active=both` 실행에서
  관측한 `source=both` empty lifecycle evidence를 `out`/`in` directional cache key에도 투영하게 했다.
- 이후 directed traversal은 undirected 실행에서 얻은 root-empty/payload cache evidence를
  `threshold-input=runtime-cache ... source:out|in`으로 소비할 수 있다.
- `cypher_vle`에 undirected 실행 후 directed EXPLAIN이 directional runtime cache를 읽는 fixture를 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

## 2026-06-05: VLE materialized payload replay feedback

- payload replay feedback을 exact consumer class에만 보관하지 않고 `path-materialized`와 `terminal-object`가
  공유하는 materialized family cache에도 기록하게 했다.
- planner는 shared replay ratio를 현재 consumer의 materialization weight에 맞춰 headroom/batch로 다시 해석한다.
  path query에서 관측한 replay가 후속 terminal vertex/properties materialization plan에도 바로
  `payload-input=runtime-cache ... class:adjacency-replay`로 들어간다.
- materialized family cache도 `active=both` evidence를 `out`/`in` key로 투영하게 했고, benchmark summary는
  `payload_input_class`를 별도 column으로 뽑아 shared replay feedback class를 직접 비교할 수 있게 했다.
- benchmark final summary는 현재 split EXPLAIN line 구조에 맞춰 source runtime, plan, counters, payload,
  empty evidence/lifecycle, runtime feedback line을 각각 파싱해 join하도록 고쳤다.
- `cypher_vle` regression에 path replay 실행 뒤 `RETURN n` EXPLAIN이 shared payload feedback을 소비하는
  fixture를 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`
  - `git diff --check`

## 2026-06-05: Adjacency right-label block pruning

- `age_adjacency` main run block의 `next_label_id` metadata를 payload scan contract가 직접 소비하게 했다.
  right terminal label이 맞지 않는 block은 posting을 풀어 cache하기 전에 건너뛰고, delta/fallback path는
  공통 terminal-label helper로 같은 runtime counter를 쌓는다.
- `AGE Adjacency Match`는 `Adjacency Payload Runtime` line을 별도로 출력해 visible payload, label-filtered
  posting, emitted row를 드러낸다. terminal property runtime과 payload pruning runtime을 섞지 않는다.
- cost는 right-label constraint가 있는 adjacency custom path에서 terminal-label pruning selectivity를 반영한다.
- `age_adjacency` regression은 같은 start vertex에서 `N`/`M` terminal label edge를 함께 만들어
  `label-filtered=1`이 raw `EXPLAIN ANALYZE VERBOSE` surface에 보이게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`

## 2026-06-05: Adjacency terminal property prefilter

- metadata-backed terminal property index prefetch set을 executor-local post-filter로만 쓰지 않고
  `age_adjacency` visible payload scan의 vertex filter callback으로 연결했다.
- prefilter 가능한 `property-index-prefetch` 모드에서는 terminal vertex id가 matching set에 없으면 heap visibility
  check와 edge properties fetch 전에 posting을 버린다. 비-prefetch 모드는 기존 id-btree/cache recheck를 유지한다.
- `Adjacency Payload Runtime`은 `property-filtered=N`을 출력해 label pruning과 property prefilter를 분리해 보여준다.
- `age_adjacency` regression은 같은 `N` label이지만 property가 다른 terminal edge를 추가해
  `label-filtered=1 property-filtered=1`을 `EXPLAIN ANALYZE VERBOSE` surface로 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`
- 후속 descriptor 정리에서 `Adjacency Pruning`의 right property mode를 `source-prefetch`로 올리고 residual은
  `join-verify`로 표기하게 했다. cost도 같은 prefetch eligibility helper를 사용해 row pruning과 residual
  recheck cost를 계산한다.
- terminal property value가 constant가 아니어도 `CustomScan.custom_exprs` value slot으로 넘겨 scan 시작 시점에
  평가하게 했다. lookup은 value setter로 cache/prefetch set을 재구성하므로 `n:N {i: 1 + 0}` 같은 runtime
  expression도 source prefilter를 탄다.

## 2026-06-05: array aggregate duplicate property slot sharing

- `array_agg` map/list property narrow path에서 같은 property expression이 반복되면 lower
  target에 같은 slot expression을 여러 번 싣지 않고 기존 `sortgroupref`를 공유하게 했다.
- aggregate argument 순서와 map/list 출력 semantic은 유지하고, base scan/projection boundary의
  반복 property lookup 폭만 줄였다.
- duplicate map/list property EXPLAIN을 `cypher_match` regression에 추가해 lower `Seq Scan`
  output이 중복 property expression을 한 번만 출력하는 것을 raw plan surface로 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: Compact age_adjacency main postings

- `INDEX.md`의 sorted adjacency/trie/compressed graph index 방향에 맞춰 `age_adjacency` payload layout을 v4로
  올렸다.
- bulk-built main posting은 directory run key와 중복되는 endpoint key를 저장하지 않고
  `(heap_tid, edge_id, next_vertex_id)`만 보관한다. directory entry의 key/count가 main run scan boundary를
  제공한다.
- visible payload main page cache도 full posting으로 다시 확장하지 않고 compact main posting payload만 보관한다.
  active key는 emission 직전에 stack-local posting으로 붙여 on-disk compaction과 runtime cache layout을 맞췄다.
- main cache는 page 전체가 아니라 directory entry가 가리키는 active run window만 읽는다. 같은 page에 다른 endpoint
  run이 함께 있어도 현재 key의 offset/count 범위만 cache해 CSR-like run boundary를 executor cache에도 반영했다.
- `age_adjacency_debug_main_probe()`를 추가해 main run window offset, page offset, cached entry 수를
  regression-visible하게 만들었다. window cache가 page 전체 cache보다 좁게 동작하는지 결과 assertion 밖에서 확인한다.
- insert delta posting은 unordered delta page scan을 위해 기존 full key layout을 유지한다. fresh install 전용
  정책에 따라 예전 on-disk layout compatibility decode는 추가하지 않았다.
- delta page의 `min_key/max_key/posting_count` metadata를 payload scan cursor에 연결해, key range 밖 page와
  빈 page는 tuple loop 전에 건너뛴다. v4 main compaction과 달리 on-disk tuple을 다시 바꾸지는 않지만, insert
  delta가 여러 page로 커지는 fresh workload에서 full delta scan 압력을 줄이는 중간 단계다.
- `age_adjacency_debug_delta_probe()`를 추가해 delta page visited/skipped와 실제 tuple scan 수를 확인할 수 있게
  했다. 이 probe는 range skip이 결과 보존을 넘어 scan lifecycle을 줄였는지 regression-visible하게 보여준다.
- planner cost hook은 first key equality qual이 graphid constant이면 같은 delta probe를 사용해 delta tuple CPU
  cost를 `delta_postings` 전체가 아니라 key range가 실제로 scan할 posting 수로 계산한다. linked delta page의
  header 확인 비용은 유지하고 tuple loop 비용만 낮춰 runtime contract와 맞췄다.
- `age_adjacency_debug_delta_maintenance()`를 추가해 delta lifecycle을 `none`, `observe-delta`,
  `range-skip-delta`, `reindex-delta` action으로 노출한다. threshold boolean을 숨은 stats로 두지 않고,
  benchmark와 다음 maintenance policy가 직접 읽을 수 있는 action/reason surface로 올렸다.
- `tools/age_adjacency_baseline.sql`은 index stats에 delta maintenance action/reason과 delta page capacity를
  함께 저장한다. delta-heavy phase 뒤에는 이 action이 `reindex-delta`일 때만 `REINDEX INDEX`를 실행해 benchmark가
  maintenance policy 입력을 실제 실행 흐름으로 소비한다.
- `age_adjacency_reindex_if_needed()`를 추가해 DB 내부 maintenance entry point를 만들었다. 함수는
  `age_adjacency_debug_delta_maintenance()`와 같은 action을 계산하고, `reindex-delta`일 때만 PostgreSQL
  `reindex_index()`를 호출한 뒤 before/after delta posting 수를 반환한다. benchmark harness도 수동 `REINDEX`
  대신 이 entry point를 호출한다.

## 2026-06-05: VLE runtime EXPLAIN summary split

- `VLE Source Runtime` 한 줄에 observed runtime, planned source, class match를 모두 싣던 방식을 줄였다.
  runtime summary는 `dominant/class/pressure/action`만 남기고 planner/index 비교는 `VLE Source Plan`으로
  분리했다.
- `AGE Adjacency Match`는 metadata-backed terminal property index와 constant right property value가 있으면
  endpoint vertex id lookup으로 right property residual을 tuple emission 전에 precheck한다.
- adjacency descriptor에 graph oid와 right property value를 싣고 executor가 graph label relation을 직접 열 수
  있게 했다. SQL join/recheck는 semantic safety를 위해 남기고, EXPLAIN은 `precheck=yes|no`를 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - focused `installcheck REGRESS='age_adjacency cypher_vle cypher_match'`는 expected 갱신 전 출력 diff만 확인했다.

## 2026-06-05: Adjacency terminal property lookup boundary

- `AGE Adjacency Match` executor에서 terminal property relation/index/slot 처리를
  `cypher_adjacency_match_terminal.c`로 분리했다.
- terminal property lookup은 graph oid, label id, property key/value, metadata-backed 여부를 request descriptor로
  받아 초기화한다. scan node 본체는 request 생성과 opaque lookup 호출만 담당한다.
- repeated endpoint를 같은 execution 안에서 다시 확인하지 않도록 `vertex_id -> match` cache를 추가했다.
  EXPLAIN은 기본 property line에 `mode=id-btree-cache`를 남기고, runtime counter는 ANALYZE 전용
  `Adjacency Terminal Runtime` line으로 분리했다.
- property source index oid도 descriptor/request에 싣고, AGTYPEOID expression index일 때는 executor begin 단계에서
  property index를 한 번 스캔해 matching vertex id set을 채운다. 이 경우 EXPLAIN은
  `mode=property-index-prefetch`를 출력한다.

## 2026-06-05: Adjacency Match terminal property index request

- `AGE Adjacency Match`가 terminal node property predicate의 첫 top-level key를 읽고,
  `ag_catalog.ag_graph_index`의 NODE `PROPERTY` metadata와 맞춰 terminal property index 후보를 descriptor에 싣는다.
- `Adjacency Terminal Property` EXPLAIN line을 추가했다. 예시는
  `key=i index=graph-metadata:n_i_source provider=btree metadata=yes` 형태이며, adjacency source/pruning line을 다시
  길게 만들지 않고 terminal property index handoff만 별도로 드러낸다.
- cost는 right-property residual이 있을 때 deferred recheck penalty를 추가하고, metadata-backed property index 후보가
  있으면 그 penalty를 낮춘다. CustomScan 자체가 vertex property lookup을 수행하는 것은 아직 아니므로 row estimate는
  억지로 줄이지 않았다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`

## 2026-06-05: Adjacency Match graph index descriptor

- `AGE Adjacency Match` 후보 등록 API가 graph index metadata detail을 함께 넘기도록 확장했다. parser는
  `ag_catalog.ag_graph_index`에서 `index_kind`, `provider`, `direction`, property count, metadata-backed 여부를
  읽고 planner candidate에 싣는다.
- CustomScan descriptor는 index OID/source뿐 아니라 metadata kind/provider/direction/properties를 보존한다.
  executor EXPLAIN은 `Adjacency Index`, `Adjacency Index Descriptor`, `Adjacency Pruning`으로 나눠 출력한다.
- runtime 비용이 같은 `age_adjacency` AM이면 metadata-backed 여부만으로 가짜 runtime cost discount를 주지 않았다.
  대신 descriptor surface를 넓혀 다음 단계의 property/composite index handoff와 cost 분기가 같은 metadata를
  소비할 수 있게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`

## 2026-06-05: VLE replay lifecycle class 정규화

- ORCA의 physical property/required column handoff를 다시 확인하고, VLE source feedback도 단순 문자열
  class 비교가 아니라 planned lifecycle이 요구한 property를 runtime이 제공했는지로 해석하도록 정리했다.
- `adjacency-replay` runtime feedback은 cache seed 가능한 `age_adjacency` empty lifecycle의 더 강한 provided
  property로 보고, planned `adjacency-cache-seeded` class와 mismatch로 처리하지 않는다.
- `VLE Source Runtime`은 해당 shape에서 `class-match=true`와
  `pressure=adjacency-payload-replay action=keep-payload-replay`를 출력한다. 이는 rollback/tune-source-policy
  후보가 아니라 payload replay contract를 유지해야 하는 신호다.
- 너무 길어진 runtime evidence는 `VLE Source Runtime` summary, `VLE Source Counters`,
  `VLE Payload Runtime`, `VLE Empty Evidence`, `VLE Empty Lifecycle`, `VLE Runtime Feedback`으로 나눴다.
  index/source 선택 판단은 첫 줄에서 보고, counter와 lifecycle 세부값은 별도 줄에서 확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle'`

## 2026-06-05: graph index metadata와 VLE CustomScan child explain

- `AGE Adjacency Match`가 `ag_catalog.ag_graph_index` metadata에서 방향별 `age_adjacency` source index를
  먼저 찾도록 바꿨다. metadata가 없거나 stale이면 기존 relcache scan으로 fallback한다.
- adjacency descriptor와 EXPLAIN에 `source=graph-metadata:<index>` / `source=relcache-scan`을 싣는다.
  parser DDL로 만든 graph index가 실제 planner source로 소비되는지 raw plan surface에서 확인할 수 있다.
- `AGE VLE Stream` CustomPath가 marker `Values Scan` reference path를 `custom_paths`로 보존하고,
  executor가 `custom_plans`를 `custom_ps`로 초기화한다. PostgreSQL `explain.c`의
  `ExplainCustomChildren()` 경로가 그대로 동작해 `Custom Scan (AGE VLE Stream)` 아래에 child plan이 보인다.
- 다른 AGE CustomScan도 확인했다. DML CustomScan은 `lefttree` child를 이미 사용하고, `AGE Property Projection`과
  `AGE Adjacency Match`는 relation-backed scan이라 child plan을 붙일 대상이 아니다. VLE만 marker input path를
  버리고 있던 구조적 누락이었다.
- 전체 regression에서 `cypher_match_plan` graph가 `drop`까지 남던 cleanup 누락을 고쳤고,
  VLE `EXPLAIN ANALYZE`는 source/runtime evidence에 불필요한 buffer hit drift를 피하도록 `BUFFERS OFF`를 명시했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle cypher_match'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency index'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`

## 2026-06-05: VLE payload cache run evidence

- `AGE Property Projection` slot descriptor에도 final materialization weight를 추가했다. typed scalar property
  output은 weight 1, final `agtype` wrapper output은 weight 2로 EXPLAIN의 `Cached Property Slots`에 출력된다.
  `cypher_match` regression은 `MATCH (n:v) RETURN n.i`의 agtype output과 typed bigint/float output을 모두
  plan surface로 고정한다.
- path-materialized/terminal-object strong replay headroom을 terminal-scalar와 분리했다. benchmark replay sweep에서
  `replay_branches=2, replay_leaves=16`의 25% replay부터 path/object/properties는
  `payload-replay-ratio-observed` class를 안정적으로 출력했고, terminal-scalar는 같은 25% replay를
  observed로 유지했다. materialized consumer는 object/path materialization과 label-row fallback 비용을 더 크게
  갖기 때문에 materialization weight를 planner descriptor에 추가했다. path는 weight 3과 strong replay headroom
  18%, terminal-object/properties는 weight 2와 headroom 20%, terminal-scalar는 weight 1과 기존 strong headroom
  25%를 사용한다.
- `tools/vle_benchmark.sql`에 `replay_branches`, `replay_leaves` psql 변수를 추가했다. 기본값 `0`은
  기존처럼 `label_fanout_edges`에서 파생하지만, 큰 dataset 측정에서는 replay fan-in과 replay leaf fan-out을
  fanout benchmark와 독립적으로 조절할 수 있다.
- benchmark final join summary는 `label_fanout_edges`, `replay_branches_input`, `replay_leaves_input`,
  resolved `replay_branches`, `replay_leaves`를 같이 출력한다. 여러 smoke 결과를 저장해도 어떤 replay scale에서
  나온 threshold evidence인지 row 자체로 구분할 수 있다.
- final join summary에 `rows_returned`, `elapsed_ms`도 추가했다. replay percent/headroom/class와 실행 시간,
  cardinality를 같은 row에서 비교해 threshold 조정을 판단한다.
- terminal-scalar replay threshold를 낮은/높은 replay ratio profile로 다시 측정했다. `replay_branches=2,
  replay_leaves=16`의 25% replay는 seed ratio 50%, headroom 35, `payload-replay-observed`로 유지하고,
  `replay_branches=3, replay_leaves=16`의 40% replay는 rows/elapsed가 50% profile에 가까워지므로 strong
  replay로 승격했다. 이에 따라 terminal-scalar strong threshold를 50%에서 40%로 낮췄다.
- `label_fanout_edges=64` large smoke에서 payload replay shape는 replay ratio 95%, seed ratio 3%를 보였고,
  terminal-scalar도 strong replay threshold를 넘었다. 따라서 이번 변경에서는 threshold 상수를 감으로 바꾸지 않고
  replay shape 변수를 열어 낮은/높은 replay ratio를 분리 측정하는 쪽을 우선했다.
- threshold feedback reason을 planner/runtime class vocabulary로 승격했다. 후속 plan이
  `threshold-input=.../reason:root-empty-saturated`를 소비하면 `adjacency-empty-batch` /
  `keep-empty-batch`를 출력하고, observed empty lifecycle은 `adjacency-empty-lifecycle` 후보로 둔다.
  이는 `adjacency-cache-seeded` 안에 empty completion/batch request가 묻히지 않게 하기 위한 정리다.
- runtime feedback 우선순위도 맞췄다. payload replay run이 있으면 missing-vertex 또는 empty lifecycle
  정규화보다 `adjacency-replay`가 우선하고, replay가 없는 planned empty lifecycle evidence만 planner의
  empty lifecycle class와 맞춘다.
- payload feedback을 단순 run count가 아니라 replay/seed ratio가 있는 planner input으로 확장했다.
  `VLE Edge Source`는
  `payload-input=.../replay-percent:N/seed-percent:N/...`를 출력하고, replay 비율이 25% 이상이면
  path/object consumer의 endpoint headroom을 0.25까지 낮춰 seed-only feedback보다 더 공격적으로
  `age_adjacency` lifecycle을 유지한다. terminal-scalar consumer는 같은 25% replay evidence를
  `payload-replay-observed`로 유지하고 headroom 0.35를 사용해 scalar-only output의 endpoint-btree 여지를
  더 남긴다. `vle_payload_replay_policy` 후속 path plan은
  `payload-input=runtime-cache/headroom:25/.../replay-percent:25/seed-percent:50/.../reason:payload-replay-ratio-observed`
  를, 후속 terminal-property plan은
  `payload-input=runtime-cache/headroom:35/.../replay-percent:25/seed-percent:50/.../reason:payload-replay-observed`
  를 고정한다.
- `vle_payload_replay_policy` regression을 추가해 한 실행 안에서 같은 source vertex의 `age_adjacency`
  payload cache replay가 실제 발생하는 shape를 고정했다. 첫 `EXPLAIN ANALYZE`는
  `payload-cache=runs:scan:4/replay:1/seed:2/...`를 출력하고, 후속 `EXPLAIN`은
  `payload-input=runtime-cache/.../replay-runs:1/.../reason:payload-replay-ratio-observed`를 출력한다.
  seed-only feedback보다 강한 replay evidence가 planner input으로 들어오는 contract를 hidden assertion 없이
  plan surface로 검증한다.
- `age_adjacency` payload cache evidence를 source-run 단위와 tuple/event 단위로 분리했다. fresh scan,
  payload replay, cache seed run을 따로 누적하고 기존 scan/replay/seed count와 함께 출력한다.
- `VLE Source Runtime`은
  `payload-cache=runs:scan:N/replay:N/seed:N/tuples:scan:N/replay:N/seeds:N`을 출력한다.
- backend-local source feedback cache가 payload scan/replay/seed run evidence도 보관한다. 다음 planning은
  `payload-input=runtime-cache/headroom:N/scan-runs:N/replay-runs:N/seed-runs:N/replay-percent:N/seed-percent:N/observed:N/reason:...`으로
  입력 여부를 드러내고, payload cache seed evidence는 endpoint headroom을 더 낮추는 후보가 된다.
- `tools/vle_benchmark.sql`은 `payload_*_runs`, `payload_scan_tuples`, `payload_replay_tuples`,
  `payload_seed_events`, `payload_input_*`를 runtime summary와 planner/runtime join summary에 포함한다.
- benchmark harness에 `payload-replay-path`, `payload-replay-terminal`, `payload-replay-vertex`,
  `payload-replay-properties` shape를 추가했다. converging `ReplayStart -> ReplayMid -> ReplayHub ->
  ReplayLeaf` workload로 seed-only feedback과 실제 replay-ratio feedback을 분리해 본다.
- planner가 `payload-input` replay reason을 소비하면 policy class/recommendation도
  `adjacency-replay`/`keep-age-adjacency`로 정규화한다. source는 이미 맞지만 class vocabulary가
  `adjacency-cache-seeded`로 남아 benchmark에서 mismatch로 보이던 상태를 runtime feedback class와 맞췄다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `AgeVLEStreamEdgeSource` descriptor layout 변경 뒤 stale object를 피하기 위해
    `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config` 후 Werror rebuild
    실행
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `63600+` 동적 port에서 materialized replay sweep 실행. path/object/properties는
    25% replay부터 `payload-replay-ratio-observed`를 출력하고, terminal-scalar는 25% observed / 40% strong
    경계를 유지했다.
  - 임시 PostgreSQL instance `63800+` 동적 port에서 `replay_branches=2`, `replay_leaves=16` materialization
    weight smoke 실행. `payload-replay-path`는 weight 3/headroom 18, `payload-replay-vertex`와
    `payload-replay-properties`는 weight 2/headroom 20, `payload-replay-terminal`은 weight 1/headroom 35를
    출력했다.
  - 임시 PostgreSQL instance `62000+` 동적 port에서 small profile `tools/vle_benchmark.sql` smoke 실행
  - 임시 PostgreSQL instance `63300+` 동적 port에서 `replay_branches=3`, `replay_leaves=16` terminal-scalar
    threshold smoke 실행. `payload-replay-terminal`은 replay percent 40, headroom 25,
    `payload-replay-ratio-observed`를 출력했다.

## 2026-06-05: VLE frontier empty batch evidence

- `age_adjacency` frontier known-empty queue flush를 batch lifecycle로 계측했다. runtime stats는 flush 수,
  direction, queued key 수, 최대 flush 폭을 누적한다.
- `VLE Source Runtime`은
  `empty-frontier-batch=flushes:N/out:N/in:N/keys:N/max:N`을 출력한다. 이는 `empty-frontier` mark 수와
  별도로 source completion queue가 얼마나 큰 batch로 flush됐는지 보여준다.
- `tools/vle_benchmark.sql`은 `empty_frontier_batch_*` 컬럼을 runtime summary와 planner/runtime join summary에
  포함한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61973`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE threshold feedback cache state

- backend-local threshold feedback cache가 `observed_count`, `saturated_count`, `relaxed_count`를 보관하게 했다.
  saturated root empty evidence는 batch를 키우고, non-saturated completion evidence는 관측 completion의 2배와
  현재 capacity 사이에서 batch를 다시 낮출 수 있다.
- `VLE Source Runtime`은 `threshold-feedback=.../headroom:N/batch:N/...`을 출력하고, `VLE Edge Source`는
  `threshold-cache=observed:N/saturated:N/relaxed:N`을 출력한다.
- `tools/vle_benchmark.sql`은 `threshold_feedback_batch`와 `threshold_cache_*` 컬럼을 planner/runtime summary에
  포함한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61972`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE saturated feedback batch sizing

- backend-local threshold feedback cache가 endpoint headroom뿐 아니라 다음 `empty-batch` 후보도 보관하게 했다.
  root empty completion이 batch capacity를 포화하면 다음 planning은 batch를 최소 현재 capacity의 2배 또는
  관측 completion 수까지 키운다.
- `VLE Edge Source`의 `threshold-input` surface를
  `threshold-input=none|runtime-cache/headroom:N/batch:N/source:.../reason:...`으로 확장했다. planner가 읽은
  batch 후보는 `AgeVLEStreamEdgeSource` descriptor를 거쳐 executor의 empty lifecycle capacity가 된다.
- `vle_empty_cache_policy` regression fixture를 8-way fan-out으로 넓혀 `threshold-feedback=...reason:root-empty-saturated`
  뒤 후속 plan이 `empty-batch=eligible/size:16`과
  `threshold-input=runtime-cache/headroom:35/batch:16/source:out/reason:root-empty-saturated`를 출력하도록 고정했다.
- `tools/vle_benchmark.sql`은 `threshold_input_batch`를 planner summary와 planner/runtime join summary에 포함한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61971`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE threshold feedback planner cache

- runtime `threshold-feedback`을 `graph,label,edge-label-oid,consumer-class,active-direction` 키의 backend-local
  feedback cache에 기록하고, 다음 planning의 `VLESourcePolicyProfile`이 이를 endpoint headroom 입력으로
  소비하도록 연결했다.
- `AgeVLEStreamEdgeSource` descriptor에 `threshold_input_*` typed field를 추가했다. `VLE Edge Source`는
  `threshold-input=none|runtime-cache/headroom:N/batch:N/source:.../reason:...`을 출력하므로 raw EXPLAIN에서
  runtime feedback이 planner input으로 들어왔는지 확인할 수 있다.
- `vle_empty_cache_policy` regression은 `EXPLAIN ANALYZE` 뒤 같은 shape의 `EXPLAIN`을 실행해
  `threshold-input=runtime-cache/headroom:50/batch:8/source:out/reason:root-empty-observed`를 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61971`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle descriptor

- `VLESourceRuntimeThresholdFeedback`을 추가해 root/source empty completion summary를 다음 source policy input
  후보로 해석한다. `root-empty`가 saturated이면 runtime feedback은 endpoint headroom 후보를 0.35로 낮추고,
  saturated가 아니면 현재 planned headroom을 유지한 observed feedback으로 남긴다.
- `VLE Source Runtime`은 `threshold-feedback=eligible|ineligible/headroom:N/source:out|in|both|none/reason:...`
  을 출력한다. 이는 아직 persistent cache가 아니라, benchmark와 raw EXPLAIN에서 어떤 shape가 threshold 보정
  입력이 될 수 있는지 보여주는 typed feedback surface다.
- `tools/vle_benchmark.sql`은 `threshold_feedback`, `threshold_feedback_headroom`,
  `threshold_feedback_source`, `threshold_feedback_reason`을 runtime summary와 planner/runtime join summary에
  포함한다.
- repeated empty source completion을 `VLETraversalEmptyCompletionSummary` root descriptor로 올렸다.
  source skip/cache/frontier/run completion은 root summary와 source stats snapshot을 함께 갱신하고,
  `AgeVLEStreamScanState` accumulator가 이 필드를 누적한다.
- `VLE Source Runtime`은 `root-empty=completion:N/out:N/in:N/batch:N/saturated-roots:N`을 출력한다.
  `empty-summary`가 전체 source counter 합계라면, `root-empty`는 planned lifecycle batch capacity와 root/source
  completion 방향성을 같이 보여주는 feedback surface다.
- `tools/vle_benchmark.sql`은 `root_empty_completion_count`, `root_empty_completion_out`,
  `root_empty_completion_in`, `root_empty_batch_capacity`, `root_empty_saturated_count`를 runtime summary와
  planner/runtime join summary에 포함한다.
- header struct 변경 뒤 stale object로 `AGE VLE stream multi-call context` chunk overwrite가 발생해
  `make clean` 뒤 Werror rebuild로 ABI 크기 불일치를 제거했다.

- `empty-batch` size가 강한 repeated completion 후보이면 endpoint headroom을 0.50에서 0.35로 낮춘다.
  작은 synthetic fixture에서는 source가 이미 `age_adjacency`여도, 큰 dataset에서는 endpoint-btree 유지 여지를
  더 줄이는 threshold 보정이다.
- `VLE Source Runtime`에 `empty-summary=completion:N/batch:N/saturated:true|false`를 추가해
  frontier/run/cache/complete evidence가 planned batch capacity를 채웠는지 raw EXPLAIN으로 확인한다.
- runtime action은 batch가 포화된 `empty-run`/`empty-frontier` evidence에서
  `keep-empty-run-batch:*` 또는 `keep-empty-frontier-batch:*`를 출력한다.
- `tools/vle_benchmark.sql`은 `empty_completion_count`, `empty_summary_batch`,
  `empty_batch_saturated`를 planner/runtime join summary에 포함한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61968`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle batch descriptor

- planned empty lifecycle에서 repeated empty completion을 기대할 수 있으면 `empty_lifecycle_batch_size`를
  planner descriptor에 추가한다.
- `AgeVLEInput`, context apply, cached refresh, local context, source stats가 같은 batch size를 전달하고
  `VLE Source Runtime`은 `empty-batch=eligible|ineligible/size:N/capacity:N match=true|false`를 출력한다.
- `age_adjacency` frontier known-empty queue는 planned batch capacity를 초기 allocation에 사용해, 큰 fan-out에서
  반복 empty completion을 작은 array growth가 아니라 lifecycle batch 후보로 처리한다.
- `tools/vle_benchmark.sql`은 planner/runtime join summary에서 `empty_batch`, `empty_batch_size`,
  `empty_batch_capacity`, `runtime_empty_batch_match`를 출력한다.
- `AgeVLEInput`과 source stats layout 변경 뒤 incremental build에서는 stale object로 SIGSEGV가 날 수 있으므로,
  이 종류의 변경은 clean rebuild로 ABI 경계를 맞춘 뒤 판단한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61967`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle context handoff

- empty lifecycle descriptor를 `AgeVLEInput`, context apply, cached refresh, local context,
  source stats까지 관통시켰다.
- cached context reuse가 planner의 empty lifecycle policy를 잃지 않도록
  `VLEContextRefreshInput`에도 같은 typed field를 싣고, refresh 시점의 context run을 runtime
  source stats에 기록한다.
- `VLE Source Runtime`은 `empty-context=eligible|ineligible/depth:N/runs:N match=true|false`를 출력해
  raw EXPLAIN만으로 plan descriptor와 executor-local/cached context handoff가 같은지 확인한다.
- `tools/vle_benchmark.sql` planner/runtime join summary도 `empty_context`, `empty_context_depth`,
  `empty_context_match`, `runtime_empty_context_match`를 출력한다.
- 이 변경은 `AgeVLEInput`과 `AgeVLESourceStats`의 cross-module struct layout을 바꾸므로 incremental object가
  섞이면 allocation corruption처럼 보일 수 있다. 실패 시 되돌리지 않고 clean rebuild로 ABI 경계를 맞춘다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61966`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle planner descriptor

- planner source cost decision에 `empty_lifecycle_eligible`와 `empty_lifecycle_depth`를 추가했다.
- `AGE VLE Stream`의 `AgeVLEStreamEdgeSource` descriptor가 empty source lifecycle eligibility를
  typed field로 싣고, executor/runtime feedback이 이 field를 직접 읽는다.
- `VLE Edge Source`는 `empty-lifecycle=eligible|ineligible/depth:N`, `VLE Source Runtime`은
  `empty-plan=eligible|ineligible/depth:N`을 출력한다.
- planned `adjacency-cache-seeded` source와 runtime empty suppression/frontier/run evidence를 비교할 때
  EXPLAIN 문자열을 역파싱하지 않고 descriptor field로 판단한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-05: VLE empty lifecycle benchmark join

- `tools/vle_benchmark.sql` planner summary가 `empty_lifecycle`, `empty_lifecycle_depth`를 출력한다.
- runtime summary와 planner/runtime join summary가 `empty_plan`, `empty_plan_depth`,
  `empty_plan_match`, `empty_plan_depth_match`, `runtime_empty_plan_match`를 출력한다.
- 80-label/8-edge fan-out smoke에서 right/left terminal/path/object shape 모두 planned empty lifecycle과
  runtime empty plan이 일치했고, `suppression-match=true`, `class_match=true`,
  `empty_plan_match=true`가 함께 출력됐다.
- 검증:
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61961`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE runtime empty plan match

- `VLE Source Runtime`에 `empty-plan=... match=true|false`를 추가했다.
- runtime empty source evidence가 없으면 mismatch로 보지 않고, `age_adjacency` empty probe/suppression/cache/
  frontier/run evidence가 있으면 planned empty lifecycle eligibility가 있는지 직접 비교한다.
- `cypher_vle` expected output은 raw EXPLAIN에서 `empty-plan match=true`를 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61961`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE expansion source-run empty precheck

- missing-vertex 전용으로만 쓰던 known-empty source-run precheck를 일반 expansion source cursor에도 적용했다.
- `age_vle_context_expansion_source_cursor_known_empty()`가 planned source cursor의 payload cache `known_empty`
  state를 확인하고, source object를 만들기 전에 `empty-run` skip으로 기록한다.
- known-empty로 완료된 source direction은 used source로 기록해 packed fallback이 같은 방향을 다시 열지 않게 했다.
- 80-label/8-edge fan-out smoke에서 right fanout terminal/path/vertex/properties shape의
  `empty-cache=age_adjacency:8/out:8/in:0`이 `empty-run=age_adjacency:8/out:8/in:0`으로 올라갔다.
  left fanout은 frontier hint가 없어 기존 `empty-suppressed=age_adjacency:8/out:0/in:8`을 유지했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61962`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE reverse empty directory probe

- `age_adjacency_visible_payload_scan_key_known_empty()`가 delta가 없는 index에서 directory cache range 밖의 key도
  directory lookup으로 확인해 negative property를 제공하게 했다.
- active payload scan의 main cursor와 directory cache는 분리되어 있으므로, frontier hint 중 directory cache가
  다른 page로 이동해도 현재 source payload scan을 유지한다.
- 80-label/8-edge fan-out smoke에서 left/reverse terminal/path shape도
  `empty-suppressed=age_adjacency:8/out:0/in:8` 대신
  `empty-frontier=age_adjacency:8/out:0/in:8`과 `empty-run=age_adjacency:8/out:0/in:8`을 출력했다.
- `cypher_vle` expected output은 broader directory negative property로 생긴
  `empty-frontier`/`empty-run` evidence를 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61963`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle pressure vocabulary

- `VLE Source Runtime`에 `empty-evidence=none|empty-scan|endpoint-empty-scan|empty-complete|empty-cache|empty-frontier|empty-run`을
  추가했다.
- planned empty lifecycle과 runtime evidence가 일치하면 generic `adjacency-empty-suppressed` pressure로 남기지
  않고, `adjacency-empty-frontier` 또는 `adjacency-empty-run`처럼 실제 제공된 lifecycle 단계로 분리한다.
- `tools/vle_benchmark.sql` runtime summary와 planner/runtime join summary도 `empty_evidence`를 추출한다.
- `cypher_vle` expected output은 raw EXPLAIN에서 planned lifecycle match와 `empty-evidence`, pressure/action
  vocabulary를 함께 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61964`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty lifecycle source threshold

- cache-seed 가능한 multi-step `age_adjacency` source의 endpoint 유지 headroom을 generic cache seed 75%에서
  planned empty lifecycle 전용 50%로 분리했다.
- `VLE Edge Source`는 empty lifecycle eligible source에서 `endpoint-headroom=0.50`을 출력한다.
- endpoint work가 전체 budget 안에 있지만 empty lifecycle headroom을 넘는 경우 policy reason은
  `empty-lifecycle-headroom`으로 출력된다. 이는 source enum rollback이 아니라 repeated empty completion을
  기대하는 root/source lifecycle threshold 보정이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin` temp instance `61965`에서
    `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty source cache lifecycle

- `age_adjacency` payload cache entry에 `known_empty` state를 추가했다.
- payload cache replay와 known-empty hit은 payload scan cursor를 열기 전에 판단한다.
- activation cleanup은 adjacency scan cursor와 output scratch만 정리하고, payload cache는 cached VLE context의
  전체 cleanup까지 유지한다. cache storage는 activation `multi_call_context`가 아니라 explicit free 대상 context에
  둬 reset 뒤 dangling pointer가 남지 않게 했다.
- `VLE Source Runtime`과 benchmark summary에 `empty-cache=age_adjacency:N/out:N/in:N`을 추가했다.
- `vle_empty_cache_policy` regression fixture는 converging path에서 같은 terminal source가 같은 activation 안에서
  반복 확장될 때 `empty-cache=age_adjacency:1/out:1/in:0`이 출력되는 plan evidence를 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE suppressed source feedback surface

- `VLE Source Runtime` explain line에 `suppressed-source=out:.../in:...`와
  `suppression-match=true|false`를 추가했다.
- suppressed empty source가 planner/executor가 고른 directed source와 맞는지 문자열 재파싱 없이
  runtime feedback에서 계산한다.
- `tools/vle_benchmark.sql` runtime summary와 planner/runtime join summary가 `suppressed_source`,
  `suppression_match`를 추출한다.
- small fan-out smoke에서 right fan-out은 `suppressed-source=out:age-adjacency/in:none`,
  left fan-out은 `suppressed-source=out:none/in:age-adjacency`로 출력되고 모두
  `suppression-match=true`였다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `git diff --check`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE empty adjacency source suppression

- `age_adjacency_visible_payload_scan_begin_key()`가 false인 source vertex는 payload scan을 열지 않고
  selected empty source completion으로 처리한다.
- `age_adjacency_empty_source_skips` counter를 추가하고 `VLE Source Runtime`과 benchmark summary에
  `empty-suppressed=age_adjacency:N`을 출력한다.
- suppressed empty source counter를 outgoing/incoming 방향별로 나누고,
  `action=observe-suppression:out|in|both`를 출력한다.
- missing-vertex fallback에서는 suppressed empty source를 failure가 아니라 handled source로 보아 packed/global
  fallback이 잘못 활성화되지 않게 했다.
- 800-label fan-out smoke에서 `age_adjacency` scan이 9회에서 1회로 줄고, empty scan 8회가
  `empty-suppressed=8`, `pressure=adjacency-empty-suppressed`로 이동했다. right fan-out은 `out:8/in:0`,
  left fan-out은 `out:0/in:8`로 summary에 분리된다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `tools/vle_benchmark.sql` smoke profile: `run_standard_cases=0`, `800-label-fanout-*`

## 2026-06-05: VLE runtime pressure 출력 추가

- `VLE Source Runtime` explain line에 `pressure=... action=...`을 추가했다.
- source mismatch, class mismatch, materialized tie, cache seed miss, adjacency density low, endpoint fanout을
  runtime feedback class와 별도 pressure로 분류한다.
- `tools/vle_benchmark.sql` runtime summary와 planner/runtime join summary가 `runtime_pressure`,
  `runtime_action`을 추출한다.
- fanout small benchmark smoke에서 `800-label-fanout-*` shape들이 `adjacency-density-low` /
  `check-fallback-suppression`을 출력해 다음 후보가 source selection보다 age_adjacency scan density와 fallback
  suppression 쪽임을 드러냈다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-05: VLE materialized tie source policy 보정

- endpoint work가 fanout budget과 같은 tie일 때 consumer class를 반영한다.
- `terminal-scalar` depth 1 tie는 endpoint-btree를 유지하지만, `path-materialized`와 `terminal-object` tie는
  final object/path materialization 비용을 반영해 `age_adjacency`를 선호한다.
- `adjacency-materialized-tie` class와 `prefer-age-adjacency-materialization` recommendation을 추가해 depth/cache
  seed 근거와 materialization tie 근거를 분리했다.
- `cypher_vle` regression에 `vle_tie_policy` fixture를 추가해 같은 fanout 1 incoming traversal에서 scalar는
  endpoint-btree, path/terminal vertex는 `age_adjacency`를 고르는 plan evidence를 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE source policy profile descriptor화

- source policy consumer, consumer class, active direction, fanout budget을 `AgeVLEStreamEdgeSource`
  descriptor field로 추가했다.
- `VLE Edge Source` explain은 이 field를 `profile=consumer:.../class:.../active:.../budget:...`로 출력하고,
  `policy=` text는 directed source, depth/work, reason, class/recommendation 중심으로 줄였다.
- `tools/vle_benchmark.sql` planner summary와 planner/runtime join summary는 `policy=` 내부가 아니라
  `profile=` surface에서 consumer/active/budget을 추출한다.
- header layout 변경 뒤 stale `cypher_vle_stream.o`가 old struct offset을 읽어 `planned-class=unknown`과
  `endpoint-headroom=0.00`을 출력했고, 관련 object를 재컴파일해 descriptor handoff를 확인했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE materialization source explain 노출

- `AGE VLE Stream`의 `VLE Materialization` explain line에 `object-source` 또는 `vertex-source`를 추가했다.
- path/terminal vertex output은 `object-source=global-metadata|label-row-fallback`, terminal properties output은
  `vertex-source=global-metadata|label-row-fallback`을 출력한다.
- local edge-state source 뒤 label relation row fallback으로 final object/properties를 만드는 contract를
  `cypher_vle` expected에 직접 남겨 hidden assertion 대신 raw plan surface로 검증한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance에서 fanout small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE terminal consumer source budget 분리

- source policy consumer class를 `terminal-scalar`, `terminal-object`, `path-materialized`로 세분화했다.
- `terminal-property` direct scalar output은 기존 fanout 2 endpoint-btree budget을 유지한다.
- `terminal-vertex`와 `terminal-properties`는 final vertex/properties materialization이 필요하므로 fanout 1 budget을
  사용한다.
- `cypher_vle` regression에 fanout 2, depth 1 terminal vertex/properties explain fixture를 추가했다.
- `tools/vle_benchmark.sql` 기본 workload에 `800-label-fanout-vertex`와
  `800-label-fanout-properties`를 추가해 terminal scalar/object/path consumer를 같이 비교한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE runtime feedback planned-class 출력

- `VLE Source Runtime` explain line에 `planned-class`, `class-match`, `planned-recommendation`을 추가했다.
- planner policy text의 stable `class=... recommendation=...` token을 runtime feedback formatter에서 다시 읽어,
  raw `EXPLAIN ANALYZE`만으로 planner feedback class와 runtime feedback class가 맞는지 확인할 수 있게 했다.
- `tools/vle_benchmark.sql` runtime summary와 planner/runtime join summary도 `planned_class`,
  `runtime_class_match`, `planned_recommendation`을 추출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE source policy profile 추가

- source policy 입력을 `VLESourcePolicyProfile`로 묶어 output requirement, consumer class, fanout budget,
  finite depth, cost eligibility, cache seed eligibility를 같은 request로 다룬다.
- `VLE Edge Source` policy text에 `cache-seed=eligible|ineligible`을 추가했다. eligible은 finite multi-step이고
  `age_adjacency` 후보가 있을 때만 표시한다.
- `tools/vle_benchmark.sql` planner summary와 planner/runtime join summary도 `cache_seed`를 추출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE cache seed headroom threshold 적용

- cache seed 가능한 multi-step `age_adjacency` 후보에서는 endpoint-btree 유지에 75% endpoint work headroom을
  요구하도록 source policy를 조정했다.
- endpoint work가 기존 limit 안이지만 75% headroom을 넘으면 `cache-seed-headroom` reason으로
  `age_adjacency`를 선택한다.
- `VLE Edge Source`와 benchmark summary에 `endpoint-headroom=0.75`를 출력한다.
- `cypher_vle` regression에 `vle_headroom_policy` fixture를 추가해 `reason=out:cache-seed-headroom` plan
  evidence를 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE source policy feedback descriptor화

- `AGE VLE Stream` edge-source descriptor에 planner policy class, recommendation, cache seed eligibility,
  endpoint headroom percent를 typed field로 추가했다.
- `VLE Source Runtime` planned-class/planned-recommendation 출력은 더 이상 `VLE Edge Source` policy text를
  역파싱하지 않고 descriptor field를 읽는다.
- `cost_policy` 문자열은 explain/benchmark surface로 유지하되 executor feedback contract에서는 분리했다.
- header layout 변경 뒤 stale `cypher_vle_stream.o`가 old struct offset을 읽어 `planned-class=unknown`이
  출력되는 regression diff가 났고, CustomScan executor object까지 재컴파일해 descriptor field handoff를 확인했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE source policy active direction 적용

- `VLESourcePolicyProfile`에 active direction을 추가해 `right`, `left`, undirected VLE가 실제 사용하는 방향만
  source policy threshold 대상으로 삼게 했다.
- directed VLE의 inactive side는 `none` source와 `inactive-direction` reason으로 출력한다. runtime planned source도
  inactive side를 `none`으로 보여 benchmark `source_match`와 `class_match`가 실제 traversal 방향만 비교한다.
- `cypher_vle` regression에 left-direction fixture를 추가해 `active=in` plan evidence도 고정했다.
- `tools/vle_benchmark.sql` planner summary와 planner/runtime join summary에 `active_direction`을 추가했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE benchmark active source join 보강

- `tools/vle_benchmark.sql`이 planner policy와 runtime planned source를 active direction 기준으로 분해해
  `active_planner_source`, `active_planned_source`를 출력하도록 했다.
- `source_match`는 더 이상 planner policy 문자열 전체에 runtime dominant source가 포함되는지만 보지 않고, `out`,
  `in`, `both` active direction에 맞는 source를 비교한다.
- 기본 fan-out benchmark에 `800-label-fanout-left-terminal`, `800-label-fanout-left-path`를 추가해 `active=in`
  workload도 planner/runtime join summary에 들어가게 했다.
- 검증:
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE terminal vertex label-row fallback 적용

- standard+fanout benchmark에서 `800-label-fanout-vertex` `EXPLAIN ANALYZE RETURN n`이 local-source terminal
  vertex object를 만들 때 global vertex metadata miss로 `build_vle_vertex_value()` assert에 걸리는 것을 확인했다.
- `build_vle_vertex_value()`가 global metadata miss 시 graphid label id로 label cache를 찾고, vertex label relation의
  id index/table scan으로 properties row를 읽어 vertex value를 만들도록 확장했다.
- runtime feedback은 `cache_seed_eligible=false`인 shape에서 payload seed/replay counter만으로
  `adjacency-cache-seeded` class를 출력하지 않고 planner descriptor의 class/recommendation을 유지한다.
- `cypher_vle` regression에 terminal-vertex `EXPLAIN ANALYZE` fixture를 추가해 local-source terminal object
  materialization과 `class-match=true` evidence를 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - standard+fanout small profile `tools/vle_benchmark.sql` smoke 실행
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE runtime source planned-match 출력

- `VLE Source Runtime` explain line에 `planned=out:.../in:... source-match=...`를 추가했다.
- runtime dominant source가 planner fixed-source descriptor의 outgoing/incoming source family 중 하나와 맞는지
  executor explain이 직접 출력한다.
- `tools/vle_benchmark.sql` runtime summary와 planner/runtime join summary도 `planned_source`,
  `runtime_source_match`를 추출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: cached property slot descriptor에 handoff signature 보관

- `CypherCachedPropertySlotDescriptor`가 `CypherPropertyHandoffDescriptor`를 함께 보관하도록 확장했다.
- ordered property projection delay와 index canonicalization은 raw field 조합 대신 cached slot descriptor helper로
  key source와 physical signature를 비교한다.
- header layout 변경 후 stale object file이 `cypher_paths.c`에서 old offset으로 slot을 읽어 crash가 났고,
  `lldb`로 `add_property_projection_custom_path()`의 `first_slot->container` 접근을 확인했다. `make clean` 뒤
  전체 rebuild로 종속 object를 재생성했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: property index canonical entry descriptor화

- partial predicate/restriction canonicalization에서 raw `Node *` 목록 대신 cached property slot descriptor와
  catalog expression surface를 함께 보관한다.
- nested typed property index에서 rebuilt helper chain이 catalog expression surface를 바꾸면 index scan이
  seq scan으로 떨어질 수 있음을 확인했고, canonical entry는 catalog에 저장된 expression을 보존하도록 조정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match index expr'`

## 2026-06-04: VLE source policy consumer evidence 추가

- `VLEStreamSourceCostInput`에 `AgeVLEOutputRequirement`를 포함해 planner source policy가 어떤 consumer
  requirement에 대한 결정인지 보존한다.
- `AGE VLE Stream`의 `VLE Edge Source` policy text와 `tools/vle_benchmark.sql` planner summary에
  `consumer=... consumer-class=...`를 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE source policy combined work 추가

- `VLEStreamSourceCostInput`에 traversal direction을 포함하고, undirected source policy에서 start/end fanout을
  합친 combined endpoint work를 계산한다.
- planner source policy가 local-index source를 고르면 runtime local edge-state도 graph metadata load 여부보다
  planner descriptor를 우선해 같은 source contract를 따른다.
- path materialization shape는 local edge-state source로 억지 전환하지 않는다. edge object materialization은
  아직 global edge metadata contract가 필요하므로 terminal/local-safe shape에서만 local source policy를 적용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE planner source policy feedback 연결

- `VLE Edge Source` planner policy text에 `class=... recommendation=...`을 추가했다.
- endpoint-btree 유지 plan은 `endpoint-direct/keep-endpoint-btree`, multi-step tie는
  `adjacency-work-tie/prefer-age-adjacency-depth`, work 초과는 `adjacency-work/keep-age-adjacency`,
  fanout 통계 부재는 `unknown-fanout/collect-endpoint-stats`로 드러난다.
- `tools/vle_benchmark.sql`은 runtime feedback summary뿐 아니라 planner policy/reason/class/recommendation
  summary도 추출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE runtime source feedback 분류 추가

- `VLE Source Runtime` feedback에 `class=...`와 `recommendation=...`을 추가했다.
- endpoint-btree 직접 probe는 `endpoint-direct/keep-endpoint-btree`, `age_adjacency` payload cache seed가
  발생한 multi-step source는 `adjacency-cache-seeded/prefer-age-adjacency-depth`로 드러난다.
- source별 scan density, yield/replay/push 숫자와 함께 실행 source의 threshold 보정 방향을 regression-visible하게
  만들었다.
- `tools/vle_benchmark.sql`은 `VLE Source Runtime` line에서 dominant source, feedback class, recommendation을
  별도 summary row로 추출한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE source fanout evidence와 tie policy 조정

- `VLESourceFanoutEvidence`에 relation tuple/fanout 통계가 실제로 알려졌는지 나타내는 flag를 추가했다.
  `AGE VLE Stream` verbose explain은 `stats=rel:.../start:.../end:...`를 출력해 fanout 0과 통계 부재를
  구분한다.
- endpoint-btree/`age_adjacency` source 선택에서 fanout 값이 0으로 보인다는 이유만으로 endpoint work를
  신뢰하지 않고, endpoint fanout 통계가 알려진 경우에만 endpoint work threshold를 적용한다.
- multi-step VLE에서 endpoint work가 threshold와 같은 tie이면 `age_adjacency`를 선택하도록 조정했다.
  단일 step tie는 endpoint-btree를 유지하고, `*1..2` 이상 tie는 payload replay 가능성을 우선한다.
- cached VLE context refresh input에 planner source policy를 포함해 refresh root descriptor가 현재
  CustomScan descriptor의 source policy를 다시 읽도록 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE source policy reason 노출

- `VLE Edge Source` policy text에 방향별 선택 사유를 `reason=out:.../in:...` 형태로 추가했다.
- endpoint-btree 유지 사유는 `endpoint-work`, `age_adjacency` 전환 사유는 `work-exceeds-limit`로 드러나며,
  source availability/fanout fallback은 `endpoint-only`, `unknown-fanout`, `adjacency-only`, `no-source`,
  `layout`으로 구분된다.
- `vle_adjacency_only_policy` regression fixture를 추가해 edge relation fanout statistics가 없는 cold policy에서
  `reason=out:unknown-fanout/in:unknown-fanout`이 visible explain에 남도록 했다.
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

## 2026-06-04: VLE planner/runtime feedback class 정렬

- multi-step VLE에서 planner가 `age_adjacency`를 선택하면 planner policy class를
  `adjacency-cache-seeded`로 출력하도록 조정했다.
- `reason=out:.../in:...`은 기존 work-tie/work-exceeds-limit 정보를 유지하고, `class`는 runtime feedback과 같은
  vocabulary로 맞춘다.
- small benchmark smoke에서 terminal/path fan-out shape 모두 `source_match=true`, `class_match=true`를 확인했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - 임시 PostgreSQL instance `61959`에서 small profile `tools/vle_benchmark.sql` smoke 실행

## 2026-06-04: VLE benchmark planner/runtime 비교 확장

- `tools/vle_benchmark.sql` 기본 workload에 `800-label-fanout-path`를 추가해 terminal-property consumer와
  path-materialized consumer를 같은 fan-out graph에서 비교한다.
- planner summary가 `budget=fanout:N`을 추출하도록 확장했다.
- benchmark 끝에 planner policy와 runtime feedback을 shape별로 join하는 summary를 추가했다. 이 summary는
  consumer class, fanout budget, planner policy, runtime dominant source, `source_match`, `class_match`,
  source별 density를 출력한다.
- 검증:
  - 임시 PostgreSQL instance `61959`에서 small profile smoke 실행:
    `psql -p 61959 -d postgres -v graph=age_vle_bench_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v run_standard_cases=0 -f tools/vle_benchmark.sql`

## 2026-06-04: VLE consumer별 source budget

- `VLEStreamSourceCostDecision`이 output requirement별 endpoint-btree fanout budget을 사용하도록 바꿨다.
- terminal-only consumer는 기존 fanout 2 budget을 유지하고, path-materialized consumer는 fanout 1 budget으로
  낮춰 path output에서 `age_adjacency` 후보를 더 빨리 선택하게 했다.
- `AGE VLE Stream` explain policy text에 `budget=fanout:N`을 출력한다.
- `cypher_vle` regression에 fanout 2, depth 1 path output fixture를 추가해 terminal-only는 endpoint-btree를
  유지하고 path-materialized는 `age_adjacency`로 전환하는 차이를 expected에 남겼다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

## 2026-06-04: VLE materialization fallback source 확장

- path/list materialization consumer가 local edge-state source를 사용할 수 있도록 vertex/edge object builder의
  global metadata 의존을 분리했다.
- global graph context에 vertex/edge object metadata가 없으면 graphid label id로 label relation을 찾고, id btree
  index 또는 table scan으로 row를 읽어 object를 materialize한다.
- planner/runtime local-index gate에서 path 출력과 cached grammar 제한을 제거했다. property constraint가 없는
  label-constrained VLE는 consumer requirement와 무관하게 local-index 후보가 될 수 있다.
- `cypher_vle` expected는 path consumer explain이 `local-index-candidate` source policy를 직접 출력하도록 갱신했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`

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

## 2026-06-05: VLE frontier empty source batching

- `age_adjacency` visible payload scan에 directory-cache 기반 known-empty key hint를 추가했다.
- VLE `age_adjacency` payload source가 frontier next source key를 source-local batch로 모은 뒤 source 종료 시점에
  payload cache `known_empty` entry로 반영하도록 했다. active scan 중 payload cache hash를 mutate하지 않고,
  self-loop/current source key는 batch 대상에서 제외한다.
- `AgeVLESourceStats`, `VLE Source Runtime`, `tools/vle_benchmark.sql`에
  `empty-frontier=age_adjacency:N/out:N/in:N` evidence를 추가했다.
- `vle_frontier_empty_policy` regression fixture를 추가해 frontier mark와 이어지는 known-empty cache hit을
  `EXPLAIN (ANALYZE, VERBOSE, TIMING OFF, SUMMARY OFF)` expected로 고정했다.
- lldb로 확인한 SIGSEGV는 새 field 추가 뒤 stale object가 남은 incremental build 문제였다.
  `make clean` 후 전체 재빌드로 `cleanup_callback` offset mismatch가 해소됐다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `tools/vle_benchmark.sql` smoke profile: `run_standard_cases=0`, `800-label-fanout-*`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-05: VLE known-empty source-run precheck

- `age_vle_adjacency_payload_cache_lookup()`를 추가해 payload cache를 생성하지 않는 read-only lookup을 제공했다.
- missing vertex fallback이 payload source object를 만들기 전에 active source cursor의 `known_empty` state를
  확인하고, 모든 active direction이 known-empty이면 source-run 자체를 처리된 empty completion으로 접도록 했다.
- `VLE Source Runtime`과 `tools/vle_benchmark.sql` summary에
  `empty-run=age_adjacency:N/out:N/in:N` evidence를 추가했다.
- `vle_empty_cache_policy`와 `vle_frontier_empty_policy` expected는 known-empty source가 payload begin 단계의
  `empty-cache`보다 더 이른 source-run precheck에서 접히는 것을 보여준다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `tools/vle_benchmark.sql` smoke profile: `run_standard_cases=0`, `800-label-fanout-*`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-05: VLE empty lifecycle runtime feedback 보정

- `missing-vertex-source` runtime class가 empty source lifecycle evidence를 덮어 class mismatch로 보이던 문제를
  정리했다.
- planned source와 runtime dominant source가 일치하고, planned class가 `adjacency-cache-seeded`이며, empty source
  suppression/frontier/run evidence가 있으면 runtime feedback class와 recommendation을 planned descriptor와 맞춘다.
- `adjacency-empty-suppressed` pressure를 cache seed miss보다 먼저 분류해 empty lifecycle pressure가 raw EXPLAIN에
  직접 남도록 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `git diff --check`

## 2026-06-05: VLE empty source pressure 분리

- `AgeVLESourceStats`에 `age_adjacency_empty_scans`와 `endpoint_btree_empty_scans`를 추가했다.
- adjacency/endpoint candidate source wrapper가 yielded candidate 0개인 scan을 empty probe로 기록하고,
  CustomScan executor가 iterator별 counter를 실행 전체 누적값에 합산한다.
- `VLE Source Runtime`과 `tools/vle_benchmark.sql` summary가 `empty=age_adjacency:N/endpoint-btree:N`,
  `runtime_pressure`, `runtime_action`을 함께 출력한다.
- 800-label fan-out smoke에서 기존 `adjacency-density-low`가 실제로는 `age_adjacency` scan 9회 중 empty
  8회인 `adjacency-empty-probe` / `suppress-empty-source` 압력임을 분리했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`
  - `tools/vle_benchmark.sql` smoke profile: `run_standard_cases=0`, `800-label-fanout-*`

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

## 2026-06-05: `AGE Adjacency Match` 기본 index path 채택

- `age.enable_adjacency_match`와 `age.enable_adjacency_match_custom_path` GUC를 제거했다.
  `age_adjacency` 인덱스가 있고 endpoint가 bound된 one-hop MATCH는 기본적으로 정상 edge RTE를 유지하면서
  `AGE Adjacency Match` CustomPath 후보를 등록한다.
- parser의 예전 SRF provider fallback과 dead helper를 제거했다.
- edge variable projection, edge property predicate, right node property predicate가 있어도 CustomPath 후보를
  배제하지 않도록 planner gate를 넓혔다. 필요한 edge payload column은 CustomScan tlist에서 수집하고 residual
  predicate는 plan qual로 유지한다.
- `AGE Adjacency Match` executor는 endpoint payload를 `List`에 모두 모으지 않고
  `AgeAdjacencyVisiblePayloadScan` cursor에서 한 tuple씩 streaming한다.
- regression은 disabled/enabled GUC assertion을 제거하고, 기본 CustomPath 채택 및 broadened edge
  property/projection/right-property shape를 expected로 고정했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: `AGE Adjacency Match` index descriptor surface

- `AGE Adjacency Match` CustomPath private descriptor에 direction, endpoint key, estimated fanout,
  edge variable/properties requirement, right label/property residual requirement를 실었다.
- executor EXPLAIN hook을 추가해 `Adjacency Index`와 `Adjacency Payload Columns`를 출력한다. required payload
  columns와 residual shape가 raw `EXPLAIN (VERBOSE, COSTS OFF)` expected에 직접 남는다.
- `age_adjacency` regression에 descriptor surface smoke를 추가했다. edge property predicate, right label/property
  requirement, endpoint fanout, payload column mode가 plan output에 보인다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`

## 2026-06-05: `AGE Adjacency Match` payload-aware cost

- `AGE Adjacency Match` descriptor에 planned payload mode를 추가했다. `Adjacency Index` EXPLAIN은
  `payload=id-only|edge-row`를 출력한다.
- candidate 단계에서 edge variable projection 또는 edge property predicate가 있으면 `edge-row`로 보고,
  heap recheck/cpu cost factor를 높인다. 그렇지 않은 endpoint id-only shape는 낮은 factor를 사용한다.
- actual CustomScan tlist가 출력하는 required payload columns는 계속 `Adjacency Payload Columns`에 남긴다.
  다음 단계는 이 actual column vector를 path cost 단계로 더 일찍 끌어올리는 것이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`

## 2026-06-05: `age_adjacency` main run block 압축

- `INDEX.md`의 sorted adjacency/CSR 방향을 기준으로 bulk-built main run을 posting별 page item에서 run-local
  packed block item으로 바꿨다.
- directory entry는 run key, 첫 block 위치, logical posting count를 유지하고, main block은
  `(heap_tid, edge_id, next_vertex_id)` array만 저장한다. scan/cache는 active key를 emission 직전에 재구성한다.
- bulk delete는 block 내부 posting을 compact하고, cost hook은 packed main block density를 main page estimate에
  사용한다.
- `age_adjacency_debug_main_probe()`는 packed block item 수를 출력해 logical posting 수 대비 page item 감소를
  regression에서 확인한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`
  - `git diff --check`
  - `git diff --check`

## 2026-06-05: `age_adjacency` main block graphid 압축

- main run block 안의 edge/next label id가 같으면 label id를 block header에 저장하고 posting에는 48-bit entry id만
  저장하게 했다.
- bulk build는 label-homogeneous chunk를 만들어 compact block을 우선 사용하고, mixed label chunk는 full graphid
  block으로 fallback한다.
- scan/cache/bulk delete는 compact/full block을 같은 helper로 읽고, debug probe는 compact/full block count를 출력한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: `age_adjacency` delta page-local graphid 압축

- delta page opaque에 key/edge/next label id triple을 저장하고, delta item은 세 48-bit entry id와 TID만 저장하게 했다.
- insert label triple이 현재 delta page와 다르면 새 delta page를 시작해 page-local compact invariant를 유지한다.
- payload scan, visible payload cursor, bulk delete, delta maintenance/cost estimate가 compact delta item layout을
  사용한다.
- regression은 `delta_tuples_per_page > 226`으로 full posting layout보다 page density가 늘어난 것을 확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: `AGE Adjacency Match` terminal property feedback

- metadata-backed terminal property index 후보를 CustomPath row estimate와 recheck cost에 반영했다.
- `Adjacency Terminal Runtime`은 property index prefetch match 수, payload 후보 수, terminal filter 수, emitted row 수,
  cache hit, id index lookup 수를 출력한다.
- `age_adjacency` regression에 `EXPLAIN ANALYZE` surface를 추가해 `property-index-prefetch`가 실제 runtime에서
  후보 pruning evidence를 제공하는지 확인한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`

## 2026-06-05: `AGE Adjacency Match` required payload vector

- `AdjacencyMatchPayloadRequest`를 CustomPath 생성 전에 산정해 required edge columns를 cost와 descriptor가
  직접 소비하게 했다.
- anonymous edge id-only shape는 `required=start_id,end_id`와 낮은 payload cost를 사용하고, edge property
  predicate shape는 `required=start_id,end_id,properties`와 edge-row cost를 사용한다.
- `custom_scan_tlist`도 같은 request에서 생성한다. executor fetch 여부와 EXPLAIN의 planned descriptor가 같은
  column vector를 기준으로 움직인다.
- `age_adjacency` regression은 id-only descriptor와 edge-row descriptor를 모두 raw EXPLAIN으로 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`

## 2026-06-05: `AGE Adjacency Match` right label pruning

- right endpoint label id를 adjacency match candidate/descriptor에 추가했다.
- `AgeAdjacencyVisiblePayloadScan`에 terminal label id setter를 추가하고, posting의 `next_vertex_id` label bits가
  맞지 않으면 edge heap fetch 전에 제외한다.
- EXPLAIN은 `right-prune=label:yes/props:deferred`를 출력한다. property residual은 아직 composite property
  lookup request가 없어 deferred로 남긴다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match'`

## 2026-06-05: graph index parser surface

- `INDEX.md` 조사 방향에 맞춰 graph index 생성 surface를 helper-only에서 Neo4j식 Cypher DDL로 올렸다.
- `CREATE INDEX name FOR (n:Label) ON (n.prop)`와 relationship property index는 property source index를 만들고,
  `CREATE INDEX name FOR ()-[r:TYPE]->() ON (ADJACENCY)`는 outgoing `age_adjacency(start_id,id,end_id)` source
  index를 만든다. incoming 방향은 `<-` pattern으로 표현한다.
- `DROP INDEX name`을 추가해 parser DDL이 create/drop 양쪽을 갖게 했다. drop은 graph schema의 실제 index와
  `ag_catalog.ag_graph_index` metadata를 함께 제거한다.
- `SHOW INDEXES`와 `ag_catalog.ag_graph_index` metadata table을 추가했다. graph-local index name, source kind,
  entity kind, label/type, property vector, state, provider를 조회할 수 있다.
- `age_adjacency` descriptor regression은 직접 DDL/helper 대신 `CREATE INDEX`, `SHOW INDEXES`, `DROP INDEX`
  parser surface를 사용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_match index'`

## 2026-06-05: property index handoff catalog slot

- `CypherPropertyIndexHandoff`에 matched index OID와 catalog index expression 기반 cached-property slot descriptor를
  추가했다.
- RTE 기반 property index surface rewrite가 relcache index expression을 직접 `ChangeVarNodes`로 변형하지 않고
  복사본을 사용하게 했다.
- DML/MERGE 중 generic mutator가 NULL context child를 방문할 때 property index rewrite context를 역참조하던 crash를
  lldb로 확인하고 context 없는 호출은 traversal-only로 처리하게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - MERGE unique violation 재현 SQL이 backend crash 없이 원래 오류로 종료되는지 확인
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='index cypher_match'`

## 2026-06-05: VLE source feedback class descriptor

- runtime threshold feedback과 payload replay feedback을 reason 문자열만이 아니라
  `threshold_input_class`, `payload_input_class` descriptor로 전달하도록 했다.
- `VLE Edge Source` EXPLAIN은 runtime-cache 입력의 `reason`과 `class`를 함께 출력한다.
- planner policy feedback은 payload replay와 root empty lifecycle 판단에서 reason 문자열보다 class를 우선 사용한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: typed terminal property source prefetch

- `AGE Adjacency Match` terminal property prefetch가 property source index의 실제 첫 key type을 기준으로
  scan key를 만든다.
- 지원 domain은 `agtype`, `int8`, `float8`, `numeric`, `text`이며, 변환할 수 없는 runtime value는 prefetch를
  끄고 기존 id-btree-cache recheck 경로로 내려간다.
- `agtype_to_int8/text` SQL cast 함수는 `fcinfo->flinfo`를 읽는 variadic fast path가 있어 executor에서
  `DirectFunctionCall1`로 부르면 backend crash가 날 수 있었다. executor는 공개 agtype container API로 scalar를
  읽고 PostgreSQL typed input/output function으로 scan key를 만든다.
- planner의 `prefetch=eligible` 판정은 더 이상 `domain=agtype`에 고정되지 않는다. metadata-backed property
  source index와 CustomScan 내부에서 평가 가능한 const/runtime-slot value가 있으면 typed prefetch 후보가 된다.
- regression은 `M.i`의 `int8` property source index가
  `Adjacency Terminal Property: ... domain=int8 ... prefetch=eligible ... mode=property-index-prefetch`로 실행되는
  plan surface를 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`
  - `git diff --check`

## 2026-06-05: adjacency payload pruning counter split

- `AGE Adjacency Match` payload runtime의 `cache-filtered` 합계를 유지하면서 `cache-label`과
  `cache-property`를 추가했다.
- main payload cache load 단계에서 terminal label block skip과 terminal property prefetch set pruning을 구분해
  기록한다. 같은 endpoint fan-out에서 label-first pruning이 큰지, property-source prefetch가 큰지 raw EXPLAIN
  surface로 확인할 수 있다.
- `M.i` typed property source regression은 `cache-label=2 cache-property=0`으로, `N.i` source prefetch regression은
  `cache-label=1 cache-property=1`로 드러난다. 다음 label+property composite seek 설계는 이 분리된 runtime
  evidence를 기준으로 진행한다.

## 2026-06-05: adjacency terminal source strategy descriptor

- `AGE Adjacency Match` CustomPath descriptor에 terminal pruning strategy를 싣고 EXPLAIN의
  `Adjacency Pruning`에 `terminal-source=...`로 출력한다.
- label-only pruning은 `label-block`, label+property prefetch는 `label-block+property-source`, property index가
  없으면 `property-recheck`로 표현한다.
- 이 값은 executor가 주변 상태에서 다시 추론하는 문자열이 아니라 planner descriptor에서 전달되는 field다. 다음
  composite seek와 cost 보정은 이 strategy와 `cache-label`/`cache-property` runtime evidence를 함께 본다.

## 2026-06-05: key-bound terminal property prefetch

- terminal property source prefetch를 value 설정 시점이 아니라 endpoint key binding 이후로 늦췄다.
- `age_adjacency_visible_payload_scan_begin_key()`가 directory run을 열면 active posting count를 볼 수 있고,
  terminal lookup은 이 candidate count를 기준으로 `property-index-prefetch`를 준비한다.
- non-ANALYZE EXPLAIN은 아직 key/run이 없으므로 `mode=deferred-prefetch`를 출력하고, ANALYZE 실행에서는 같은
  plan이 endpoint run을 연 뒤 `mode=property-index-prefetch`로 전환된다.
- 작은 endpoint run은 property index 전체 prefetch 대신 id-btree-cache recheck 경로로 내려갈 수 있는 lifecycle
  boundary가 생겼다. 다음 cost 보정은 이 threshold와 `cache-label`/`cache-property` evidence를 함께 사용한다.

## 2026-06-05: terminal prefetch gate runtime evidence

- `Adjacency Terminal Prefetch` EXPLAIN ANALYZE line을 추가해 `candidate-count`, `threshold`,
  `skipped-small`을 출력한다.
- 작은 endpoint run fixture를 추가했다. fanout 1에서는 property source prefetch를 건너뛰고
  `mode=id-btree-cache`, `index-lookups=1`, `skipped-small=1`로 실행된다.
- 이 과정에서 기존 vertex-cache recheck가 `agtype_object_field_equals`를 `DirectFunctionCall3`로 호출하면
  `fcinfo->flinfo` cache 접근 때문에 crash할 수 있음을 확인했다. terminal recheck는 local agtype object field
  equality helper로 바꿔 fmgr expression cache에 의존하지 않는다.

## 2026-06-05: array aggregate property slot vector

- `CypherArrayAggPropertyHandoff`에 cached-property slot vector, slot count, final materialization weight를
  보관하게 했다.
- `array_agg` map/list/single property rewrite가 property path expr를 만들 때 slot descriptor를 함께 누적한다.
- narrow aggregate path 비용 보정은 고정값 대신 aggregate handoff의 final materialization weight를 사용한다.
- typed collect narrow path도 cached-property slot의 final materialization weight를 비용 보정에 반영한다.
- aggregate materialization credit 계산은 `cypher_paths.c`에서 직접 handoff 내부를 해석하지 않고
  `cypher_property_paths` descriptor helper가 소유하도록 이동했다.
- `age_array_agg_map_slots`/`age_array_agg_list_slots` parallel serialization은 slot마다 null byte를 쓰지 않고
  null bitmap을 먼저 보낸 뒤 non-null payload만 직렬화한다. deserialize/copy는 state row 수에 맞는 capacity를
  처음부터 잡아 큰 partial aggregate state의 즉시 repalloc을 피한다.
- parallel worker 검증 중 NULL partial state와 combine 단계의 잘못된 memory context copy가 확인되어 함께 수정했다.
  slot value는 항상 aggregate context에 detoast-copy하고, serialize/deserialize는 NULL state를 그대로 반환한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_match'`

## 2026-06-05: aggregate payload domain cost descriptor

- `CypherArrayAggPropertyHandoff`에 typed/agtype payload slot count와 payload materialization weight를 추가했다.
- `array_agg` property/map/list narrow path cost credit이 final output materialization뿐 아니라 transition payload
  domain도 읽도록 했다. typed original map/list aggregate는 scalar payload 유지 이득을 더 직접 반영하고, legacy
  agtype property aggregate는 agtype payload로 구분된다.
- handoff에 slot별 `payload_value_types` vector를 추가하고, narrow path 후보 검증과 cost 계산이 이 vector를
  직접 소비하게 했다. aggregate state header의 value type vector와 planner descriptor가 같은 slot cardinality
  contract를 갖는다.
- aggregate cached slot도 `CypherPropertyHandoffDescriptor`를 보존하고, 확정된 slots aggregate OID를 descriptor에
  stamp한다. narrow path 후보는 cached slot, payload type vector, property descriptor count가 모두 같은 경우에만
  생성되어 이후 property/index metadata join이 expression 재파싱 없이 같은 slot descriptor에서 출발할 수 있다.
- property index handoff는 query cached slot과 index cached slot의 physical signature 일치 여부를
  `index_domain_matches_cached_slot`으로 보존한다. 일치할 때만 query slot에 index expression을 연결해 typed
  property source index와 aggregate/property descriptor가 같은 domain contract를 유지한다.
- `array_agg` property handoff refresh가 aggregate cached slot을 base rel expression index metadata와 조인한다.
  domain-matched property index가 있으면 aggregate input expression을 catalog index expression surface로 바꾸고,
  `index_domain_match_slot_count`를 row-sensitive materialization credit에 반영한다.
- `cypher_match` regression은 `payload.a` property source index 위에서 `array_agg(n.payload.a)` EXPLAIN이
  `agtype_access_operator(VARIADIC ARRAY[...])` catalog index expression surface를 lower aggregate input으로 쓰는지
  드러낸다.
- header 변경 뒤 stale optimizer object가 남으면 handoff struct 크기 차이로 planner stack이 깨질 수 있음을 lldb와
  focused regression crash로 확인했다. clean rebuild 후 같은 focused regression과 full regression은 통과했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='expr cypher_match age_adjacency cypher_vle'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`

## 2026-06-05: aggregate wide text benchmark

- `tools/aggregate_index_benchmark.sql`에 `wide_text_width` 입력과 `typed-wide-text-aggregate` shape를 추가했다.
- benchmark data의 `payload.d`는 configurable text width를 사용하고, summary는 `numeric,text` slot-vector descriptor와
  direct slot-state value bytes를 함께 출력한다.
- `threshold_rows=20,100`, `wide_text_width=64` smoke에서 `typed-wide-text-aggregate`는 row당 value bytes 84,
  estimate 32, `slot_value_estimate_ratio=2.63`을 출력했다.
- 결론: short text 기준 static estimate를 바로 키우지 않고, typmod/statistics/sample-aware width descriptor 또는
  benchmark-driven workload scaling을 다음 비용 모델 후보로 둔다.
- 검증:
  - `tools/aggregate_index_benchmark.sql` smoke (`threshold_rows=20,100`, `wide_text_width=64`)

## 2026-06-05: directory-backed adjacency fanout cost

- const-bound `AGE Adjacency Match` planner가 `age_adjacency` directory entry를 읽어 endpoint run fanout,
  terminal label fanout, terminal label group count를 descriptor와 cost input에 싣도록 했다.
- `Adjacency Index`와 `Adjacency Cost Input`은 `label-groups=N fanout-source=directory|statistics`를 출력한다.
- regression은 dynamic endpoint의 statistics fallback과 const endpoint의 `fanout-source=directory` 케이스를 함께
  고정한다.
- 검증:
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency'`

## 2026-06-05: VLE directory-backed source fanout

- VLE stream source descriptor가 start/end const graphid와 base vertex `id = const` restriction을 source cost
  evidence로 소비하게 했다.
- `age_adjacency` out/in directory entry에서 endpoint run posting 수를 읽어 VLE fanout estimate를 보정하고,
  `VLE Source Cost`는 `source=start:directory/end:statistics`처럼 evidence source를 출력한다.
- terminal label descriptor가 있으면 directory run 전체가 아니라 terminal label posting 수를 사용하고
  `source=start:directory-label`을 출력한다.
- `cypher_vle` regression에 `WHERE id(s) = ... MATCH (s)-[:R*]->...` 케이스를 추가해 directory-backed VLE source
  fanout을 고정했고, mixed terminal label fixture로 run fanout 2가 terminal label fanout 1로 줄어드는 케이스를
  고정했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: `age_adjacency` terminal label grouping

- `age_adjacency` bulk build ordering을 endpoint key 다음 terminal label/edge label 기준으로 바꿨다.
- terminal label constraint가 있는 visible payload scan은 compact main run block descriptor가 label mismatch를
  증명할 때 block 전체를 건너뛴다. full block은 per-posting filter로 유지한다.
- 같은 endpoint에 terminal label이 섞인 regression fixture를 추가해 6개 posting이 label별 2개 compact block으로
  묶이는지 확인한다.
- terminal property prefetch set도 main cache load 단계에서 소비하게 했다. `Adjacency Payload Runtime`은
  `cache-filtered=N`을 출력해 label/property source pruning이 cache 폭을 줄였는지 보여준다.
- terminal property descriptor에 `value=const|runtime-slot|none`과 `prefetch=eligible|ineligible`을 추가했다.
  const가 아닌 runtime expression도 현재 CustomScan required outer 안에서 평가 가능하면 property source prefetch
  후보로 유지한다.
- property source index metadata에 `property_type`을 기록하고, `Adjacency Terminal Property`가 `domain=...`을
  출력하게 했다. typed property index는 domain을 보존하되 AGTYPE prefetch scan key와 섞지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`

## 2026-06-05: endpoint run-size-aware terminal prefetch

- `age_adjacency` directory layout을 v6로 올리고 endpoint run의 terminal label distinct count를 summary에 추가했다.
- terminal label constraint가 있으면 terminal property prefetch threshold의 candidate count를 전체 endpoint fanout이
  아니라 label-aware estimate로 계산한다.
- 작은 mixed-label endpoint run은 `property-index-prefetch` 대신 `id-btree-cache`를 사용해 global property source
  prefetch 비용을 피한다.
- `Adjacency Terminal Prefetch`는 전체 endpoint `run-count`와 label-aware `candidate-count`를 함께 출력한다.
- `age_adjacency_debug_main_probe()`는 `main_label_groups`를 반환해 directory label summary가 regression surface에
  드러난다.
- `AGE Adjacency Match` planner descriptor와 EXPLAIN은 planned `terminal-fanout`을 출력하고, row estimate와 terminal
  prefetch threshold는 terminal label fanout을 기준으로 계산한다.
- `Adjacency Terminal Prefetch`는 threshold `reason`을 출력해 작은 terminal slice, 큰 terminal fanout, edge payload
  requirement, non-indexable shape를 구분한다.
- `Adjacency Terminal Runtime`은 `outcome`과 `action`을 출력해 threshold reason이 property source prefetch, id-cache
  유지, runtime value 대기 중 어느 lifecycle로 실행됐는지 드러낸다.
- planner cost는 planned terminal prefetch lifecycle을 기준으로 selectivity/recheck credit을 적용한다.
  `planned=id-cache-small`이면 property-source prefetch credit을 주지 않는다.
- `Adjacency Terminal Runtime`은 `lifecycle-match=true|false`를 출력해 planned terminal lifecycle과 actual outcome의
  일치 여부를 보여준다.
- 검증:
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck`
  - `git diff --check`

## 2026-06-05: directory range known-empty precheck

- `age_adjacency_visible_payload_scan_key_known_empty()`가 terminal label range뿐 아니라 active vertex-set range
  filter도 소비하게 했다.
- prefilter candidate range와 endpoint run range가 겹치지 않는 no-delta key는 scan open 전에 known-empty로
  판정할 수 있다.
- `age_adjacency_debug_key_known_empty_range()`를 추가해 label hit/range hit은 `false`, label hit/range miss는
  `true`인 경로를 regression에 고정했다.
- 이 변경은 VLE frontier empty queue가 property prefilter descriptor를 가진 payload scan에서도 directory range miss를
  source completion evidence로 올릴 수 있게 하는 기반이다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`
  - `git diff --check`

## 2026-06-05: directory range summary for VLE prefilter sets

- `age_adjacency` directory entry에 endpoint run의 `min_next_vertex_id`/`max_next_vertex_id`를 추가하고 index
  layout version을 v7로 올렸다.
- property source prefetch가 만든 vertex id range와 endpoint run range가 겹치지 않으면 main run을 열지 않고
  directory 단계에서 접는다.
- VLE는 `begin_key()`로 run count를 먼저 읽은 뒤 prefilter set을 붙이므로, vertex-set filter setter에서도 active
  directory entry를 재평가해 이미 열린 main run을 cache fill 전에 닫는다.
- `VLE Payload Runtime`/`Adjacency Payload Runtime`은 directory range reject가 발생하면
  `set-directory-filter=N`을 출력한다.
- `vle_index_probe` regression은 시작 vertex 자신만 `isolated=true`이고 direct neighbor에는 없는 property source
  match를 사용해 `set-directory-filter=7` 경로를 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`
  - `git diff --check`

## 2026-06-05: compact block precheck for VLE prefilter sets

- sorted vertex id descriptor를 `age_adjacency` compact main block precheck까지 올렸다.
- compact block의 homogeneous terminal label과 packed next entry id를 사용해 property prefetch matched set과
  교차하지 않는 block은 posting cache materialization 전에 건너뛴다.
- VLE와 fixed `AGE Adjacency Match` payload runtime은 block 단위 reject가 발생하면 `set-block-filter=N`을
  출력한다.
- 작은 regression fixture에서는 기존 range/sorted posting filter가 먼저 드러나지만, 큰 fan-out에서 compact block
  전체가 property matched set과 교차하지 않는 경우 cache fill과 per-posting filter loop를 줄이는 구조다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: VLE prefilter set sorted descriptor

- `AgeAdjacencyVertexSetFilter`가 property source prefetch 결과를 hash set뿐 아니라 sorted vertex id vector로도
  전달하게 했다.
- `age_adjacency` payload scan은 range reject 뒤 sorted membership을 먼저 확인해 hash lookup 없이 in-range
  non-match 후보를 제거한다.
- VLE와 fixed `AGE Adjacency Match` payload runtime은 sorted membership reject가 발생하면
  `set-sorted-filter=N`을 출력한다.
- `vle_index_probe` fixture는 property matched set의 min/max range 안에 있지만 sorted set에는 없는 direct
  neighbor를 포함해 `set-range-filter=5 set-sorted-filter=1` 경로를 regression surface에 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`
  - `git diff --check`

## 2026-06-05: VLE directory-filter empty evidence 정규화

- VLE `age_adjacency` source가 0 candidate를 반환했더라도 terminal property prefetch set과 adjacency directory range가
  교차하지 않아 닫힌 경우는 일반 `empty-scan`으로 세지 않도록 분리했다.
- `AgeVLESourceStats`에 directory-filtered empty scan counter를 추가하고, candidate source가 payload end 이후
  `set-directory-filter` 증가분을 확인해 `evidence=directory-filter`로 정규화한다.
- `VLE Empty Evidence` line은 불필요한 `directory-filter=0` field를 추가하지 않고, 실제 directory pruning 케이스만
  evidence vocabulary를 바꾼다. 이로써 runtime line 길이를 늘리지 않으면서 composite property prefilter와
  adjacency directory가 연계되어 후보를 없앤 사실을 드러낸다.
- 첫 focused regression에서 segfault가 발생했는데, `AgeVLESourceStats` layout 변경 뒤 증분 빌드가 일부 참조
  object를 갱신하지 않은 stale object 문제였다. clean build/install 후 crash는 사라졌고 regression diff는 의도한
  EXPLAIN evidence 변경으로 수렴했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: VLE composite prefilter feedback 입력

- `adjacency-composite-prefilter` source policy에서 payload scan이 terminal property prefilter를 실행하면 backend-local
  runtime feedback cache에 payload evidence를 기록하게 했다.
- directory range miss가 source를 0 candidate로 닫은 경우는 `payload-directory-filter-observed` /
  `adjacency-composite-prefilter` class로 기록한다. 일반 property prefilter scan은
  `payload-composite-prefilter-observed` class로 기록한다.
- 다음 같은 VLE composite plan은 `VLE Source Payload Input: source=runtime-cache ... class=adjacency-composite-prefilter`
  를 출력하므로, directory-filter evidence가 EXPLAIN surface에서 끝나지 않고 다음 planning descriptor 입력으로
  소비되는 것을 확인할 수 있다.
- composite payload feedback은 `composite_prefilter_planned` profile에서만 읽도록 제한해 terminal-scalar family cache가
  terminal label/property predicate가 없는 VLE shape로 번지지 않게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency'`

## 2026-06-05: adjacency composite terminal filter request

- `age_adjacency` payload scan에 `AgeAdjacencyCompositeTerminalFilter` request를 추가해 terminal label, property source
  vertex set, callback fallback을 하나의 scan contract로 전달하게 했다.
- 기존 `set_terminal_label`, `set_terminal_vertex_filter`, `set_terminal_vertex_set_filter` API는 compatibility wrapper로
  남기고 내부에서 composite request를 호출한다.
- fixed `AGE Adjacency Match`와 VLE payload source는 label+property prefetch를 더 이상 label setter와 property
  setter의 순서 의존 조합으로 넘기지 않고, `label-property-prefetch|label-property-callback` source를 가진 composite
  request로 전달한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency cypher_match'`

## 2026-06-05: composite request property summary evidence

- `AgeAdjacencyCompositeTerminalFilter`에 property source index OID, VLE property filter id, prefetched match count를 담는
  property summary field를 추가했다.
- VLE와 fixed `AGE Adjacency Match`는 property prefilter handoff 때 이 summary를 request에 싣는다. label-only request는
  summary로 세지 않으므로 runtime output이 불필요하게 길어지지 않는다.
- `age_adjacency` payload runtime은 property summary가 실린 composite request를 받으면 counter를 증가시키고,
  VLE `VLE Payload Runtime`은 `composite-request=N`으로 이를 출력한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='cypher_vle age_adjacency cypher_match'`

## 2026-06-05: directory pruning reason runtime 정리

- `age_adjacency` directory-level vertex-set pruning 총량을 range, exact graphid summary, terminal-label bloom,
  compressed entry vector, request-side value summary, wide bloom reason으로 분리했다.
- fixed `AGE Adjacency Match`와 `AGE VLE Stream` runtime formatter가 모두
  `set-directory-filter=N/range:N/exact:N/label-bloom:N/compressed:N/value-summary:N/wide-bloom:N` suffix vocabulary를
  공유한다.
- `age_adjacency_debug_composite_probe()`는 `set_directory_range_filter`, `set_directory_exact_filter`,
  `set_directory_label_bloom_filter` column을 추가로 반환해 regression에서 generic directory miss와 value identity
  miss를 구분한다.
- `cypher_vle` composite prefilter regression은 0-candidate directory skip이 `range` reason임을 raw EXPLAIN에
  드러낸다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: value-posting bloom summary 추가

- `age_adjacency` storage version을 17로 올리고 directory entry와 main run block에 request-gated
  `value_posting_bloom` summary를 추가했다.
- property value 자체는 adjacency index에 없으므로 index tuple에 저장하지 않는다. 대신 property prefetch가 만든
  matched vertex set summary와 index-side endpoint posting bloom을 `property_filter_id`가 있는 request에서만 교차
  확인한다.
- runtime/debug output은 기존 value identity request 총량인 `/value-summary:N`과 실제 새 on-disk summary reject인
  `/value-posting:N`을 분리한다.
- directory-level value-posting은 우선 homogeneous terminal label run에만 적용해 mixed-label directory run에서
  block-level pruning evidence를 과도하게 가리지 않게 했다.
- `age_adjacency_value_posting` regression fixture는 기존 64/256-bit value summary false positive를 통과한 뒤
  새 directory value-posting bloom이 34 postings를 접는 경로를 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: label-slice value-posting summary 연결

- `age_adjacency` storage version을 18로 올리고 directory terminal-label slot마다
  `next_vertex_label_value_posting_bloom`을 추가했다.
- mixed-label directory run에서도 request terminal label이 directory label slot에 있으면 전역 run bloom이 아니라
  label-slice value-posting bloom을 소비한다. slot이 없는 label은 safe fallback으로 기존 residual path를 유지한다.
- fixed `AGE Adjacency Match` planner/explain descriptor는 directory fanout evidence와 함께
  `value-posting=label-slice|run|none`을 출력한다.
- `age_adjacency_label_value_posting` regression fixture는 label 1/2가 섞인 run에서 label 1 request가
  directory `/value-posting` skip을 만드는지 고정한다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE value-posting source evidence 연결

- `AGE VLE Stream` descriptor에 start/end `value-posting` source field를 추가해 directory fanout evidence와 함께
  `label-slice|run|none` source를 executor까지 전달한다.
- `VLE Source Cost`는 `value-posting=start:.../end:...`를 출력하고, composite property prefilter가 planned된
  경우 `VLE Source Policy`도 `value-posting=out:.../in:...`를 출력한다.
- value-posting source availability만으로 planned class를 승격하지 않도록 정리했다. 실제
  `/value-posting:N` runtime counter가 발생한 실행만 runtime pressure/action에서
  `adjacency-composite-value-posting` / `keep-value-posting`으로 드러낸다.
- `tools/vle_benchmark.sql` summary에 `value_posting_source` column을 추가해 planned source evidence와 runtime
  counter를 같은 row에서 비교할 수 있게 했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE value-posting feedback cache 연결

- `VLE Source Payload Input`에 `value-posting=SOURCE/observed:N`을 추가해 backend-local feedback cache가 실제
  value-posting skip 수와 source class를 후속 plan에 전달한다.
- `derive_vle_source_runtime_payload_feedback()`은 `/value-posting:N` runtime counter가 있는 composite prefilter
  실행을 `payload-value-posting-observed` reason과 `adjacency-composite-value-posting` class로 기록한다.
- planner policy는 runtime cache에서 관측된 value-posting success가 있을 때만 `recommendation=keep-value-posting`으로
  승격한다. source descriptor에 `label-slice|run`이 존재한다는 이유만으로 planned class를 올리지는 않는다.
- `vle_value_posting_feedback` regression fixture를 추가해 첫 `EXPLAIN ANALYZE`가
  `set-directory-filter=38/value-posting:38`을 만들고, 이어지는 plain `EXPLAIN`이
  `class=adjacency-composite-value-posting recommendation=keep-value-posting`을 소비하는 경로를 고정했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE value-posting feedback identity key 세분화

- VLE source feedback cache key에 terminal label id, property source index OID, `property_filter_id`,
  value-posting source class를 추가했다.
- planner descriptor는 const terminal property predicate에서 같은 `age_adjacency_property_filter_id()`를 계산해
  lookup key에 싣고, executor descriptor는 runtime feedback record가 같은 identity를 쓰도록 terminal label/property
  filter field를 전달한다.
- composite prefilter가 아닌 empty/replay feedback은 broad key를 유지하고, terminal property prefilter가 있는
  value-posting/composite payload feedback만 identity discriminator를 채운다.
- `vle_value_posting_feedback` regression은 `n.i = 59`가 만든 value-posting success를 같은 predicate 후속 plan만
  소비하고, `n.i = 1` 후속 plan은 `source=none`으로 남아 false promotion이 없음을 고정한다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE benchmark value-posting decision table 확장

- `tools/vle_benchmark.sql` planner summary에 `payload_input_value_posting_source`,
  `payload_input_value_posting_observed`, `value_posting_decision`을 추가했다.
- runtime summary는 `VLE Payload Runtime`의 `/value-posting:N` counter를
  `payload_value_posting_filtered`로 파싱한다.
- final decision table은 `value_posting_identity_cache_hit`, `value_posting_runtime_hit`,
  `value_posting_runtime_decision`을 출력해 source availability, predicate-safe cache hit, 현재 실행의 runtime hit을
  같은 row에서 비교한다.
- 검증:
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_smoke_pg`, port 55432
  - `psql -p 55432 -d postgres -v graph=age_vle_bench_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=4 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE benchmark value-posting fixture 추가

- `tools/vle_benchmark.sql`에 `value_posting_edges` profile 변수를 추가하고,
  `ValuePostingNode`/`ValuePostingOther`/`ValuePostingEdge` fixture를 직접 graphid 기반으로 생성한다.
- fixture는 `ValuePostingNode.i` property source index와 `ValuePostingEdge` `age_adjacency` index를 함께 만들고,
  `value-posting-reject-seed`, `value-posting-reject`, `value-posting-endpoint-control` EXPLAIN ANALYZE shape를
  실행한다.
- `value-posting-reject`는 property source 후보가 실제 endpoint가 아닌 case라
  `set-directory-filter=42/value-summary:42/wide-bloom:42`를 만들고, summary는 이를
  `payload_value_posting_filtered=42`, `value_posting_runtime_hit=true`,
  `value_posting_runtime_decision=runtime-hit`으로 읽는다.
- endpoint-control은 같은 start/edge/index에서 `n.i = 1` 실제 endpoint를 조회해 runtime hit가 false로 남는다.
  따라서 benchmark table에서 negative pruning과 endpoint hit control을 한 번에 비교할 수 있다.
- 검증:
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_value_pg`, port 55434
  - `psql -p 55434 -d postgres -v graph=age_vle_bench_value_smoke -v sparse_nodes=8 -v dense_nodes=4 -v label_fanout_labels=4 -v label_fanout_edges=8 -v value_posting_edges=38 -v replay_branches=4 -v replay_leaves=4 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`
  - `git diff --check`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE value identity pruning feedback 승격

- VLE runtime payload feedback이 on-disk `/value-posting:N`만 보지 않고 request-side value identity 기반
  `value-summary` block/directory pruning도 같은 composite value pruning evidence로 소비한다.
- `VLE Source Payload Input`은 큰 value-posting fixture에서
  `reason=payload-value-posting-observed class=adjacency-composite-value-posting`으로 후속 plan에 전달되고,
  `VLE Source Policy`와 runtime pressure/action도 `keep-value-posting`으로 맞춰진다.
- `tools/vle_benchmark.sql`의 `value_posting_edges` fixture는 per-root posting run을 256개 이하로 나눠
  `age_adjacency` per-run posting append 한계를 피한다. final summary는 `value_posting_root_count`,
  `value_posting_edges_per_root`를 출력한다.
- 1024-edge benchmark는 `value_posting_root_count=4`, `value_posting_edges_per_root=256`으로 실행되고,
  `value-posting-reject`에서 `payload_input_value_posting_observed=1024`,
  `class=adjacency-composite-value-posting`, `recommendation=keep-value-posting`을 확인했다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - 임시 PostgreSQL 18 instance `/tmp/age_vle_bench_policy_pg`, port 55435
  - `psql -p 55435 -d postgres -v graph=age_vle_bench_policy_256 -v sparse_nodes=16 -v dense_nodes=8 -v label_fanout_labels=64 -v label_fanout_edges=64 -v value_posting_edges=256 -v replay_branches=32 -v replay_leaves=16 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`
  - `psql -p 55435 -d postgres -v graph=age_vle_bench_policy_1024 -v sparse_nodes=16 -v dense_nodes=8 -v label_fanout_labels=128 -v label_fanout_edges=128 -v value_posting_edges=1024 -v replay_branches=64 -v replay_leaves=16 -v run_standard_cases=0 -v preserve_graph=0 -f tools/vle_benchmark.sql`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: VLE payload feedback cache merge 강화

- ORCA `CPhysical::CReqdColsRequest`가 required column set과 child/scalar child index를 함께 key로 쓰는 구조를
  다시 확인했다. AGE VLE feedback cache도 request boundary를 key로 삼고 entry payload lifecycle을 merge해야 한다.
- VLE source threshold cache payload update를 공통 helper로 묶고, replay/cache seed/value identity pruning count를
  누적하면서 `payload_class`/`payload_reason`은 더 강한 payload class rank를 보존하게 했다.
- 이 변경으로 같은 source request family 안에서 일반 composite prefilter나 cache seed 관측이 나중에 들어와도
  `adjacency-composite-value-posting` evidence를 약한 class로 덮어쓰지 않는다.
- 검증:
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `git diff --check`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle cypher_match'`

## 2026-06-05: typed value stats 기반 composite fanout

- VLE와 fixed `AGE Adjacency Match`가 property source index expression의 `pg_statistic.stadistinct`를 읽는 공통
  selectivity helper를 사용하게 했다.
- const predicate는 MCV slot을 먼저 보고, matching frequency가 기존 fallback보다 더 선택적이면 `typed-mcv`를
  composite fanout에 사용한다. MCV가 fallback보다 넓으면 `fallback-mcv-ceiling`으로 fanout 확대를 막는다.
- MCV가 없으면 통계 기반 `1 / ndistinct`가 기존 fallback보다 더 선택적일 때만 composite fanout을 낮춘다. 작은
  fixture나 통계 부재 상황에서는 기존 fallback을 유지해 작은 데이터 regression 안정성을 보존한다.
- `VLE Composite Fanout`은 property-prefilter가 planned인 경우 `selectivity=... selectivity-source=...`를 출력해
  source policy가 fallback인지 typed 통계인지 확인할 수 있게 했다.
- `vle_typed_selectivity` regression fixture를 추가해 `ANALYZE`된 property source index에서
  `selectivity-source=typed-mcv`가 출력되는 경로를 고정했다.
- fixed adjacency descriptor에도 같은 selectivity ppm/source field를 추가해 다음 fixed MATCH EXPLAIN 확장과 cost
  feedback이 같은 vocabulary를 쓰게 했다.
- 검증:
  - `make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  - `make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install`
  - `make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck REGRESS='age_adjacency cypher_vle'`

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
