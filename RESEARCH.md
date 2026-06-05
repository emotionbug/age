# VLE/RPQ Research Notes

## 목적

이 문서는 AGE Cypher/VLE/property 최적화 중 방향을 바꾼 근거, 참고한 PostgreSQL
source/project/paper, 그리고 되돌린 접근의 결론을 기록한다. 상세 진행 로그는
`HISTORY.md`, VLE 구조와 benchmark는 `VLE.md`에 둔다.

## 조인 오더 탐색 방향

- PostgreSQL core는 `deconstruct_jointree()`가 만든 joinlist와 `SpecialJoinInfo` 제약을
  `make_one_rel()`/joinrel DP 탐색에 넘긴다. AGE가 Cypher pattern을 너무 일찍 고정된
  nested-loop/FunctionScan shape로 낮추면 PostgreSQL join search가 graph pattern 사이의
  선택도를 비교할 기회를 잃는다. 따라서 다음 조인 오더 작업은 Cypher pattern을 하나의
  opaque CustomScan으로 숨기는 것이 아니라, graph pattern component와 connector predicate를
  PostgreSQL joinrel 또는 AGE-side query graph descriptor로 남기는 방향이어야 한다.
- ORCA는 `CLogicalNAryJoin`을 만든 뒤 `CXformExpandNAryJoin*`가 query order, min-card,
  greedy avoid-cross-product, DP, DPv2 같은 join order property를 붙여 탐색한다. 특히
  `CJoinOrderDPv2::SExpressionProperties`는 “어떤 알고리즘으로 얻은 join order인가”를
  expression property로 보존하고, group 안에서 property별 best expression을 비교한다. AGE도
  단순히 cheapest path 하나만 남기지 말고 `query-order`, `min-card`, `index-anchored`,
  `adjacency-anchored`, `vle-frontier-anchored` 같은 조인 오더 property를 descriptor로
  남긴 뒤 cost/EXPLAIN/benchmark가 같은 vocabulary를 보게 해야 한다.
- ORCA의 `CJoinOrder::SEdge`/`SComponent`는 join graph edge와 component cover를 bitset으로
  관리한다. 이는 AGE graph pattern에도 직접 맞는다. node binding, relationship binding,
  VLE frontier, property index seek를 component로 보고, equality/property/endpoint constraint를
  edge로 보면 connected expansion과 disconnected Cartesian/product-value join 후보를 같은
  탐색 문제로 만들 수 있다.
- Neo4j planner는 `IDPQueryGraphSolver`, `SingleComponentPlanner`, `ComponentConnectorPlanner`로
  query graph component를 먼저 풀고, disconnected component는 `ValueHashJoinComponentConnector`,
  `ValueMergeJoinComponentConnector`, `CartesianProductComponentConnector`, `ApplyComponentConnector`
  같은 connector로 연결한다. AGE도 VLE/fixed MATCH를 PostgreSQL FROM order에만 맡기기보다
  graph pattern component를 만들고, node-id join, property equality join, adjacency expansion,
  VLE ExpandInto 후보를 connector로 분리해야 한다.
- Neo4j의 `plan_var_expand_into` heuristic은 Cartesian product 뒤의 비싼 ExpandInto를 피하기 위해
  source cardinality를 제한한다. AGE VLE도 unbound/bound endpoint, start/end graphid const,
  terminal label/property index selectivity, `age_adjacency` directory fanout을 조인 오더 request에
  넣어야 한다. 특히 VLE를 먼저 실행할지, selective node/property index seek를 먼저 실행한 뒤
  ExpandInto처럼 검증할지는 join order 단계에서 비교해야 한다.
- 현재 `age_adjacency`와 VLE source policy는 source 내부 후보 폭을 줄이는 데 집중했다. 다음 큰
  전환은 이 evidence를 개별 scan descriptor에 묶어 두지 않고 조인 오더 입력으로 올리는 것이다.
  예를 들어 `Adjacency Composite Source: candidate/composite/planned/reason`,
  `VLE Source Cost: source=start:directory-label`, property value identity matched count는 모두
  join component cardinality와 connector cost로 재사용해야 한다.
- 첫 구현 단위는 전체 optimizer를 새로 쓰는 것이 아니라, Cypher MATCH lowering 단계에서
  “join-order descriptor”를 만들고 EXPLAIN/DEBUG surface에 드러내는 것이다. descriptor에는 component
  id, bound variables, required output variables, provided variables, connector kind, estimated rows,
  source property, order property를 싣는다. 그 다음 단계에서 PostgreSQL path 생성 시
  `AGE Adjacency Match`, property index seek, VLE stream 후보가 이 descriptor의 component/connectivity를
  소비하도록 확장한다.
- 첫 surface는 fixed `AGE Adjacency Match`에 붙였다. `Adjacency Join Order`는 component alias,
  `adjacency-expand`/`adjacency-composite-expand` connector, `start-bound`/`end-bound`,
  `query-order`/`adjacency-anchored`/`adjacency-directory-anchored`/`index-anchored` property와
  row/fanout evidence를 한 줄로 출력한다. 아직 reordering을 바꾸지는 않지만, scan-local
  `Adjacency Cost Input`을 join-order vocabulary로 재표현해 다음 joinrel/path 소비 단계의 stable surface를
  만든다.
- join hook은 이 descriptor를 읽기 시작했다. bound adjacency path의 `required_outer`가 반대쪽 relids에
  묶여 있고 order property가 index/directory/adjacency anchored family이면 nested join row estimate를
  adjacency expansion row 폭으로 조정한다. 이는 전체 join enumerator를 교체하기 전 단계에서
  scan-local fanout evidence를 PostgreSQL joinrel path cost surface로 올리는 최소 연결이다.
- ORCA source 확인 결과 `CLogicalNAryJoin::PxfsCandidates()`는 `ExfExpandNAryJoin`,
  `ExfExpandNAryJoinMinCard`, `ExfExpandNAryJoinDP`, `ExfExpandNAryJoinGreedy`,
  `ExfExpandNAryJoinDPv2`를 동시에 후보로 열고, `CJoinOrderDPv2::SExpressionProperties`는
  join order algorithm bitmask를 `SExpressionInfo`에 보존한다. AGE의 다음 구조도 하나의 fixed lowering만 남기는
  방식이 아니라 graph component별 후보와 order property를 같이 보존해야 한다. 지금의
  `Adjacency Join Order`/`VLE Join Order` line은 이 property vocabulary를 PostgreSQL EXPLAIN surface에 먼저
  고정하는 단계다.
- Neo4j source 확인 결과 `ComponentConnectorPlanner`는 disconnected component를 IDP로 연결할 때
  `ValueHashJoinComponentConnector`, `ValueMergeJoinComponentConnector`, `CartesianProductComponentConnector`,
  `ApplyComponentConnector`를 별도 solver step으로 합성한다. `expandSolverStep`의 VarExpand heuristic은
  both endpoint bound shape를 `ExpandInto`, 한쪽 endpoint bound shape를 `ExpandAll`로 구분하고, single-row 또는
  minimum-cost 정책으로 Cartesian product 뒤 비싼 ExpandInto를 피한다. AGE VLE도 `age_vle` 함수 호출 비용이 아니라
  `vle-expand`/`expand-into-verification` connector 후보로 모델링해야 한다.
- `AGE VLE Stream`은 이제 `VLE Join Order`를 출력한다. composite terminal property prefilter가 계획되면
  `connector=vle-composite-expand property=index-anchored`, directory label fanout/cache seed/empty lifecycle/payload
  feedback이 있으면 `property=vle-frontier-anchored`, 그 외에는 `property=query-order`로 둔다. join hook은 이제
  adjacency 전용 path walker가 아니라 graph expansion walker로 동작하며, bound VLE path의
  `vle-frontier-anchored`/`index-anchored` property를 joinrel row adjustment 입력으로 읽는다. 조정된 row ratio는
  `NestPath` run cost에도 반영한다. PostgreSQL core는 `add_paths_to_joinrel()` hook 뒤 joinrel에
  `set_cheapest()`를 호출하므로, 이 cost 조정은 단순 EXPLAIN surface가 아니라 join method/path 선택에 들어간다.
  regression에서는 adjacency directory evidence가 있는 shape가 outer nested-loop+materialize 대신 hash join으로
  바뀌어 cost surface가 실제 선택을 바꿀 수 있음을 보여준다. 아직 full alternative enumeration은 아니지만 VLE source
  evidence가 scan-local EXPLAIN line을 넘어 PostgreSQL join path cost surface에 들어가기 시작했다.
- `AGE VLE Stream` base path도 marker `Values Scan`의 rows=1/cost*0.8 wrapper로 두면 join order 단계가 VLE fanout을
  볼 수 없다. 그래서 VLE CustomPath 생성 시 edge-source descriptor의 active direction fanout, finite upper depth,
  composite prefilter fanout, materialization weight를 읽어 rows/cost를 산정한다. zero frontier는 depth multiplier를
  적용하지 않고 minimum 1 row로 clamp한다. `VLE Join Order rows`는 이제 이 base path cardinality를 보여주므로,
  fixture plan에서도 `rows=4`, `rows=24`처럼 fanout/depth signal이 드러난다.
- 양 endpoint argument가 모두 제공된 VLE는 Neo4j의 `ExpandInto`에 대응하는 verification connector다. AGE는 이를
  `vle-expand-into`/`vle-composite-expand-into` connector와 `expand-into-verification` property로 출력한다. NULL const
  endpoint는 bound로 보지 않고, runtime endpoint는 surrounding plan에서 공급되는 runtime id로 본다. 이 property도
  graph expansion join hook의 anchored family에 포함되어 Cartesian product 뒤 검증 비용과 한쪽 endpoint ExpandAll
  비용을 분리할 수 있는 surface가 된다.
- ORCA `CJoinOrderDPv2`의 핵심은 DP 자체보다 `SGroupInfo`가 atom bitset, cardinality, 여러
  `SExpressionInfo`를 함께 들고, 각 expression이 `SExpressionProperties` bitmask와 cost를 갖는다는 점이다.
  `AddExprToGroupIfNecessary()`는 새 expression이 기존 property보다 싼지, 또는 새 property를 제공하는지 보고
  group에 남긴다. AGE의 다음 descriptor는 `Path` 하나의 cheapest 결과만 설명하면 부족하다. 같은 graph component에
  대해 `query-order`, `index-anchored`, `adjacency-directory-anchored`, `vle-frontier-anchored`,
  `expand-into-verification` 후보를 property별 row/cost와 함께 보존해야 PostgreSQL path 생성 또는 AGE-side
  enumerator가 후보를 버리기 전에 비교할 수 있다.
- Neo4j `expandSolverStep`는 solved plan이 한 endpoint를 포함하면 양쪽 방향의 `ExpandAll` 후보를 만들고, 이미
  relationship 또는 양 boundary node가 solved scope에 있으면 `ExpandInto` 후보를 만든다. 이후
  `preFilterCandidatesByIntoVsAllHeuristic`와 `plan_var_expand_into=SINGLE_ROW|MINIMUM_COST`가 Cartesian product 뒤
  verification으로 밀리는 나쁜 shape를 줄인다. AGE에 그대로 옮길 실행 기준은 “두 endpoint가 bound라는 이유만으로
  항상 싸게 보지 말고, source component rows가 작거나 property/node seek로 먼저 좁혀진 경우 `expand-into-verification`
  후보를 강하게 둔다”이다.
- Neo4j `ComponentConnectorPlanner`는 disconnected component 연결에서 value hash/merge join, apply, cartesian
  connector를 별도 solver step으로 합성한다. AGE는 fixed MATCH/VLE를 relation scan 내부 기술로만 두면 이 단계에
  끼지 못한다. `AGEGraphJoinComponent`는 available/provided variable bitset, required outer relids, solved
  predicates, output width를 들고, `AGEGraphJoinConnector`는 adjacency expand, VLE expand, ExpandInto verification,
  node/property value join, cartesian/apply를 같은 후보 목록에 넣는 쪽이 맞다.
- 현재 코드의 `adjust_graph_expansion_join_rows()`는 PostgreSQL join hook에서 이미 `CustomPath` descriptor를 읽어
  row/cost를 조정한다. 이것은 bridge로 유지하되 최종 구조는 hook에서 path tree를 사후 보정하는 방식이 아니라,
  MATCH lowering 직후 graph component table을 만들고 `add_adjacency_match_custom_path()`와
  `add_age_vle_stream_custom_path()`가 같은 candidate table entry를 소비하는 방식이어야 한다. 이때 EXPLAIN의
  `Adjacency Join Order`/`VLE Join Order` line은 candidate table에서 선택된 physical connector를 보여주는 결과물로
  남긴다.
- PostgreSQL parallel scan/join은 graph 후보 테이블에 별도 physical property로 들어가야 한다.
  `set_plain_rel_pathlist()`는 `rel->consider_parallel`이고 parameterized outer가 없을 때
  `create_plain_partial_paths()`로 parallel seqscan partial path를 만들고, extension hook도 `add_partial_path()`를
  호출할 수 있다. `generate_gather_paths()`는 `partial_pathlist`의 cheapest partial path 위에 `Gather`를, pathkeys가
  있으면 `Gather Merge`를 얹는다. joinrel도 양쪽 rel의 `consider_parallel`이 true이고 restrictlist/target이
  `is_parallel_safe()`이면 `consider_parallel`이 true가 되며, nestloop/hashjoin partial path가
  `add_partial_path()`로 들어간다.
- AGE CustomPath도 parallel 후보를 내려면 `parallel_safe`, `parallel_aware`, `parallel_workers`를 채우고
  shared-state/lifecycle contract를 명시해야 한다. `AGE Adjacency Match`는 endpoint key별 posting run scan이
  independent하고 read-only라서 partial path 후보가 비교적 자연스럽다. 반면 `AGE VLE Stream`은 traversal visited
  state, path uniqueness, output order, payload cache replay/suppression이 worker-local인지 shared인지가 semantic을
  좌우하므로 바로 parallel-aware로 켜면 안 된다. candidate table에는 `parallel-safe`, `parallel-aware`,
  `parallel-workers`, `gather-cost`, `shared-state-required`, `order-preserving` 같은 필드가 필요하다.
- ORCA는 PostgreSQL의 worker/Gather와 같은 모델은 아니지만 `CPhysicalScan`이 derived distribution을 갖고,
  hash join이 child distribution requirement를 계산하며, `CPhysicalMotionGather`/broadcast/hash distribute/random
  같은 Motion operator로 병렬/분산 실행 비용을 plan property로 다룬다. AGE는 single-node PostgreSQL extension이므로
  Motion을 그대로 옮기기보다, graph component candidate에 distribution/order/parallel safety property를 두고
  PostgreSQL partial path와 Gather/Gather Merge 비용으로 낮추는 것이 맞다.
- Neo4j parallel runtime 조사에서는 `BatchedParallel` execution model과 `parallelRepeatHeuristic`이 핵심이다.
  특히 `RepeatToVarExpandRewriter`의 `PreferRepeatForBetterParallelizationWhenInputCardinalityIsExactlyOne`은 parallel
  runtime이고 LHS가 `AtMostOneRow`이며 nested Apply 아래가 아닐 때 Repeat를 VarExpand로 접지 않는다. AGE VLE도
  source component가 single-row로 고정되는 경우 parallel-friendly Repeat/VLE stream 후보와 compact VarExpand 후보를
  둘 다 보존하고, source cardinality와 state sharing 비용으로 선택해야 한다.
- Citus는 distributed plan을 PostgreSQL planner/executor와 섞는 extension 구조 참고점이다.
  `CreateCitusCustomScanPath()`는 set relation hook에서 `CustomPath`를 만들고, 선택되면
  `CitusCustomScanPathPlan()`이 executor용 `CustomScan`을 돌려준다. `FinalizePlan()`은 executor 종류
  adaptive/sorted-merge/delayed-error를 고른 뒤 `DistributedPlan`을 `CustomScan.custom_private`에 싣는다.
  AGE도 planner-only `AGEGraphJoinCandidate`와 executor-visible VLE/adjacency descriptor를 분리해야 한다.
- Citus `multi_explain.c`는 `Task Count`, `Tasks Shown`, received tuple bytes, task placement EXPLAIN을
  `CustomScanState`의 runtime `DistributedPlan`에서 꺼내 출력한다. AGE의 `AGEGraphJoinConnector`도 선택 당시
  row/cost/property만 보존하고 끝내지 말고, VLE source runtime, adjacency payload scan/replay, future parallel
  worker/source partition evidence를 EXPLAIN에서 같은 vocabulary로 이어야 한다.
- Citus `CreateCitusCustomScanPath()`가 sorted merge일 때 pathkeys를 설정해 PostgreSQL이 상위 Sort를 생략하게
  하는 점도 중요하다. AGE graph connector 후보의 `order-preserving` 필드는 단순 주석이 아니라, future
  `Gather Merge`, ordered property projection, path output ordering contract와 연결될 physical property로 유지한다.
- Citus처럼 path에서 정한 physical property를 plan 단계에서 다시 덮어쓰지 않는 것이 중요하다. fixed
  `AGE Adjacency Match`는 const endpoint로 unparameterized가 된 경우 `CustomPath`의 `parallel-safe`와
  `required_outer`를 선택된 path 기준으로 graph join candidate와 `CustomScan` plan에 전달한다. 아직
  worker별 posting run partition contract가 없으므로 `parallel-aware`/partial path는 열지 않고, 안전성 metadata만
  planner/executor boundary에서 보존한다.
- `AGEGraphJoinCandidateTable`을 planner-only 구조로 추가했다. 현재 VLE와 fixed adjacency는 각각 하나의 후보만
  등록하지만, `CustomPath.custom_private`에는 table에서 선택된 후보 descriptor만 내려간다. 이 경계는 Citus의
  planner-side `DistributedPlan` 선택과 executor-visible `CustomScan` payload 분리와 같은 이유로 필요하다. 다음
  단계에서는 같은 table에 node/property index seek, bound `age_adjacency`, VLE `ExpandAll`/`ExpandInto` 후보를
  같이 넣고 cost/parallel/order property로 선택한다.
- `AGEGraphJoinCandidateTable`에는 이제 `Path` 기반 등록 API가 있다. VLE와 fixed adjacency는 rows/cost/parallel/order
  property를 `CustomPath`에서 직접 읽어 candidate를 만들고, join-order walker는 parameterized `IndexPath`와
  `BitmapHeapPath`/bitmap tree를 `index-anchored` graph 후보로 해석한다. 따라서 node/property index seek는
  adjacency `CustomPath` descriptor에 거짓으로 섞이지 않고, 실제 PostgreSQL index path 자체가 graph join-order
  vocabulary에 들어간다.
- VLE candidate table은 bound endpoint shape에서 `ExpandInto` 후보와 같은 executor로 가능한 `ExpandAll` fallback을
  함께 등록한다. fallback은 같은 rows/cost base에서 작은 verification penalty를 더해 기본 선택을 기존
  `ExpandInto`로 유지하되, candidate table이 단일 descriptor wrapper가 아니라 대안 후보 list라는 contract를 갖게
  한다.

## FalkorDB GraphBLAS 조사

- `/Users/emotionbug/IdeaProjects/postgres_proj/FalkorDB`는 GraphBLAS/SuiteSparse를 핵심 dependency로 빌드하고,
  graph relation/label을 `Delta_Matrix`로 유지한다. `Build_Matrix()`는 relation matrix들을 union하고 label matrix
  `L`을 이용해 `A = L * A * L` 형태로 label-constrained adjacency matrix를 만든다. `Delta_mxm()`은 base matrix와
  delta-plus/delta-minus를 분리해 `GrB_mxm` 후 mask/addition을 적용한다.
- 일반 MATCH traversal도 algebraic expression을 만든다. `CondTraverse`/`ExpandInto`는 input record batch를 filter
  matrix `F`로 만들고, `F * R` 또는 `F * expression`을 평가한 뒤 tuple iterator로 결과를 소비한다. 단일 relation
  `ExpandInto`는 relation matrix를 직접 검사하는 shortcut도 둔다.
- AGE에 GraphBLAS를 그대로 링크하는 것은 당장 우선이 아니다. PostgreSQL extension ABI, build portability,
  MVCC visibility, heap TID/label catalog invalidation, per-query memory context와 GraphBLAS global/JIT lifecycle이
  충돌한다. 특히 AGE는 `age_adjacency` AM과 heap visibility를 source cursor에서 검증하므로, matrix만으로 tuple
  visibility와 property payload를 완전히 대체할 수 없다.
- 그러나 FalkorDB의 batch filter matrix 아이디어는 VLE 큰 fan-out에서 쓸 수 있다. AGE는 `AGEGraphJoinCandidateTable`에
  `matrix-frontier-expand` 후보를 추가하고, 일정 frontier size 이상에서 여러 source vertex를 CSR-like temporary
  frontier block으로 묶어 `age_adjacency` posting run을 batch intersection/merge한다. GraphBLAS는 1차 목표가 아니라
  optional backend로 두고, 먼저 PostgreSQL-native sparse frontier representation을 만든 뒤 `EXPLAIN`에
  `source=matrix-frontier`/`backend=native|graphblas`를 노출하는 방향이 안전하다.

## PostgreSQL planner/executor boundary

- PostgreSQL 18 source 기준 `CustomPath`/`CustomScan`은 path target, custom expr,
  qual, reparameterization contract를 명확히 맞춰야 planner와 executor가 안정적으로
  상호작용한다.
- PostgreSQL 18 `src/backend/commands/explain.c`의 `ExplainCustomChildren()`는
  `CustomScanState.custom_ps`만 순회한다. `CustomScan.custom_plans`를 plan node에 보관하는 것만으로는
  EXPLAIN child가 보이지 않고, provider가 `BeginCustomScan`에서 child plan을 `ExecInitNode()`로
  `custom_ps`에 넣어야 한다.
- `src/backend/optimizer/plan/createplan.c`는 `CustomPath.custom_paths`를 재귀적으로 `Plan`으로 바꿔
  provider의 `PlanCustomPath(..., custom_plans)`에 넘긴다. 따라서 VLE처럼 marker input plan을 보존하려면
  planner 단계에서 `reference_path`를 `custom_paths`에 싣고, plan 단계에서 `CustomScan.custom_plans`로
  전달해야 한다.
- DML CustomScan은 PostgreSQL core child display가 `outerPlanState()`를 보는 경로를 이미 사용한다.
  반면 relation-backed CustomScan은 scan relation 자체가 input이므로 child plan을 억지로 만들지 않는다.
  VLE marker stream은 relation-backed scan이 아니고 실제 reference path가 있으므로 `custom_paths/custom_ps`
  contract를 쓰는 것이 PostgreSQL explain 구조와 맞다.
- AGE Cypher expression을 PostgreSQL 표준 expression으로 더 많이 낮출수록 plan cache,
  index matching, invalidation, costing을 PostgreSQL에 맡길 수 있다.
- ORCA `CCostModelPG`/`CCostModelGPDB`는 hash aggregate와 compute scalar 비용을 operator 자체의 존재 여부만으로
  계산하지 않고 input/output rows와 width를 함께 소비한다. AGE의 typed collect/property aggregate rewrite도
  final `agtype` materialization을 lower typed slot으로 미루는 구조라면, credit이 slot 개수 상수에 머물면 큰
  dataset에서 row-width/materialization 절감 효과를 planner가 보지 못한다. 따라서 final materialization weight는
  aggregate input rows와 `cpu_operator_cost`로 스케일하되, 작은 fixture에서는 기존 base weight credit을 유지하는
  방향이 맞다.
- aggregate slot-vector width estimate는 short synthetic scalar에서는 실제 runtime state width와 맞출 수 있지만,
  varlena payload에서는 typmod/statistics/sample evidence 없이 정적 type 상수만으로는 부족하다.
  `typed-wide-text-aggregate` benchmark는 `wide_text_width=64`에서 `numeric,text` value width가 row당 84 bytes이고
  descriptor estimate가 32 bytes라 `slot_value_estimate_ratio=2.63`으로 벌어졌다. 이 결과는 `text` 상수를 단순히
  올리라는 신호가 아니라, ORCA의 width-sensitive cost처럼 property signature가 typmod, pg statistics, 또는 benchmark
  profile에서 가져온 expected width를 별도 descriptor로 들고 planner credit에 전달해야 한다는 근거다.
- slot-vector aggregate executor는 partial aggregate를 지원하므로 state layout을 넓히면 serialize/deserial contract까지
  같이 바뀐다. 첫 단계에서는 typed scalar Datum을 state에 직접 저장하지 않고 transition 시점에 `agtype` slot으로
  정규화해 기존 partial aggregate layout을 유지한다. 이 경계가 있어야 다음 단계에서 slot별 value type 배열과
  cached-property descriptor를 state header로 올릴 때 regression surface를 분리할 수 있다.
- 다음 단계의 planner handoff가 slot별 cached-property descriptor를 넘기려면 executor state가 최소한 value type
  vector를 보존해야 한다. 그래서 state payload는 `agtype`으로 유지하면서 header에 original value type OID를 넣고,
  combine은 type mismatch를 오류로 본다. 이는 ORCA의 physical property가 child output column metadata를 보존하는
  것과 같은 방향이며, typed slot payload 자체를 저장하는 더 큰 layout 변경 전에 partial aggregate contract를 먼저
  안정화한다.
- typed map/list aggregate에서 early rewrite가 `{a: n.x::pg_numeric}`를 `(properties, key[])` AGTYPE-only aggregate로
  바꾸면 property signature의 value/result type이 사라진다. original `array_agg` expression에서 map/list value
  expression을 다시 읽어 slot descriptor를 만들면 cached-property slot metadata가 lower target까지 유지된다. 이는
  aggregate 함수 나열을 늘리는 방식이 아니라 existing variadic slots aggregate가 typed argument vector를 받도록
  planner handoff를 넓히는 방향이다.
- slot-vector payload를 typed Datum으로 유지하면 final `agtype` materialization을 aggregate row마다 미리 수행하지
  않는다. PostgreSQL aggregate state는 internal state라 serialize/deserial까지 같은 layout을 읽어야 하므로,
  지원 범위는 현재 planner가 내리는 `agtype`/`int8`/`float8`/`numeric`/`text`로 제한하고 unsupported type은 오류로
  막는다. 이 제한은 임의 함수 나열을 늘리는 것이 아니라 cached-property scalar slot domain을 명시하는 contract다.
- `internal` aggregate state는 SQL에서 직접 조회할 수 없으므로, header evidence는 summary final function으로
  노출한다. map/list summary는 같은 transition/combine/serialize contract를 공유해 partial aggregate boundary의
  provided physical property를 regression output에 드러낸다. summary는 payload 값을 materialize하지 않고 shape, slot
  count, row count, typed/agtype slot count, payload materialization weight, value type vector만 출력한다.
- varlena typed payload는 transition state에 flat detoast copy로 저장해야 partial aggregate serialize가 compressed
  toast pointer layout에 묶이지 않는다. typed payload 자체를 유지하되 wire format은 extension state가 소유하는 flat
  varlena bytes로 고정해야 다음 cached-property descriptor를 state header와 연결할 때 executor contract가 안정적이다.
- 2-field map은 기존에 별도 `map2` intermediate aggregate로 접히는 경로가 있어 typed slot metadata를 잃기 쉽다.
  map2/map/list 모두 original expression 기반 lowering으로 통일하고 typed signature 판정을 공유해야 같은
  cached-property slot descriptor family로 볼 수 있다.
- ORCA `CPhysicalIndexScan`은 단순 index OID가 아니라 `CIndexDescriptor`를 들고 required/output column
  contract와 index identity를 같이 유지한다. Neo4j relationship index seek/scan provider도 logical plan 단계에서
  descriptor와 predicate compatibility를 함께 다룬다. AGE `AGE Adjacency Match`도 `ag_graph_index` metadata를
  source 문자열로만 남기면 다음 property/composite index handoff가 다시 catalog scan 또는 문자열 파싱에 묶인다.
  따라서 graph index metadata의 kind/provider/direction/property surface를 CustomScan descriptor로 올리고,
  EXPLAIN에서 summary와 descriptor/pruning을 분리해 raw plan surface가 index handoff contract를 보여주게 한다.
- right terminal property predicate는 adjacency payload scan 자체로 바로 해결되지 않는다. 하지만 Neo4j식
  relationship index seek도 relationship expansion과 endpoint predicate compatibility를 logical plan descriptor에서
  함께 보존한다. AGE도 terminal node의 top-level property key와 graph property index metadata를
  `Adjacency Terminal Property` descriptor로 올리면, 다음 단계에서 endpoint vertex lookup, label+property composite
  request, 또는 join order/cost 보정이 같은 metadata를 소비할 수 있다. 현재 단계에서는 실제 row 수를 줄이지 않고
  deferred recheck penalty만 조정한다. 이는 executor가 아직 property index seek를 수행하지 않는 상태에서 잘못된
  cardinality를 만들지 않기 위한 경계다.
- VLE terminal property prefilter는 endpoint-btree scan이 아니라 `age_adjacency` payload scan에서 실행된다. 따라서
  composite fanout을 endpoint-btree fanout 자체로 낮추면 source 선택 의미가 틀어진다. ORCA `CPhysicalIndexScan`의
  residual predicate count처럼 index/source가 실제로 해결하는 predicate만 해당 physical operator의 property로
  둬야 하고, Neo4j `RelationshipIndexSeekPlanProvider`가 index-compatible predicate와 hidden selection을 분리하는
  방식처럼 AGE도 `VLE Composite Fanout`과 `VLE Source Policy`를 분리해야 한다. 결론적으로 VLE source policy는
  `endpoint-work`를 그대로 보존하고, `age_adjacency` payload source가 prefilter를 실행할 수 있을 때만
  `composite-work=planned`와 `adjacency-composite-prefilter` class를 출력한다.
- fixed `AGE Adjacency Match`는 같은 property source metadata를 쓰더라도 small terminal slice에서는 property source
  scan보다 id cache verification이 더 좋은 lifecycle이다. 따라서 VLE와 class family는 공유하되
  `adjacency-composite-id-cache`와 `adjacency-composite-prefilter`를 분리한다. 이 분리는 ORCA의 residual predicate
  count와 Neo4j hidden selection 분리처럼 “metadata가 있다”와 “source operator가 predicate를 해결한다”를 구분하기
  위한 것이다. `Adjacency Composite Policy`의 `class-match`는 threshold 조정 전에 planned lifecycle과 runtime
  outcome이 같은 family인지 먼저 확인하는 evidence다.
- 단순 helper rewrite가 expression index surface를 바꾸면 no-index workload는 빨라져도
  indexed workload가 seq scan으로 떨어질 수 있다. 그래서 property predicate rewrite는
  index-aware guard를 둔다.
- nested typed property index에서는 catalog expression surface와 reconstructed helper chain이 같은
  property signature를 가리켜도 PostgreSQL expression index matching 관점에서는 다른 expression이 될 수
  있다. 따라서 canonicalization entry는 ORCA식 descriptor matching 근거와 catalog expression surface를
  함께 보관하고, restriction/predicate rewrite는 rebuilt helper chain이 아니라 catalog surface를
  canonical expression으로 재사용해야 한다.
- ORCA식 index scan은 required output column과 residual predicate를 분리해 child physical property로 전달한다.
  Neo4j relationship expansion도 terminal node label/property predicate를 relationship expansion 이후의 단순
  join filter로만 두지 않고 pruning 가능한 descriptor로 보존한다. AGE의 `age_adjacency` main run block은
  이미 `next_label_id`를 갖고 있으므로, right-label constraint는 heap recheck 전 payload filter가 아니라
  block-level source pruning contract로 올리는 것이 맞다. property predicate는 별도 terminal property index
  lookup으로 남기되, label pruning은 adjacency posting cache 생성 전에 소비해 cache 폭 자체를 줄인다.
- 같은 근거로 `age_adjacency` directory entry에도 endpoint run의 `next_label_id` min/max summary를 둔다.
  terminal label이 summary range 밖이면 main block을 열기 전에 endpoint run 전체를 skip할 수 있다. 이는 SQL-visible
  `(endpoint_id, edge_id, next_vertex_id)` contract를 바꾸지 않으면서 index data 자체가 label 후보 축소에 쓰이게
  하는 변경이다. Neo4j식 label/type aware expansion이 row join 전에 후보 폭을 줄이는 것과 같은 방향이며,
  ORCA의 physical property처럼 planner/executor가 source pruning evidence를 `directory-label` counter로 공유한다.
- directory label summary는 단일 scan의 payload filtering에만 머물면 효과가 작다. VLE frontier source completion도
  같은 index summary를 known-empty probe로 소비해야 source object 생성 전에 반복 empty expansion을 줄일 수 있다.
  따라서 `age_adjacency_visible_payload_scan_key_known_empty()`는 no-delta index에서 endpoint run miss뿐 아니라
  terminal label range miss도 known-empty로 취급한다. 이는 graph index data가 payload row pruning과 source
  lifecycle pruning을 같은 contract로 제공하는 방향이다.
- terminal property index prefetch는 ORCA의 provided physical property에 가깝다. matching terminal vertex id set이
  이미 executor begin 단계에서 materialize됐다면, 이를 join 후 residual predicate로 다시 확인하는 것은 source
  boundary를 늦게 소비하는 것이다. AGE는 이 set을 `age_adjacency` visible payload scan의 vertex filter callback으로
  전달해 heap visibility/property fetch 전에 버릴 수 있다. Neo4j의 index-backed seek가 candidate entity id set을
  먼저 좁힌 뒤 expansion/runtime row를 만드는 방향과도 맞다. non-prefetch fallback은 여전히 per-vertex id btree
  lookup이 필요하므로 source prefilter로 승격하지 않는다.
- 다만 matching set을 vertex filter callback으로 넘기는 것만으로는 adjacency posting fetch 자체가 줄었다고 볼 수
  없다. ORCA식 physical property 관점에서는 provided candidate id set 폭과 source operator가 실제 읽은 posting 폭을
  분리해서 봐야 한다. 그래서 VLE runtime은 `prefetch-matches`를 `property-prefilter` counter와 함께 출력한다.
  이 evidence에서 matched set이 terminal label candidate보다 충분히 작으면 다음 구조 변경은 guard 추가가 아니라
  label+value composite request를 adjacency scan key/cache key로 내려 posting fetch boundary를 더 앞당기는 것이다.
- cache key도 physical property의 일부다. property-prefilter가 적용된 payload cache를 terminal label cache와 공유하면
  다른 predicate value의 traversal이 filtered payload나 known-empty result를 재사용할 수 있다. 따라서 VLE payload
  cache key는 property index oid와 predicate value image hash를 포함한 filter identity를 갖는다. prefetch matched
  set이 empty일 때 scan을 열지 않고 source completion으로 접는 것은 posting order를 아직 바꾸지 않아도 source/cache
  lifecycle이 property predicate를 소비한다는 첫 단계다. 다음 단계는 이 identity를 단순 hash key에 머물게 하지 않고,
  adjacency posting iterator가 property matched vertex id set 또는 label+value composite directory를 직접 소비하게
  하는 것이다.
- payload feedback도 ORCA식 physical property처럼 class name 하나만이 아니라 lifecycle evidence 전체를 봐야 한다.
  `adjacency-composite-value-posting`은 property value pruning이 가장 강한 class지만, 같은 cache entry에 replay 또는
  cache seed evidence가 함께 쌓이면 source headroom과 empty batch는 replay/seed lifecycle을 소비해야 한다. 따라서
  planner profile은 payload class가 value-posting으로 남아도 `replay_runs`/`seed_runs`를 읽어 headroom을 낮추고,
  class rank는 pruning identity를 보존하는 데만 사용한다. 서로 다른 terminal filter/profile key에서 나온 value
  pruning과 replay를 강제로 합치지는 않는다.
- 깊이 있는 VLE의 terminal label/property predicate는 all-depth source constraint와 terminal-only end constraint를
  구분해야 한다. fixed label chain rewrite처럼 모든 중간 vertex label이 같은 경우는 source cursor 전체에 label
  pruning을 적용해도 되지만, 일반 `*1..N`의 `(n:Label)`/`WHERE n.k = v`는 마지막 emitted endpoint에만 적용해야 한다.
  marker를 무조건 일반화하면 property prefilter가 depth 1/2 중간 vertex까지 잘못 요구해 path semantic을 깨뜨린다.
  따라서 `VLEContextSourceCursor`는 `target_path_length`를 갖고, terminal property filter id/cache identity는 upper
  bound에 도달하는 expansion에서만 켜지는 contract가 필요하다.
- fixed label-chain collapse는 all-depth marker를 유지하므로 `age_adjacency` local source/cache seed와 잘 맞는다.
  `vle_fixed_label_chain` regression에 adjacency index를 붙이면 endpoint btree fallback 대신
  `adjacency-cache-seeded` profile이 선택된다. named terminal property가 붙은 explicit chain은 마지막 terminal
  binding을 보존해야 VLE terminal output으로 retarget된다. 또한 direct terminal-property output은 path container를
  내보내지 않으므로 marker child plan의 residual property predicate를 제거해야 한다. source prefilter identity는
  path length가 final vertex expansion을 가리킬 때만 붙여야 하며, local adjacency payload는 skeleton vertex entry를
  운반해야 label-row fallback property lookup이 가능하다. 이 구조가 갖춰진 뒤에야 value-posting/replay evidence를
  같은 all-depth source profile key에 묶을 수 있다.
- 추가 regression 확인 결과, variable range의 terminal node label/property는 upper depth 하나만의 constraint가
  아니다. `*1..N`은 각 depth의 emitted endpoint를 검사해야 하며, label/property가 맞지 않는 중간 depth candidate도
  더 깊은 path로 확장될 수 있다. 그러므로 terminal join을 제거하는 parser marker는 exact finite range에서만 안전하다.
  variable range를 최적화하려면 source pruning과 terminal acceptance를 같은 predicate로 묶지 말고, DFS step
  descriptor가 `accept if endpoint predicate matches`와 `continue expansion regardless of endpoint predicate`를
  분리해야 한다.
- `cache-filter=total/label/property`는 residual predicate count가 아니라 cache construction boundary의 physical
  evidence다. `property-filtered`만 보면 최종 tuple 후보에서 얼마나 버렸는지 알 수 있지만, `cache-filter=6/0/6`은
  property matched set이 heap visibility/property fetch 전에 main cache fill 폭을 줄였음을 보여준다. ORCA식
  physical property로 보면 아직 block/window seek order는 바꾸지 못했지만, source operator 내부의 provided filter가
  materialized payload width를 줄인 상태다. 다음 연구/구현은 matched vertex id set을 sorted/range descriptor로 바꿔
  main run block traversal 또는 directory entry에서 더 일찍 skip할 수 있는지를 봐야 한다.
- callback은 source operator의 physical property를 숨긴다. `AgeAdjacencyVertexSetFilter`는 property source index가
  만든 matched vertex id set을 `age_adjacency` scan target의 typed descriptor로 올린다. 이 단계는 hash lookup
  자체는 유지하지만, VLE와 fixed MATCH가 같은 descriptor를 전달하므로 다음 변경에서 hash set을 sorted id/range
  descriptor나 directory-compatible composite summary로 교체할 수 있는 boundary가 생긴다.
- min/max range는 sorted descriptor의 첫 물리 property다. property index scan 결과의 vertex id range를
  `AgeAdjacencyVertexSetFilter`에 싣고, `age_adjacency` scan이 hash lookup 전에 range miss를 버리면 source operator가
  matched set의 ordered summary를 소비하기 시작한다. `set-range-filter=6`은 아직 block read를 줄인 값은 아니지만,
  다음 단계에서 main run block의 destination vertex id range 또는 directory composite summary와 join할 수 있는
  runtime evidence다.
- terminal property value도 constant descriptor에만 갇히면 ORCA식 required expression handoff가 아니다.
  `CustomScan.custom_exprs` value slot으로 넘기면 setrefs/evaluation은 PostgreSQL executor contract를 따르고,
  terminal property lookup은 value setter로 prefetch set을 갱신할 수 있다. 이번 단계는 `1 + 0`처럼 current
  scan context에서 평가 가능한 expression을 source prefilter로 내린다. 이전 clause variable을 참조하는
  `m.i` shape는 현재 Cypher subplan target list 경계에서 막히므로, 다음 단계의 outer variable target handoff
  과제로 남긴다.
- composite request는 단순 runtime counter에 머물면 source operator의 physical property가 아니다.
  `AgeAdjacencyCompositeTerminalFilter`의 property source OID/filter id/matched count를 scan target에 보존하고,
  directory begin-key/known-empty 판단이 label/property mismatch를 같은 composite target helper에서 받게 하면
  ORCA식 index descriptor처럼 request identity와 pruning decision이 같은 boundary를 지난다. `composite-directory-filter`
  counter는 기존 vertex-set range pruning 총량과 별도로, label+property composite request가 directory summary를
  실제로 소비한 경우만 드러낸다. 다음 index layout은 이 helper에 directory-level value summary를 추가해
  min/max vertex range보다 더 선택적인 label+property value pruning을 비교해야 한다.
- ORCA `CPhysicalIndexScan`은 `CIndexDescriptor`와 residual predicate count를 함께 들고, Neo4j
  `RelationshipIndexSeekPlanProvider`도 index-compatible predicate와 hidden selection을 분리한다. AGE의
  `composite=request:N/dir-estimate:N`도 같은 방향으로, terminal label 후보 폭은 그대로 두면서 property source
  matched count가 directory/run 후보 폭에 주는 상한을 별도 descriptor evidence로 둔다. 이 estimate는 아직
  directory value posting을 읽은 결과가 아니므로 actual filter count와 분리해야 하고, 다음 단계에서 directory-level
  value summary 또는 posting intersection을 붙일 때 residual predicate가 얼마나 남았는지 비교하는 기준이 된다.
- actual pruning evidence도 같은 descriptor family에 있어야 한다. `set-block-filter` 총량만 있으면 property source
  request가 만든 skip인지, 단순 label/range pruning인지 분리하기 어렵다. `composite=.../block-filter:N`과
  `age_adjacency_debug_composite_probe()`는 sorted matched vertex set이 compact block과 교차하지 않는 경우를 직접
  실행해, future directory value posting이 estimate를 actual skip으로 바꾸는지 비교할 수 있는 기준 surface다.
- directory-level value summary는 exact posting intersection을 바로 만들기 전에 safe-negative filter부터 갖추는 것이
  맞다. v8 directory entry의 `next_vertex_id` bloom은 false positive를 허용하지만 false negative를 만들지 않으므로,
  matched vertex set이 bloom에 전혀 걸리지 않을 때만 main run을 열지 않는다. 이는 ORCA의 residual predicate count처럼
  “index summary가 해결한 negative”와 “아직 block/posting에서 확인해야 하는 residual”을 분리하는 중간 layout이다.
- global bloom은 run 안의 다른 terminal label vertex 때문에 positive가 될 수 있다. v9 directory entry는 작은
  label-local bloom slot을 추가해 terminal label descriptor가 있는 composite request에서 이 false positive를 한 번 더
  줄인다. slot이 없는 label은 safe fallback으로 기존 global bloom/residual filter에 맡기고, slot이 있는 label만
  directory safe-negative로 접는다. 다음 단계의 exact vector 또는 posting intersection도 이 방식처럼 summary가 없는
  경우에는 residual로 떨어지고, summary가 있는 경우에만 main/block traversal을 줄여야 한다.
- v10 directory entry는 small-run exact `next_vertex_id` vector를 추가한다. 이는 bloom을 대체하는 것이 아니라
  small run에서 bloom positive 이후에도 확정 intersection을 확인해 block traversal을 생략하는 두 번째 summary다.
  distinct vertex가 slot을 넘으면 overflow marker로 기존 bloom/residual path를 유지한다. 다음 구조 후보는 property
  source posting intersection을 directory/block layout에 붙여 exact vector가 담지 못하는 큰 run에서도 label+value
  request를 posting fetch 전에 줄이는 것이다.
- v11 main block header는 256-bit `next_vertex_id` bloom을 보관한다. directory summary가 run 전체를 보고 보수적으로
  통과시킨 뒤에도, block summary는 property source matched vertex set과 더 작은 posting window를 교차한다. 이는 아직
  property source posting list를 adjacency index 안에 병합한 것은 아니지만, posting fetch boundary에서 request set과
  block-local adjacency summary를 직접 intersect하는 첫 단계다. 다음 단계는 property source의 matched set을 block bloom
  이후에도 반복 hash probe로만 쓰지 말고, block-local exact/posting vector 또는 property-source posting intersection
  handle로 올리는 것이다.
- v12 main block header는 small block-local exact `next_vertex_id` vector도 보관한다. slot 안의 block은 property
  source matched set과 exact vector를 먼저 교차하므로 bloom false positive를 만들지 않는다. slot overflow block은
  v11 bloom과 posting residual로 내려간다. 따라서 다음 효용 후보는 exact slot을 넘는 큰 block/run에 대해 property
  source posting intersection handle이나 block-local compressed posting vector를 두는 것이다.
- v13 main block header는 `min/max next_vertex_id` range summary도 보관한다. directory range는 run 전체를 덮기 때문에
  label 또는 block 사이의 gap을 놓칠 수 있지만, block range는 같은 property matched set을 훨씬 좁은 posting window와
  교차한다. range negative는 exact vector와 bloom보다 싸고 결정적이므로 block pruning 순서는 range, exact, bloom,
  posting residual이 된다. 다음 후보는 range/exact/bloom을 모두 통과한 큰 block에서 property source posting
  intersection을 payload order와 직접 맞추는 것이다.
- compact main block의 payload-order posting intersection은 on-disk summary가 아니라 residual exact check지만, block
  전체를 cache로 풀기 전에 `next_entry_id`와 sorted matched vertex set을 직접 교차하므로 property source posting
  intersection의 현재 하한선이다. 이 경계를 `set-block-filter/posting:N`으로 분리하면 range/exact/bloom false positive
  뒤에 남은 비용이 얼마나 payload-order scan으로 해결되는지 보인다. 다음 layout은 이 residual을 더 앞당겨 directory
  value summary나 block-local compressed value posting vector가 같은 negative를 payload decode 없이 만들도록 해야 한다.
- v14 directory entry는 기존 64-bit `next_vertex_id` bloom 옆에 256-bit wide bloom을 추가한다. 이는 아직 property
  value 자체를 인코딩한 posting summary는 아니지만, property source가 만든 matched vertex id set을 run directory에서
  더 넓은 signature와 교차해 64-bit false positive를 줄인다. regression은 `_graphid(1, 2)`가 min/max range 안에 있고
  64-bit bloom에는 걸리지만 wide bloom에는 없어서 `set-directory-filter=19/wide-bloom:19`로 main block traversal을
  생략하는 케이스를 고정한다. 다음 단계는 이 wide signature를 terminal value identity별 directory posting summary나
  block-local compressed value posting vector로 바꾸는 것이다.
- v15 main block header는 compact block의 homogeneous `next_label_id`를 이용해 distinct `next_entry_id`를 48-bit
  compressed exact vector로 저장한다. 기존 graphid exact slot은 8개를 넘으면 overflow되지만, compressed vector는 같은
  header budget에서 더 많은 terminal candidate를 exact intersection으로 처리한다. directory summary가 candidate를
  포함해 통과하고 edge-label block 하나만 candidate를 갖지 않는 fixture에서 `set-block-filter=19/compressed:19`가
  발생하므로, bloom false positive와 payload-order posting scan 사이의 residual 비용이 한 단계 더 줄었다. 아직 value
  identity 자체를 저장하지는 않으므로 다음 구조 변경은 property filter id/value identity별 directory 또는 block posting
  summary로 이어져야 한다.

## VLE CustomScan iterator 판단

- `age_vle` 접근이 `FunctionScan`/SRF evaluator에 머무르면 consumer projection, rescan,
  materialization 요구를 traversal state와 직접 연결하기 어렵다.
- PostgreSQL CustomScan은 `custom_exprs`, `custom_private`, scan tuple slot, rescan callback을 통해
  planner가 고른 path shape와 executor-local state를 함께 관리할 수 있다. 따라서 VLE는 SQL SRF를
  완전히 없애기 전 단계라도 traversal loop를 iterator API로 분리하고, CustomScan executor가 직접
  iterator를 구동하는 쪽이 더 나은 중간 경계다.
- `FuncExpr` 전체를 CustomScan executor에 전달하면 executor가 여전히 SQL SRF expression surface에
  묶인다. 더 나은 중간 경계는 setrefs 대상인 argument expressions는 `custom_exprs`로 두고,
  `nargs`, const flag, range/direction 같은 scan metadata만 `custom_private` descriptor로 넘기는 것이다.
  이렇게 해야 function identity 없이 start/end/range/direction/materialization descriptor로 전환할 수 있다.
- executor 내부에서도 argument list를 순서만으로 다루면 descriptor 전환이 어려워진다. graph,
  start/end, edge prototype, range, direction, grammar node, terminal property key를 semantic slot으로
  저장해야 이후 일부 slot을 expression 평가가 아니라 planner-derived descriptor나 cached traversal
  contract로 대체할 수 있다.
- CustomScan executor가 직접 `FunctionCallInfo`를 만들면 다시 PostgreSQL function-call ABI에 묶인다.
  `AgeVLEInput` 같은 fixed-size iterator input descriptor를 두고 fcinfo 변환을 VLE iterator adapter
  내부로 내리면, 다음 단계에서 SQL expression 평가 slot과 planner-derived typed slot을 같은 입력
  contract로 섞을 수 있다.
- VLE call의 graph name, edge prototype, range, direction, grammar node, terminal property key는 많은
  lowered shape에서 `Const`다. 이 slot을 expression state가 아니라 plan-time Datum/null descriptor로
  보관하면 CustomScan state가 일부 input을 이미 typed descriptor처럼 소유하게 되고, 동적인
  start/end vertex slot과도 명확히 분리된다.
- const 여부를 executor가 `IsA(arg, Const)`로 다시 판단하면 CustomScan이 여전히 SQL expression tree
  shape에 의존한다. planner가 argument별 const flag를 만들고 executor가 그 descriptor를 따르게 하면,
  이후 const flag를 start/end source kind, edge prototype kind, materialization requirement 같은 typed
  VLE scan descriptor로 확장할 수 있다.
- CustomScan 전환 regression은 node 이름만 확인하면 함수 wrapper 제거의 실제 근거가 약하다.
  `ExplainCustomScan`에서 shape/range/direction/slot layout을 출력하면 planner가 만든 VLE descriptor가
  expected 파일에 드러나고, hidden assertion 없이 scan contract 변화를 검증할 수 있다.
- range와 direction은 VLE traversal strategy에 직접 영향을 주는 scalar metadata다. 이 값을 executor가
  agtype const에서 다시 파싱하면 scan node가 function argument ABI에 계속 묶인다. planner가
  int/null descriptor로 추출해 `custom_private`에 넘기는 방식은 start/end source kind, edge prototype,
  materialization requirement도 같은 typed descriptor 계열로 옮기기 위한 선행 단계다.
- VLE CustomScan 전환은 descriptor 종류가 늘어날수록 executor 파일에 helper가 빠르게 쌓인다.
  descriptor parsing/explain formatting을 별도 모듈로 분리하면 실행 루프와 planner-derived metadata
  contract를 나눌 수 있고, start/end/edge prototype descriptor를 추가할 때 파일 책임이 덜 섞인다.
- public `age_vle` SRF를 계속 남기면 사용자가 FunctionScan/SRF surface에 의존할 수 있고,
  optimizer도 함수 이름 compatibility를 유지해야 한다. 이 최적화 브랜치에서는 하위 호환성을 gate로
  두지 않으므로 public `age_vle` overload는 제거하고, 직접 SQL regression도 Cypher marker stream
  결과/plan 검증으로 대체한다. terminal-property output은 9-arg SQL function OID lookup 대신 marker
  row의 key slot 확장으로 표현한다. SQL-visible `age_vle_internal` surface와 C SRF wrapper도 제거해
  VLE CustomScan 진입이 marker descriptor에만 의존하게 한다.
- `age_vle_internal` Function RTE를 Cypher lowering의 기본 anchor로 계속 쓰면 함수 ABI와
  `FuncExpr` identity가 planner/executor boundary에 남는다. 더 나은 중간 단계는 stable marker를
  가진 `VALUES` RTE를 Cypher VLE stream descriptor row로 만들고, optimizer가 marker를 확인해
  `AGE VLE Stream` CustomPath를 추가하는 방식이다. 이 구조는 SQL function identity 없이도
  argument expression list를 setrefs 대상에 남길 수 있다.
- `VALUES` marker row의 output placeholder가 plain `NULL::agtype`이면 compact consumer에서
  PostgreSQL이 single-row values를 pull-up/constant-fold 하면서 `age_vle_path_length(NULL)` 같은
  `Result Output: NULL::text` plan을 만들 수 있다. marker는 stable const로 두되 stream output
  placeholder는 volatile agtype expression으로 만들어 CustomScan replacement 전 plan-time folding을
  막는 것이 맞다.
- 예전 direct SQL `age_vle` 7-arg regression은 더 이상 gate가 아니다. CustomScan 전환의 기준
  regression은 Cypher marker stream과 terminal property output shape이며, function ABI 호환보다
  marker/descriptor 기반 VLE scan contract를 우선한다.
- terminal-property output을 SQL function OID retarget으로 구현하면 parser가 catalog function ABI를
  알아야 하고 optimizer도 Function RTE fallback을 유지해야 한다. marker RTE row에 key slot을 직접
  추가하면 terminal-property도 planner-visible descriptor의 한 shape가 되므로, `EXPLAIN (VERBOSE)`
  expected가 `VLE Shape: terminal-property`와 slot layout을 직접 보여줄 수 있다.
- ORCA의 physical property/required column handoff처럼 VLE도 traversal loop가 final projection 세부
  구현을 직접 소유하면 descriptor 전환이 느려진다. terminal scalar/full property output, direct cache,
  block prefetch, batch materialization을 `age_vle_terminal_output`으로 분리하면 DFS/search loop는
  `VLETraversalStep`과 output policy만 전달하고, output module이 final target write와 terminal
  materialization contract를 소유한다.
- `properties(n)`처럼 terminal vertex의 전체 properties object가 최종 출력인 경우도 SQL wrapper로
  돌아가지 않는다. scalar terminal property와 같은 marker slot을 쓰되 key `Const`가 NULL이면
  `AGE_VLE_OUTPUT_REQUIREMENT_TERMINAL_PROPERTIES`로 해석해 full-properties descriptor가 된다.
  scalar key slot과 full-properties slot은 같은 물리 slot을 공유하지만 requirement가 다르므로,
  parser retarget은 이미 scalar key가 들어간 marker row를 full-properties consumer와 섞지 않는다.
  이 방식은 호출자별 guard를 늘리는 대신 output descriptor semantic을 넓혀 `EXPLAIN` plan evidence에
  `terminal-properties` shape를 직접 남긴다.
- `age_vle_internal` SQL surface 제거 뒤 `AgeVLEInput`에서 내부 `FunctionCallInfo`를 재구성하던
  adapter도 제거했다. `VLE_local_context` builder는 이제 `AgeVLEInput` slot을 직접 읽는다. 다만 각
  slot의 semantic parsing은 아직 agtype argument layout을 공유하므로, 다음 단계는
  graph/start/end/edge/range/direction/terminal-property typed slot descriptor를 builder가 직접 받게
  하는 것이다.
- range/direction은 planner가 이미 const agtype에서 int/null descriptor로 추출할 수 있는 metadata다.
  이 값을 `AgeVLEInput`에 싣고 `build_local_vle_context()`가 known slot에서 직접 사용하게 하면
  SQL function argument ABI에 남아 있던 parsing 의존이 줄어든다. dynamic slot은 fallback parser를
  유지하되, Cypher marker stream의 일반 shape는 planner-derived traversal descriptor를 우선한다.
- grammar-node numeric id는 실행 구조상 cache key로 필요하지만 regression/explain contract로는
  안정적인 의미 값이 아니다. output descriptor는 내부 numeric 값을 traversal input으로 넘기되,
  `EXPLAIN`에는 `cached`/`terminal-only` semantic marker와 terminal-property key를 출력하는 쪽이
  plan evidence로 더 적합하다.
- graph name은 모든 Cypher marker VLE에서 planner가 안정적으로 볼 수 있는 const string이다. graph oid
  lookup과 global graph load decision의 시작점이므로, graph descriptor를 먼저 traversal input으로
  넘기면 start/end runtime slot보다 적은 위험으로 SQL function argument ABI 의존을 줄일 수 있다.
- edge prototype은 단순 label 문자열만이 아니라 property constraint 존재 여부, property count,
  targeted edge label load 여부를 동시에 결정한다. planner가 edge const에서 label/properties descriptor를
  만들면 `build_local_vle_context()`가 global graph load decision 전에 edge agtype container를 다시
  열 필요가 줄고, 이후 cached-property/index handoff를 같은 descriptor에 붙일 수 있다.
- start/end는 planner 단계에서 항상 graphid를 알 수 있는 값이 아니라 outer Var와 subplan runtime value를
  포함한다. 따라서 planner-only descriptor보다 executor evaluation 직후 typed endpoint로 정규화하는
  contract가 맞다. traversal builder에서 agtype을 다시 열지 않게 하면 cached context refresh와 새 context
  creation이 같은 endpoint handoff를 공유할 수 있고, 다음 단계에서 adjacency prefetch/load decision으로
  직접 넘기기 쉽다.
- cached context refresh도 `VLEContextRefreshInput`을 통해 start/end validity와 graphid를 받는다. refresh
  helper는 traversal root 갱신, reverse paths-to root, reverse-output swap, terminal-property scratch reset을
  한 경계에서 처리하므로, 새 context setup/apply contract와 cached refresh contract가 같은 endpoint
  descriptor 계열로 맞춰진다.
- traversal root 자체는 `VLETraversalRootDescriptor`로 분리했다. 새 context setup은 descriptor를 만든 뒤
  apply하고, cached refresh는 기존 root descriptor를 갱신한 뒤 같은 apply helper를 탄다. 이 구조가 있어야
  paths-to reverse root, cheaper endpoint root swap, cached rescan root advancement를 별도 ad hoc field
  mutation이 아니라 공통 root contract로 확장할 수 있다.
- adjacency/index source layout도 `VLETraversalSourceLayout`으로 분리했다. candidate expansion이
  `VLE_local_context`의 out/in age_adjacency OID와 endpoint btree OID를 직접 조합하면 skeletal endpoint
  fallback과 normal vertex expansion이 서로 다른 source contract를 갖게 된다. source layout은 이제
  outgoing/incoming 방향별 `VLETraversalDirectedSource`를 담고, root apply/index refresh 시점에
  age_adjacency 우선, endpoint-btree fallback을 source kind와 index OID로 고정한다.
- runtime source layout은 `VLETraversalRootDescriptor`가 소유하고 root apply가 `VLE_local_context`의 current
  layout을 갱신한다. expansion 때 context field를 다시 조합하지 않고 fixed source kind만 dispatch하는 것이
  중요하다. cached index refresh는 같은 layout builder로 current context layout만 갱신하므로, 새 root selection
  path와 cached refresh path는 source contract를 root descriptor boundary에서 공유한다.
- packed adjacency fallback도 source layout contract를 읽어야 한다. 단순히 `used_out_source` 또는
  `used_in_source`만 보면 unconstrained/global VLE와 property-constrained VLE까지 fallback setup을 막을 수
  있다. policy skip은 local edge-state, edge label, no-property constraint, 실제 directed source kind가
  모두 확인된 경우에만 적용해 fixed-source가 covered direction을 보장하는 lifecycle 최적화로 유지한다.
- adjacency/endpoint index OID 묶음은 `VLETraversalSourceIndexes` descriptor로 통합했다. setup path,
  cached index refresh, load policy, source layout builder가 같은 index handoff를 읽으므로, 이후 root/load
  policy descriptor가 source index ownership을 직접 가져갈 때 context의 raw OID 필드를 다시 노출할 필요가
  줄어든다.
- `VLETraversalSourcePolicy`는 source index handoff와 metadata load decision을 같은 boundary에 둔다. targeted
  local-index path가 가능한지는 source index availability를 보고 결정되므로, load policy가 setup의 raw index
  field를 다시 조합하지 않고 source policy 안의 descriptor를 읽는 구조가 맞다.
- `VLETraversalGraphLoad`는 graph name/oid, edge label oid, source policy를 묶어 global graph context load
  helper가 descriptor 하나를 소비하게 한다. 이 단계가 있어야 `VLETraversalSetup` 적용부를 별도 module boundary로
  낮출 때 graph/source/load field를 다시 풀어 넘기지 않는다.
- graph/label lookup cache, adjacency/endpoint index discovery, `VLETraversalSetup` 생성, global graph load
  helper는 `age_vle_setup.c`로 분리했다. 이는 `age_vle.c`의 역할을 traversal context mutation과 iterator
  orchestration으로 줄이는 방향이다. 남은 apply/root refresh는 `VLE_local_context` layout을 직접 쓰므로,
  다음 분리는 context field 나열을 먼저 root/apply handoff로 좁힌 뒤 진행해야 한다.
- `VLETraversalContextApply`는 새 context에 적용할 graph identity, cache flag, local edge-state policy,
  edge constraint, source index, range/prefetch budget을 하나의 handoff로 묶는다. root selection은 아직
  context base field가 적용된 뒤 fan-out을 계산해야 하므로 별도 단계로 남겼지만, apply helper가 setup raw
  field를 직접 나열하지 않게 되어 다음 module boundary 이동의 입력 표면이 줄었다.
- cached refresh도 `VLETraversalRefreshApply`로 root descriptor와 terminal direct-result scratch reset을
  같은 apply handoff에 담는다. 새 context setup과 cached refresh는 이미 같은 root apply helper를 쓰지만,
  refresh 쪽이 context raw field를 직접 갱신하던 부분을 descriptor화해 다음 root/apply module 이동의
  입력과 출력이 더 명확해졌다.
- source layout builder는 더 이상 `VLE_local_context` 전체를 받지 않고 `VLETraversalSourceLayoutInput`을
  받는다. source index bundle, local edge-state 여부, label constraint 여부, property constraint 여부가
  root/source layout 계산에 필요한 입력 전부라는 점을 드러내므로, root descriptor 생성부를 별도 모듈로
  옮길 때 context layout 노출을 줄일 수 있다.
- planner의 `AGE VLE Stream` edge-source descriptor와 runtime root layout이 endpoint-btree 선호 조건을
  따로 계산하면 `EXPLAIN`의 fixed-source와 실제 traversal source가 갈라질 수 있다. ORCA가 required
  property 결정을 operator 내부에 숨기지 않고 property object로 전달하듯, AGE도
  `select_vle_traversal_source_layout()`으로 candidate availability, upper bound, local edge-state,
  property constraint를 같은 selector에 넘겨 planner/runtime source decision을 공유한다.
- endpoint fanout estimate도 planner의 adjacency custom path costing과 runtime VLE edge-state capacity가
  따로 계산하면 같은 label/index evidence에서 서로 다른 cost 판단을 만들 수 있다. `age_vle_source_cost`는
  reltuples/statistics 기반 endpoint fanout을 공통 API로 제공하고, 다음 단계의 runtime source counter
  feedback도 같은 cost evidence boundary에 붙일 수 있게 한다.
- payload cache seed evidence는 `age_adjacency` source가 cacheable했다는 신호지만, replay run evidence는 같은
  source key가 실제 traversal 안에서 재사용됐다는 신호다. 따라서 dense-local/`age_adjacency` threshold 보정에서
  seed-only feedback보다 replay feedback을 더 강한 physical lifecycle 근거로 보는 것이 맞다. ORCA의
  `CReqdPropPlan`과 `CPhysical::CReqdColsRequest`가 required property를 child request로 캐시하듯, AGE VLE도
  runtime feedback을 단순 카운터가 아니라 scan/replay/seed ratio가 포함된 source lifecycle request로
  캐시해야 한다. 새 regression은 hidden assertion 대신 `EXPLAIN ANALYZE` runtime counter와 후속 `EXPLAIN`
  planner input을 그대로 출력해 이 contract를 검증한다.
- replay run ratio는 seed 여부보다 강한 threshold 입력이다. path-materialized/terminal-object consumer에서 replay
  비율이 25% 이상이면 같은 source key reuse가 우연한 seed가 아니라 반복 source lifecycle이라는 뜻이므로,
  endpoint-btree 유지 headroom을 0.25까지 낮춰 dense-local/`age_adjacency` 전환을 더 공격적으로 고른다.
  terminal-scalar consumer는 전체 path/object materialization을 피하고 scalar property만 읽으므로 같은 25%
  replay evidence를 strong ratio로 보지 않고 headroom 0.35를 유지한다. 이는 consumer required property가
  source lifecycle threshold를 바꾸는 ORCA식 request 해석에 맞춘 분리다.
- replay sweep 결과 path-materialized와 terminal-object는 25% replay에서 이미 materialization-heavy lifecycle로
  분리된다. path는 path container와 vertex prefetch/label-row fallback을 동반하고, terminal-object/properties는
  vertex object 또는 properties object materialization을 요구하므로 terminal-scalar와 같은 25% strong headroom에
  묶을 이유가 약하다. 따라서 source policy profile은 ORCA식 physical property처럼 materialization weight를
  typed descriptor로 노출한다. path는 weight 3으로 empty lifecycle batch와 replay headroom을 더 공격적으로
  보정하고, terminal-object/properties는 weight 2, terminal-scalar는 weight 1을 사용한다. 이에 따라 path
  strong replay headroom은 18%, terminal-object/properties는 20%, terminal-scalar는 40% threshold와 25%
  headroom을 유지한다.
- ORCA `CPhysicalScan`/`CPhysicalIndexScan`은 child가 없더라도 required column이 scan operator의 provided
  property로 만족되는지를 명시적으로 검사한다. AGE VLE도 runtime feedback class를 plan class 문자열과
  1:1로 맞추는 방식보다, planned source lifecycle이 요구한 property를 runtime source가 제공했는지로 봐야
  한다. `adjacency-replay`는 cache seed 가능한 `age_adjacency` empty lifecycle을 실제 source key reuse로
  강화한 provided property이므로, planned `adjacency-cache-seeded` lifecycle과 class mismatch가 아니라
  satisfied lifecycle로 정규화한다.
- 같은 weight vocabulary를 generic property projection에도 연결했다. `AGE Property Projection`은 lower slot에서
  properties object를 한 번 읽고 final output type에 따라 scalar Datum 또는 `agtype` wrapper를 만든다. 따라서
  cached property slot descriptor에 final materialization weight를 싣고, typed scalar output은 weight 1, final
  `agtype` output은 weight 2로 EXPLAIN에 드러낸다. 이 값은 다음 partial materialization/aggregate final
  descriptor가 VLE와 같은 vocabulary를 쓰기 위한 연결점이다.
- terminal-scalar도 replay fan-in이 40%까지 올라가면 scalar-only output이라는 차이보다 repeated source lifecycle
  비용이 더 커진다. `replay_branches=3, replay_leaves=16` profile은 25% profile보다 seed ratio가 낮고
  rows/elapsed가 50% strong profile에 가까워지므로, terminal-scalar strong replay threshold는 50%가 아니라
  40%가 맞다. 25% replay는 계속 `payload-replay-observed`로 남겨 seed-heavy 작은 profile이 과도하게
  `age_adjacency`를 고르지 않게 한다.
- replay feedback은 threshold 숫자만 바꾸는 입력이 아니라 physical lifecycle class도 바꾸는 입력이다. benchmark에서
  source는 `age_adjacency`로 맞아도 planner class가 `adjacency-cache-seeded`로 남으면 runtime
  `adjacency-replay` evidence와 mismatch가 난다. ORCA식 required property request 관점에서는 child/source
  request가 payload replay lifecycle을 요구했는지까지 class vocabulary에 반영해야 하므로, replay reason을
  소비한 planner policy는 `adjacency-replay`/`keep-age-adjacency`로 정규화한다.
- ORCA의 `CReqdPropPlan`/`CReqdPropRelational`은 child에게 요구한 physical property를 단순 cost 숫자로
  숨기지 않고 request/provided property vocabulary로 보존한다. Neo4j var-expand 쪽도 pruning/BFS/slotted
  expand shape가 runtime operator vocabulary로 드러난다. AGE VLE도 `root-empty-saturated`처럼 repeated empty
  completion을 관측한 feedback은 단순 headroom 보정이 아니라 `adjacency-empty-batch` class로 올려야 다음
  benchmark에서 source family 선택과 source lifecycle/batching request를 분리해 볼 수 있다.
- 큰 profile smoke(`label_fanout_edges=64`)에서는 payload replay ratio가 95%로 올라가 terminal-scalar도
  strong threshold를 넘었다. 이 결과만으로 terminal-scalar threshold를 낮추면 낮은 replay ratio profile을
  놓칠 수 있으므로, replay fan-in/leaf fan-out을 benchmark 변수로 분리해 ORCA식 request 선택을 workload
  dimension별로 비교하는 쪽이 먼저다.
- `VLE Source Runtime`의 raw counter만 executor에서 포맷하면 cost evidence boundary와 runtime evidence가
  다시 갈라진다. `format_vle_source_runtime_evidence()`는 dominant source, dominant yield, payload replay,
  push/yield ratio를 `age_vle_source_cost`에서 만든다. 이 출력은 아직 planner feedback cache는 아니지만,
  source cost module이 runtime counter를 해석하는 첫 contract다.
- start/end root swap을 위한 fan-out 비교도 `VLETraversalRootSelectionInput`을 통해 graph context, edge label,
  empty range, zero-only range만 읽는다. `get_initial_edge_count()`가 `VLE_local_context` 전체를 보지 않게
  되어 root descriptor 생성부의 남은 context 의존이 source layout input과 root selection input으로
  분리됐다.
- `age_vle_root.c`는 root descriptor 생성, cached refresh root 갱신, source layout builder, initial fan-out
  count를 맡는다. 이 분리는 root/source selection을 `VLE_local_context` field mutation과 분리하고, 본체는
  context에서 descriptor를 만들고 결과 descriptor를 apply하는 orchestration만 담당하게 만드는 단계다.
- `VLETraversalRootApplyInput`은 root selection input, source layout input, current root descriptor를 함께
  묶는다. 새 context setup과 cached refresh가 같은 handoff를 root module에 넘기므로, root apply 단계에서
  필요한 context-derived 입력이 하나의 surface로 정리됐다.
- `VLETraversalSetupApply`는 새 context의 base field apply, root descriptor, terminal output policy,
  edge-state init 결정을 함께 보관한다. root descriptor와 edge-state capacity는 `VLE_local_context`에 base
  field를 먼저 적용한 뒤 다시 읽는 대신 `VLETraversalContextApply`와 `VLETraversalRootDescriptor`에서
  계산한다. 따라서 setup apply sequence의 hidden context round-trip이 줄고, initial context 구성 계약이
  context mutation이 아니라 descriptor handoff 중심으로 정리됐다.
- `VLETraversalActivationApply`와 `VLETraversalCachedReuseApply`는 initial stack load, cached context refresh
  후 source index refresh, SRF memory context 전환, next start-vertex reload를 같은 activation vocabulary로
  묶는다. `load_initial_dfs_stacks()` 호출자는 activation helper 내부로 모였고, `build_local_vle_context()`는
  cached/new context 선택과 graph load orchestration만 담당한다. 이는 traversal 재적재 정책을
  `VLE_local_context` field mutation이 아니라 reload descriptor로 옮기는 단계다.
- `age_vle_context.h`는 `VLE_local_context` layout과 setup/apply/refresh/activation descriptor type을 갖는
  internal context surface다. 이 이동은 context layout을 모든 모듈에 무작정 공개하려는 목적이 아니라,
  setup/apply 구현을 별도 module로 내릴 때 필요한 mutation contract를 한 header에 모으고 `age_vle.c`의
  로컬 type 소유를 해체하는 단계다.
- `age_vle_apply.c`는 context apply, setup root/output/edge-state apply, cached refresh apply, activation
  reload, source layout refresh 구현을 맡는다. `VLETraversalApplyOps`는 source index refresh와 initial stack
  load처럼 traversal graph operation callback만 남기고, edge property constraint cache는 context base apply
  내부 lifecycle로 이동했다. 이 구조는 apply policy가 graph-runtime callback을 덜 요구하게 만들고, constraint
  cache ownership을 context setter module로 낮춘다.
- `age_vle_context.c`는 base/root/output/edge-state setter, terminal-property direct-result reset, source/root
  input builder, source layout refresh를 맡는다. `age_vle_apply.c`의 `VLE_local_context` raw field 접근을 제거해
  apply module은 descriptor orchestration과 callback invocation만 남겼다. 다음 구조 병목은 context 전체
  layout이 여전히 graph identity, traversal bounds/root, output policy, terminal scratch를 한 struct에 담는
  폭 자체다.
- `VLEContextOutputState`는 output requirement, terminal-only/property emission flag, frame vertex carry flag,
  terminal property key metadata, direct terminal-property result scratch, block prefetch state,
  terminal-property batch state를 하나로 묶는다. output policy와 terminal scratch를 traversal/root/cache identity
  field에서 분리해 context layout의 첫 substate boundary를 만들었다.
- ORCA의 physical property가 plan node 경계에서만 보이고 executor local state에서 사라지면 실제 enforcement
  여부를 판단할 수 없듯, VLE empty lifecycle도 planner descriptor와 EXPLAIN text만으로는 cached refresh가 같은
  contract를 유지했는지 확인하기 어렵다. `AgeVLEInput -> VLETraversalContextApply -> VLEContextRefreshInput ->
  VLE_local_context -> AgeVLESourceStats`로 typed field를 관통시키면 runtime context 자체가 planned empty
  lifecycle을 소비했는지 `empty-context`로 확인할 수 있다. 이는 source enum rollback보다 root/source lifecycle
  batching threshold를 조정하기 위한 더 강한 근거다.
- ORCA의 `CReqdPropPlan`/`CReqdProp` 구조는 required property를 top-down request로 전달하고 operator가 이를
  child request와 enforcement 판단에 사용한다. VLE empty completion도 단순 runtime counter가 아니라 plan에서
  요구한 lifecycle property로 봐야 한다. 그래서 planner가 `empty-batch` capacity를 계산하고 executor context가
  이를 frontier empty queue allocation에 소비한 뒤 runtime stats로 match 여부를 돌려준다. 이 흐름은
  `source_match`처럼 결과를 사후 비교하는 데서 멈추지 않고, 다음 threshold/batching policy의 입력으로 쓸 수 있는
  property handoff다.
- `empty-batch`가 큰 경우는 작은 regression 결과가 같아도 큰 fan-out에서 endpoint-btree를 유지할 위험이 더 크다.
  따라서 batch size 자체를 physical property strength로 보고, strong batch 후보에서는 endpoint headroom을 낮춰
  planner가 `age_adjacency` lifecycle을 더 빨리 선택하게 한다. runtime의
  `empty-summary=completion:N/batch:N/saturated:true|false`는 이 threshold가 실제 반복 completion과 맞는지 확인하는
  다음 feedback surface다.
- ORCA는 `CReqdPropPlan`, `CPropSpec`, `CEnfdProp` 계열에서 physical property를 operator-local 문자열이 아니라
  required/enforced property object로 전달한다. AGE VLE도 `empty-summary` 문자열만 늘리면 planner feedback으로
  소비하기 어렵다. 따라서 repeated empty source completion을 `VLETraversalEmptyCompletionSummary`로 root/source
  descriptor에 올리고, stream accumulator와 benchmark join이 같은 typed summary를 읽게 했다. 다음 threshold 보정은
  이 summary의 `completion/out/in/batch/saturated-roots`를 source policy input으로 넘기는 방향이 맞다.
- `VLESourceRuntimeThresholdFeedback`은 이 handoff의 첫 소비자다. planner가 아직 persistent runtime cache를 읽지는
  않지만, runtime summary를 `eligible/headroom/source/reason` descriptor로 정규화하므로 다음 단계에서 graph/label,
  direction, consumer class별 threshold cache를 붙일 수 있다. saturated root-empty feedback은 planned batch size가
  작아도 큰 workload에서 반복 empty completion이 확인된 physical property로 보고 0.35 headroom 후보를 낸다.
- `VLETerminalOutputPolicy`, `VLETerminalPropertyLookup`, `VLEPropertyKeyDescriptor`도 context vocabulary로
  올렸다. context module이 key descriptor, char-fast flag, relation cache pointer, prefetched block hash,
  prefetch budget을 조립하고 direct result scratch를 get/set/clear API로 관리하므로, DFS/output hot path는
  `VLEContextOutputState` layout 대신 terminal output descriptor만 소비한다.
- terminal-property batch lifecycle도 context API로 낮췄다. batch id append, result/null allocation, fetch,
  materialized flag, emit cursor를 caller가 직접 만지면 output substate 분리가 이름 변경에 머문다. 이제
  batch materialization caller는 current terminal vertex id와 next batch result를 context API로 주고받아
  traversal path stack과 batch array layout을 동시에 숨긴다.
- output materialization input도 같은 방식으로 좁혔다. batch fetch descriptor와 `VLEContainerBuildInput`은
  context API가 조립하므로 caller가 graph oid, path stack, path vertex stack, reverse-output flag, terminal
  property key handoff를 직접 읽지 않는다. 이는 container builder를 별도 output/materializer boundary로
  더 옮길 때 필요한 descriptor surface를 context vocabulary에 고정하는 단계다.
- `VLETraversalApplyInput`은 setup descriptor, loaded graph context, cache flag, grammar node id를 묶어
  local context apply helper가 runtime apply contract 하나만 받게 한다. 이 handoff가 있어야 setup apply
  module을 분리할 때 graph load 결과와 cache identity를 별도 인자 목록으로 다시 노출하지 않는다.
- `AGE VLE Stream` explain의 edge-source descriptor도 같은 vocabulary로 확장했다. planner 단계에서는
  catalog/index 존재 여부와 local edge-state 후보를 보고 outgoing/incoming fixed source kind를 계산하고,
  candidate index 존재 여부와 분리해 출력한다. endpoint btree index가 존재해도 global metadata traversal이면
  fixed source는 `none`으로 보이므로, regression expected가 실제 runtime dispatch contract를 더 직접
  검증한다.
- typed endpoint handoff는 load decision에도 써야 의미가 있다. targeted edge-label VLE에서 bound end만
  있는 paths-to shape는 나중에 reverse traversal root가 end로 바뀌므로, vertex metadata list에서 임시
  start를 가져올 필요가 없다. bound start 또는 bound end가 있으면 targeted adjacency/lazy hydrate 경로가
  전체 vertex metadata scan을 피할 수 있다.
- `age_vle.c`에 input slot parsing, traversal state, cache, materializer가 함께 있으면 다음 구조 변경이
  다시 helper 추가로 흐르기 쉽다. `age_vle_input.c`를 분리해 graph/endpoint/range/direction/output key
  accessor와 dynamic fallback을 맡기면, traversal state 쪽 변경은 `VLE_local_context`와 cache/layout
  contract에 집중할 수 있다.
- `age_vle_iterator_next()`가 traversal search와 output emission을 동시에 결정하면 output requirement가
  늘 때마다 iterator loop가 다시 커진다. 먼저 `VLEIteratorOutputState`로 terminal scalar batch,
  terminal full-properties, terminal vertex/path container, cleanup contract를 한 경계에 묶으면
  traversal loop는 path discovery/advance만 맡고, 다음 단계에서 output/materializer helper를 별도
  모듈이나 final projection descriptor와 연결할 수 있다.
- 같은 이유로 DFS path-function dispatch와 next-start advance도 `VLEIteratorSearchState`에 묶는다.
  iterator public API는 SRF row 생산 순서만 조율하고, search helper는 path discovery와 zero-bound
  handoff만 책임진다. 이 경계가 있어야 terminal scalar/properties requirement가 늘어도 traversal policy와
  materializer output 선택이 같은 함수에서 다시 결합되지 않는다.
- materializer 선택도 `emit_terminal_only`/`output_requirement` field를 emission 함수 안에서 직접
  반복해 읽는 대신 `VLEIteratorMaterialization` descriptor로 낮춘다. 이 descriptor는 path, terminal
  vertex, terminal scalar property, terminal full properties, zero-bound 여부를 하나의 handoff로 묶어
  다음 파일 분리 또는 final projection descriptor 연결 때 넘길 표면을 줄인다.
- `VLEIteratorMaterialization` 선택을 별도 모듈로 빼면 `age_vle.c`가 output requirement enum의 해석을
  계속 반복하지 않는다. 이는 단순 helper가 아니라 final projection/materializer descriptor를 붙일
  내부 API 표면이다. terminal-only 판정도 같은 모듈에 두어 frame payload carry, iterator output,
  planner-derived output requirement가 같은 semantic boundary를 보게 한다.
- path/container builder 자체는 아직 `VLE_local_context`와 `VLE_path_container` 내부 layout에 강하게
  묶여 있으므로 바로 새 파일로 옮기면 private state를 과도하게 공개하게 된다. 먼저
  `VLEIteratorContainerKind`를 descriptor에 추가해 zero-bound, reverse-output, terminal-vertex 조합
  선택을 output descriptor 모듈이 맡게 했다. 다음 이동은 builder 실행에 필요한 최소 context/handoff를
  좁힌 뒤 진행하는 것이 맞다.
- 그 다음 단계로 `VLEContainerBuildInput`을 도입해 builder 실행 입력을 graph oid, start id,
  reverse flag, path stack, path vertex stack으로 좁혔다. `VLE_local_context` 전체를 넘기는 방식은
  traversal/cache/output state가 builder에 암묵적으로 열려 있어 파일 분리를 방해한다. 이 handoff는
  builder를 별도 output/materializer 모듈로 옮길 때 공개해야 할 context 표면을 미리 제한한다.
- container builder 실행부는 `age_vle_container.c`로 분리했다. `age_vle_container.h`는
  `VLE_path_container` layout과 `VLEContainerBuildInput`만 노출하고, path/reversed/zero/terminal container
  조립과 container-kind dispatch는 새 모듈이 맡는다. `age_vle.c`는 traversal state에서 build input을 만들고
  iterator emission handoff를 호출하는 역할만 남는다.
- plan evidence도 같은 descriptor 관점으로 맞춘다. `EXPLAIN (VERBOSE)`의 `VLE Materialization`은
  path container, terminal vertex container, terminal scalar direct, terminal full-properties direct를
  구분해, output requirement가 실제 materializer/output handoff와 어떻게 연결되는지 regression에
  드러낸다.
- edge prototype dynamic fallback도 `age_vle_input.c`로 옮기면 traversal setup은 edge label OID와
  property constraint agtype을 만드는 입력 contract만 소비한다. 이 경계가 있어야 다음 단계에서 edge
  descriptor를 targeted load, adjacency payload, cached-property handoff로 직접 넘길 수 있다.
- `build_local_vle_context()`의 graph lookup, edge prototype, adjacency index lookup, load policy,
  initial endpoint, range/direction 계산을 한 함수 안의 지역 변수로 흩어두면 이후 typed traversal
  descriptor로 빼기 어렵다. `VLETraversalSetup`을 두면 `AgeVLEInput`에서 한 번 만든 planner/executor
  handoff를 context builder가 적용하는 구조가 되고, 새 context에서는 setup이 이미 찾은 adjacency index
  OID를 재사용해 같은 label index list scan을 반복하지 않는다.
- `VLETraversalSetup`의 load decision도 별도 `VLETraversalLoadPolicy`로 분리했다. policy는 graph/edge
  descriptor와 endpoint/index/range/direction descriptor가 모두 준비된 뒤 계산되어야 한다. 이렇게
  해야 context builder가 metadata load boolean을 다시 조합하지 않고, 이후 endpoint source kind나
  adjacency/index source layout을 policy 입력으로 확장할 수 있다.
- endpoint validity, initial root graphid, lower/upper range, direction은 함께 traversal semantic을 이룬다.
  이를 `VLETraversalShape`로 묶어 load policy, root selection, context apply가 같은 descriptor를 읽게
  하면, planner-derived typed shape를 직접 넘기는 다음 단계에서 endpoint/range/direction을 다시
  `AgeVLEInput` slot parsing으로 풀지 않아도 된다.
- `apply_vle_traversal_setup()`은 setup descriptor를 local context에 적용하는 단일 경계다. endpoint
  path-function 결정, edge constraint cache seeding, index OID handoff, range/direction, reverse-root
  선택을 helper 안으로 모아 context builder가 setup field를 직접 조합하지 않게 했다. terminal output
  descriptor는 setup 적용 뒤 초기화해 traversal/load contract와 output materialization contract가
  섞이지 않게 둔다.
- DFS candidate frame은 traversal root와 terminal output handoff를 구분해야 한다. `source_vertex_id`는
  frame마다 반복 저장할 필요가 없고 consume/root descriptor에서 복원되는 metadata다. 반대로
  `vertex_entry`는 단순 cache pointer가 아니라 terminal vertex/property output과 indexed property
  helper가 scalar/property cache를 공유하는 handoff이므로, terminal-only output에서는 보존해야 한다.
  assertion 실패를 guard로 막는 대신 output requirement 정책으로 frame payload contract를 정리했다.
- 조기 종료되는 subplan(`EXISTS`, semi-join, LIMIT류 consumer)은 VLE stream을 끝까지 읽지 않을 수
  있다. 이 경우 cached `VLE_local_context`를 clean으로 표시하면 DFS stack/frame state가 완전한 종료
  상태라는 잘못된 contract가 된다. SRF shutdown callback과 같은 의미로 resource cleanup만 하고
  dirty cache는 다음 재사용 시 폐기해야 한다.
- 다음 CustomScan 단계는 argument expression list도 start/end vertex, edge prototype, range,
  direction, terminal-only/property output 요구를 가진 정규화 descriptor로 낮추는 것이다. 이 구조가
  잡혀야 planner/executor boundary에서 path materialization, terminal property fetch, adjacency
  payload scan을 더 공격적으로 재배치할 수 있다.

## ORCA required/computed column 구조

조사 대상:

- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca/libgpopt/src/operators/CPhysicalComputeScalar.cpp`
- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca/libgpopt/src/operators/CExpressionPreprocessor.cpp`
- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca/libgpopt/src/operators/CLogicalProject.cpp`
- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca/libgpopt/src/operators/CLogicalGbAgg.cpp`
- `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca/libgpopt/src/operators/CPhysicalStreamAgg.cpp`

관찰:

- `CPhysicalComputeScalar::PcrsRequired`는 parent required column set을 복사한 뒤
  scalar child에서 실제 계산에 필요한 child column만 relational child로 내려보낸다.
  computed output 전체를 child target에 싣는 방식이 아니다.
- `CPhysicalComputeScalar::PosRequired`는 required order가 ComputeScalar가 정의한
  column을 쓰면 child에 sort order를 요구하지 않고, ComputeScalar 위에서 order
  enforcer가 처리하게 둔다. computed column과 child physical property를 분리한다.
- `CExpressionPreprocessor::PexprPruneUnusedComputedCols`는 query output/order에서
  필요한 `CColRefSet`을 기준으로 `CLogicalProject`와 `CLogicalGbAgg`의
  `CScalarProjectElement`를 제거한다. 남은 project element의 used column만 child
  required set에 추가한다.
- `CLogicalGbAgg`는 grouping colref를 local used column에 포함하고, project list가 모두
  prune되어도 grouping column이 있으면 empty project list를 가진 aggregate를 유지한다.
- `CScalarProjectElement`는 expression 자체보다 output `CColRef` identity가 핵심이다.
  상위 operator는 expression tree 반복이 아니라 colref descriptor로 required/output을
  표현한다.

AGE에 대한 결론:

- collect/property aggregate rewrite처럼 함수별 aggregate를 계속 늘리는 방향은 ORCA의
  핵심과 맞지 않는다. 먼저 PathTarget에서 final output descriptor와 lower required
  expression을 분리해야 한다.
- `agtype_access_operator(properties, key)` 전체를 aggregate/sort input target에 계속
  싣지 말고, typed/scalar key 또는 ctid+key descriptor처럼 lower path가 실제로 필요한
  값만 보존하는 구조를 우선한다.
- PostgreSQL에서는 ORCA의 `CColRefSet`에 해당하는 표준 descriptor가 명시적으로 없으므로
  `PathTarget`, `TargetEntry`, `sortgroupref`, final `ProjectionPath`를 조합해 비슷한
  contract를 만들어야 한다.
- PostgreSQL final DISTINCT의 `UpperUniquePath`는 projection-capable path가 아니지만
  `subpath->pathtarget`을 그대로 사용한다. 따라서 lower target을 바꾸려면 `Unique` 위에
  projection을 얹는 것이 아니라 `UpperUniquePath`의 subpath까지 target 교체가 통과해야
  한다.
- `DISTINCT` output property를 ctid refetch로 LIMIT 위에 미루면 `Unique`가 비교해야 하는
  logical output expression과 lower descriptor가 갈릴 수 있다. 현재는
  `distinctClause != NIL` shape에서 ordered property deferred projection을 적용하지
  않는다. `DISTINCT`와 typed order를 함께 최적화하려면 output descriptor와 distinct key
  descriptor를 별도로 설계해야 한다.
- typed `collect(DISTINCT n.i::pg_bigint|pg_text)`는 generic `age_collect`로 남으면
  DISTINCT sort/input target에서 typed scalar descriptor 이점을 충분히 쓰지 못한다.
  새 aggregate 함수를 추가하지 않고 기존 `age_collect_int8/float8/text` OID로 rewrite하면
  typed aggregate와 narrow lower target rewrite를 함께 적용할 수 있다.
- 작은 regression fixture에서는 효과가 작아도, 넓은 `properties`, 높은 cardinality,
  `Sort`/`HashAggregate`/`Unique` 입력 row width가 큰 workload에서는 lower target
  축소가 더 큰 효과를 낼 수 있다.
- PostgreSQL `IndexOptInfo`는 expression index key를 `indexprs`로 보관하고, partial
  predicate는 `check_index_predicates()`가 `predOK`/`indrestrictinfo`를 다시 계산한 뒤
  `create_index_paths()`에서 path 후보로 이어진다. AGE property rewrite가 index surface를
  바꾸면 query expression, generated index expression, predicate expression이 같은
  canonical helper를 써야 한다.
- `pg_numeric` property typecast는 기존 Cypher `numeric` semantic과 분리해야 한다.
  `numeric`은 agtype numeric을 반환하는 기존 surface로 유지하고, `pg_numeric`은
  PostgreSQL `numeric` Datum을 반환하는 `agtype_object_field_numeric()`으로 낮춰
  btree expression index와 `HashAggregate`/`Sort` key가 varlena `agtype` container 대신
  numeric Datum을 다루게 한다.
- `pg_numeric`은 `ORDER BY ... LIMIT`의 deferred projection에서도 기존 ctid/id refetch
  descriptor를 그대로 쓸 수 있다. lower path는 lookup key와 scalar numeric sort key만
  들고, final output은 refetch helper로 agtype property를 만든다.
- `collect(DISTINCT ...::pg_numeric)`은 `age_collect_numeric(numeric)`으로 낮춰 DISTINCT
  sort/input target이 agtype numeric container가 아니라 PostgreSQL numeric Datum을 다루게
  한다. 이 확장은 typed collect family의 scalar type coverage를 완성하는 쪽이고, 앞으로는
  aggregate별 helper를 더 늘리기보다 descriptor 공통화를 우선한다.
- AGE 쪽 구현도 ORCA의 required/computed column 분리와 Neo4j cached-property slot key
  구조에 맞춰, scalar type마다 별도 함수 분기를 늘리는 대신 `(value_type, physical helper,
  aggregate/final materializer)` descriptor를 공유해야 한다. 이번 typed collect aggregate
  선택은 먼저 `(value_type, aggregate name, oid cache)` table로 묶었다.
- scalar property helper와 final `agtype` materializer도 같은 방향으로 묶는다. field
  helper result type과 semantic scalar type을 함께 보관하면 `numeric` agtype helper처럼
  field result가 `AGTYPEOID`이고 semantic value가 `NUMERICOID`인 shape도 함수별 branch 없이
  다룰 수 있다. 이 descriptor는 다음 cached-property slot/index handoff에서 property key,
  physical scalar type, final materialization 여부를 하나의 metadata로 넘기는 기반이다.
- expression index matching도 semantic value type만으로 충분하지 않다. `numeric` cast와
  `pg_numeric` cast는 둘 다 semantic `NUMERICOID` property path지만, 전자는 agtype numeric
  container를 반환하고 후자는 PostgreSQL `numeric` Datum을 반환한다. 따라서
  `CypherPropertyAccessSignature`는 `(container, key path, semantic value type, physical field
  result type)`을 함께 비교해야 하며, cached-property handoff도 이 physical result type을
  slot/index metadata로 넘겨야 한다.
- optimizer가 별도 field helper OID cache를 들고 있으면 parser rewrite, expression index
  matching, lower/final projection이 같은 physical descriptor를 쓴다는 보장이 약해진다.
  property field helper lookup은 `cypher_property_signature`의 shared descriptor API로 모으고,
  optimizer-local scalar descriptor는 final materializer와 aggregate/final handoff처럼
  planner-only metadata만 보관하는 쪽이 맞다.
- final projection rewrite가 scalar expression `Node`만 넘기면 이후 cached-property slot이나
  index handoff가 어떤 semantic/physical type의 값인지 다시 expression tree에서 추출해야
  한다. `CypherScalarFinalHandoff`처럼 scalar expr, semantic value type, physical result type,
  final materializer OID, property signature를 한 번에 넘기는 구조가 ORCA의 computed column
  descriptor와 Neo4j cached property slot key에 더 가깝다.
- scalar final lower/final target builder를 property module로 이동하면 count/agtype deferred projection도
  같은 descriptor 경계를 쓴다. path hook은 projection path를 붙일 위치만 결정하고, cached-property slot
  expression 선택, lower computed expression reuse, final materializer rewrite는
  `CypherScalarFinalHandoff`가 있는 property descriptor module에서 처리한다.
- typed collect도 aggregate OID와 argument expression만 따로 다루면 DISTINCT lower target 축소와
  future cached-property slot handoff가 다시 aggregate tree local rule에 묶인다.
  `CypherTypedCollectHandoff`는 aggregate input expr, scalar value type, typed aggregate OID,
  property signature를 한 번에 넘겨 final/scalar handoff와 같은 descriptor 계열로 맞춘다.
- VLE `age_adjacency` index metadata도 방향별 helper 호출보다 label generation으로 invalidation되는
  pair descriptor가 맞다. PostgreSQL relcache index list scan은 같은 edge label에서 outgoing과
  incoming을 동시에 판정할 수 있으므로, load decision과 local context refresh가 같은 pair
  metadata를 공유하게 해야 반복 catalog/index-list probe를 줄일 수 있다.
- Neo4j의 index DDL은 helper 함수를 직접 호출하게 하지 않고 Cypher parser surface에서 index name,
  target entity, property를 선언하게 한다. AGE도 source index helper를 유지하되 사용자-facing surface는
  `CREATE INDEX ... FOR (n:Label) ON (n.prop)`, relationship property index, 방향별
  `CREATE INDEX ... FOR ()-[r:TYPE]->() ON (ADJACENCY)`, `DROP INDEX`, `SHOW INDEXES`로 맞춘다.
- `ag_catalog.ag_graph_index`는 planner가 catalog/index-list scan을 매번 재구성하지 않도록 graph, label,
  entity kind, source kind, direction, property vector, provider, index oid를 보관하는 metadata boundary다.
  지금은 helper/parser가 만든 index만 기록하지만, 다음 단계에서는 이 metadata를 `AGE Adjacency Match`와
  VLE source descriptor가 직접 소비해 property source와 adjacency source를 하나의 graph index request로
  결합해야 한다.
- VLE edge property metadata는 edge property constraint fast reject의 count/size/hash와 edge
  object materialization의 empty-object shortcut에 쓰인다. constraint가 없는 cached VLE query는
  edge properties를 cold-load에서 전부 읽을 필요가 없고, output materialization이 필요한 edge만
  TID 기반 lazy fetch로 properties를 읽는 contract가 더 큰 데이터셋에 맞다.
- fallback VLE의 vertex metadata도 traversal adjacency와 vertex materialization 요구를 분리할 수
  있다. targeted edge label이면 edge scan이 endpoint graphid를 이미 수집하므로, bound-start
  cached path shape에서는 endpoint id만으로 skeletal vertex entry를 만들고 packed adjacency를
  link할 수 있다. vertex label relation/name은 graphid label id에서 채우고, TID/properties는
  실제 path/vertex/property materialization 때 vertex id btree index로 lazy hydrate한다. lazy
  hydrate 결과는 graph-context `cached_properties`로 남겨 같은 materialization 안의 반복 vertex
  property fetch를 index probe 반복으로 만들지 않는다. 전체 edge-label 없는 VLE는 endpoint label
  surface가 없으므로 이 contract에 넣지 않는다.
- expression index matching도 matched expression만 반환하면 caller가 어떤 property descriptor를
  기준으로 index surface를 선택했는지 잃는다. `CypherPropertyIndexHandoff`는 query expr,
  matched index expr, property signature를 함께 넘겨 다음 cached-property slot 생성 지점이
  같은 `(container, key path, semantic/physical type)` descriptor를 재사용할 수 있게 한다.
- scalar final, typed collect, property index handoff가 각각 다른 optional signature field를
  들면 cached-property slot candidate를 만들 때 다시 case별 변환이 필요하다.
- cached-property slot metadata는 scalar final, typed collect, expression index handoff 생성 시점에
  함께 채워야 한다. caller가 매번 expression tree에서 `CypherCachedPropertySlotDescriptor`를 다시 만들면
  ORCA식 computed-column descriptor handoff가 아니라 local rewrite rule 나열로 되돌아간다.
  `CypherScalarFinalHandoff`, `CypherTypedCollectHandoff`, `CypherPropertyIndexHandoff`가 같은
  `(container, key path, semantic value type, physical result type, final/aggregate/index OID)` slot metadata를
  보관하면 lower target, aggregate input, index restriction rewrite가 같은 cached-property contract를
  공유한다. descriptor 필드 검사가 필요한 simple projection/canonicalization만 expression에서 slot descriptor를
  추출하는 좁은 API를 사용한다.
- simple property projection CustomScan이 key만 받으면 executor boundary가 cached-property
  slot 후보의 semantic/physical type을 모른다. `CypherCachedPropertySlotDescriptor`를 만들고
  key, semantic value type, physical field result type을 CustomScan private data로 넘기면
  이후 typed/scalar cached slot으로 확장할 수 있는 실제 planner/executor contract가 생긴다.
- slot descriptor가 key path를 들고 있는데 executor boundary가 단일 key만 받으면 nested
  property projection은 다시 helper chain materialization으로 돌아간다. CustomScan private
  data를 key path list로 유지하고 executor가 object path를 직접 따라가면 nested terminal
  scalar도 같은 cached-property slot contract 안에서 출력할 수 있다. 이 경로는 path prefix
  object를 output으로 만들지 않고 terminal value만 Datum으로 변환한다.
- executor가 physical field result type을 사용해 scalar Datum을 직접 채우면 property lookup 뒤
  `agtype` wrapper를 만들고 다시 PostgreSQL scalar로 변환하는 경로를 피할 수 있다. simple
  property projection CustomScan은 이 contract의 첫 적용 지점이며, 이후 ordered/refetch,
  aggregate lower target도 같은 slot descriptor를 재사용해야 한다.
- cached-property CustomScan도 verbose `EXPLAIN`에서 key path, semantic value type, physical
  field result type을 직접 보여줘야 한다. plan에 `Custom Scan (AGE Property Projection)` 이름만
  있으면 scalar Datum handoff를 탔는지 generic `agtype` materialization을 탔는지 구분할 수 없다.
  `Cached Property Slot` 출력은 regression을 hidden assertion 대신 planner-visible contract로 고정한다.
- multi-slot property projection은 typed/scalar field result에 우선 적용한다. untyped `agtype`
  여러 컬럼은 PostgreSQL projection layer가 CustomScan scan tuple과 targetlist를 다시 맞추는 과정에서
  semantic output을 잃기 쉬우므로, multi-slot 전환의 첫 gate는 `pg_bigint`/`pg_float8`/`pg_numeric`/
  `pg_text`처럼 physical Datum type이 명확한 descriptor로 둔다. generic `agtype` multi-output은
  별도 final materialization descriptor가 준비된 뒤 넓힌다.
- ordered property projection delay도 raw `(properties, keys)` tuple이 아니라 output/sort
  `CypherCachedPropertySlotDescriptor`를 비교해야 한다. 이렇게 해야 typed sort key reuse,
  ctid/id refetch final materialization, cached-property CustomScan이 같은 key path와 physical result
  type contract를 보며, 이후 aggregate lower target으로 descriptor를 넘길 때 case별 재해석이 줄어든다.
- simple property projection detection과 ordered property projection lower/final target builder는
  `cypher_property_paths.c`로 이동했다. hook은 path insertion과 cost adjustment만 남기고,
  property source/key/type 판정, sort output 재사용 여부, ctid/id refetch final expression 생성은
  cached-property descriptor boundary에서 결정한다.
- typed collect lower target은 handoff list와 arg expression list를 따로 들고 다니지 말고,
  `CypherTypedCollectArgPlan`처럼 handoff, computed arg, sortgroupref를 한 descriptor로 묶어야 한다.
  lower target insertion과 aggregate target rewrite가 같은 planned arg를 보므로 cached-property slot
  expression을 다시 만들거나 expression tree에서 property metadata를 재해석할 필요가 줄어든다.
- 이 descriptor를 `cypher_property_paths.c`로 이동하면 ORCA의 computed column descriptor처럼
  arg expression, sortgroupref, final aggregate rewrite가 같은 property handoff vocabulary에 묶인다.
  upper path hook은 group column과 path node 조립만 맡고, cached-property slot 선택은 scalar final,
  typed collect, expression index handoff가 모인 property descriptor module에서 결정한다.
- ORCA의 required/computed column 분리와 Neo4j의 cached property projection 전파를 AGE에
  적용하면, `ORDER BY`를 위해 이미 계산한 typed property key를 final output이 같은
  semantic/physical descriptor일 때 재사용할 수 있다. 이 경우 ctid/id refetch helper를 새로
  늘리는 것보다 lower computed column을 보존하는 편이 row width와 extra heap lookup을 동시에
  줄인다. 원래 `agtype` 값을 보존해야 하는 output과 typed sort key가 다른 descriptor인 경우는
  기존 refetch materialization 경로를 유지한다.
- Neo4j의 `orderedDistinct(... cacheN[n.firstName] ...)`와 `distinct("cacheN[n.lastName] ...")`
  plan은 DISTINCT/aggregation boundary에서도 cached property를 일반 expression이 아니라 slot
  metadata로 전파한다. AGE의 `CypherCachedPropertySlotDescriptor`도 terminal field helper를
  재구성할 수 있어야 scalar final, typed collect, index handoff가 같은 computed property column을
  공유할 수 있다. 따라서 lower target을 만들 때 원본 expression을 그대로 복사하는 대신 slot
  descriptor에서 canonical field expression을 만들도록 옮기는 것이 다음 index handoff 연결의
  선행 조건이다.
- non-DISTINCT typed collect는 `age_collect_numeric(agtype_object_field_numeric(...))`까지는
  내려가지만, plain Agg plan creation이 aggregate argument의 base Var를 보고 child scan target을
  다시 `n.id, n.properties`로 넓힌다. 단순히 AggPath subpath target을 바꾸거나 ProjectionPath를
  끼우는 방식은 existing DISTINCT collect에서 setrefs 오류를 만들 수 있다. 다음 돌파구는
  Aggref argument와 child projection target을 별도 lower aggregate descriptor로 함께 바꾸는
  구조다.
- AggPath target 안의 copied Aggref argument를 slot expression으로 바꾸면 DISTINCT aggregate의
  `aggdistinct` ressortgroupref metadata를 보존해야 한다. TargetEntry를 새로 만들면
  "ORDER/GROUP BY expression not found in targetlist" 오류가 나므로 기존 TargetEntry를 복사하고
  expr만 바꾼다. plain aggregate는 이 boundary를 통과해도 base scan이 physical tlist를 표시할 수
  있어, 다음 단계는 aggregate input을 executor-visible cached slot으로 만드는 것이다.
- PostgreSQL expression index matching은 planner가 보관한 index expression과 query restriction의
  structural equality에 민감하다. AGE가 property signature로 semantic match를 인정한 뒤에도
  caller에 raw index expression만 넘기면 cached-property slot metadata와 restriction rewrite가
  다시 갈라진다. matched index expression을 설정할 때 slot descriptor에서 canonical field
  expression을 먼저 재구성하면 index, scalar final, typed collect가 같은 property column
  contract를 공유한다.
- index handoff의 restriction rewrite도 같은 이유로 raw `index_expr`를 직접 소비하지 않는다.
  `CypherPropertyIndexHandoff`에서 `CypherCachedPropertySlotDescriptor`를 만든 뒤 slot expression
  builder를 통과시키면, matched expression index surface와 scalar/aggregate lower target이 같은
  descriptor boundary를 공유한다. slot descriptor에 `index_expr`가 있으면 catalog index surface를
  보존하고, 없을 때만 signature 기반 canonical helper chain을 재구성한다.
- 이 restriction rewrite와 partial predicate/`indrestrictinfo` canonicalization은
  `cypher_paths.c` hook-local helper가 아니라 `cypher_property_paths.c`의 property descriptor API가
  맡는다. hook은 rewrite 성공 여부에 따라 `check_index_predicates()`와 `create_index_paths()`를 다시
  호출하는 orchestration만 남기는 편이 ORCA식 required/computed column boundary에 가깝다.
- 단, `IndexOptInfo`의 index key expression surface를 실제 catalog index expression과 다르게
  바꾸면 `create_index_paths()`/plan creation에서 "index key does not match expected index
  column" 오류가 날 수 있다. 따라서 partial predicate와 `indrestrictinfo` 정규화는 rel의
  index/predicate tree 안에 이미 같은 canonical slot expression이 존재하는 경우에만 적용하고,
  raw `agtype_access_operator` prefix로 만든 기존 index surface는 그대로 보존한다.
- 다음 구현은 새 aggregate helper 나열이 아니라 기존
  `add_deferred_ordered_property_projection_path`,
  `add_deferred_count_agtype_projection_path`,
  `add_narrow_distinct_collect_paths`를 descriptor 기반 공통 구조로 정리하거나, 그 구조로
  커버 가능한 한 shape를 좁게 추가하는 방식이어야 한다.

## Neo4j Cypher planning 참고

조사 대상:

- `/Users/emotionbug/IdeaProjects/neo4j/community/cypher/cypher-planner`
- `/Users/emotionbug/IdeaProjects/neo4j/community/cypher/physical-planning`
- `/Users/emotionbug/IdeaProjects/neo4j/community/cypher/slotted-runtime`

관찰:

- Neo4j는 logical planning, physical planning, slotted runtime을 별도 계층으로 두고,
  projection/distinct/aggregation이 runtime slot과 grouping table에서 어떤 값을 실제로
  들고 갈지 분리한다.
- `SlotConfiguration`, `SlottedRewriter`, `SlottedPrimitiveGroupingAggTable`,
  `DistinctSlottedPrimitivePipeTest`, `PruningVarExpanderTest`는 AGE의
  `PathTarget`/descriptor 분리와 VLE materialization boundary를 비교할 우선 참고 지점이다.
- `SlottedPrimitiveGroupingAggTable`은 grouping key가 primitive node/relationship이면
  `CypherRow`의 long slot offset 배열만 읽어 `LongArray` key를 만든다. grouping table이
  full value object를 들고 가지 않는다.
- `SlotConfiguration`은 `CachedPropertySlotKey`를 별도 key로 보관하고 cached property
  offset 구간을 추적한다. `SlottedCachedProperty`는 entity slot offset, property token,
  cached property ref offset, `needsValue`/`hasProperty` 구분을 descriptor로 들고 간다.

AGE에 대한 결론:

- AGE는 아직 PostgreSQL `PathTarget` expression 자체가 slot/descriptor 역할을 겸한다.
  그래서 upper path가 만들어진 뒤 reltarget만 바꾸면 PostgreSQL create_plan 단계에서
  subplan target lookup이 깨질 수 있다.
- GROUP/DISTINCT key에서 `agtype_access_operator()`를 뒤늦게 path target에서 바꾸는
  시도는 `variable not found in subplan target list`로 실패했다. 이 shape는 parser 또는
  lower target 생성 시점에서 같은 expression surface를 만들어야 한다.
- 이번 단위에서는 generic property access의 semantic type은 `agtype`으로 유지하되,
  select target의 simple 2-argument access를 처음부터 `agtype_object_field_agtype()`로
  만들고 ORDER/GROUP target matching에서 기존 accessor와 같은 signature로 취급하도록
  바꾸는 방향이 안정적이다.
- 다음 AGE 후보는 PostgreSQL `PathTarget` 안에서 같은 expression을 반복하는 대신,
  property key/token과 entity descriptor를 별도 cached-property descriptor로 다루는
  구조다. 우선은 `agtype_object_field_agtype()`처럼 stable expression surface를 줄이고,
  이후 `PathTarget`/CustomPath private metadata로 cached property slot에 가까운 handoff를
  실험한다.

## Property access와 expression index

관찰:

- 기존 `agtype_access_operator(properties, key)` surface로 만든 expression index가 있다.
- `agtype_object_field_equals/cmp/exists_nonnull` 같은 direct helper는 no-index workload에서
  `agtype` wrapper 생성을 줄여 빠르다.
- 하지만 helper expression은 기존 accessor expression index와 구조가 다르기 때문에 index
  matching을 깨뜨릴 수 있다.

결론:

- expression index가 없는 baserel에서는 direct helper rewrite를 사용한다.
- matching 가능한 expression index가 있으면 기존 accessor surface를 유지한다.
- direct helper expression index처럼 기존 accessor와 tree shape가 달라도 logical
  `(container, key path)` signature가 같으면 index expression surface를 선택해야 한다.
- nested property index도 단일 key helper guard가 아니라 property key path descriptor로
  비교해야 한다. 그래야 `n.payload.a`가 direct helper chain index와 variadic access index
  양쪽 surface를 모두 사용할 수 있다.
- AGE의 `set_rel_pathlist_hook`은 PostgreSQL `create_index_paths()` 이후에 실행되므로,
  hook 안에서 filter expression만 바꾸면 새 index path가 생기지 않는다. rewrite가 발생한
  relation은 `create_index_paths()`를 다시 호출해야 expression index 후보가 반영된다.
- PostgreSQL `indxpath.c`의 `match_index_to_operand()`는 expression index key를 `equal()`로
  비교하고, partial index는 `check_index_predicates()`가 `predicate_implied_by()` 결과를
  `IndexOptInfo.predOK`에 저장한 뒤 `create_index_paths()`에서 사용한다. AGE가
  `baserestrictinfo`의 property accessor surface를 hook에서 바꾸면 index path만 다시
  만들 것이 아니라 partial predicate 상태도 다시 계산해야 한다.
- property access comparison은 bool helper에 숨기지 않고
  `CypherPropertyAccessSignature(container, keys)` descriptor 추출 API로 열어둔다. expression
  index key, direct helper chain, variadic accessor가 같은 logical property path인지 같은
  descriptor에서 비교해야 이후 cached-property/scalar descriptor로 확장할 수 있다.
- 1번의 남은 조사는 expression rewrite 자체보다 index 구조다. PostgreSQL
  `IndexOptInfo.indexprs`, `indpred`, operator class matching과 AGE property descriptor를
  연결해, property path가 expression tree 우연한 동일성에만 의존하지 않는 index key
  layout으로 갈 수 있는지 먼저 판단해야 한다.
- `create_property_index()`가 property name 하나만 index key로 만들면 planner의 nested
  signature matching이 있어도 nested property workload는 실제 index structure를 만들 수
  없다. 그래서 dotted path 입력은 `VARIADIC ARRAY[properties, key...]` expression index로
  생성하도록 바꿨고, planner rewrite가 같은 logical `(container, key path)` descriptor로
  이 surface를 선택한다.
- typed scalar index는 query expression surface와 btree opfamily가 함께 맞아야 한다.
  `create_property_index(..., 'score', 'pg_bigint')`는
  `agtype_object_field_int8(properties, key)` expression index를 만들고,
  parser가 `n.score::pg_bigint >= 10`의 RHS `agtype` literal을 typed lhs의 scalar type으로
  낮춰 int8 btree `Index Scan`을 타게 한다.
- nested typed scalar index는 전체 path를 scalar helper 하나에 넣는 방식이 아니라,
  `payload.score` 같은 path를 `prefix object(payload) + terminal key(score)` descriptor로
  나눠야 PostgreSQL btree opclass와 맞는다. 그래서
  `create_property_index(..., 'payload.score', 'pg_bigint')`는
  `agtype_object_field_int8(agtype_access_operator(VARIADIC ARRAY[properties, '"payload"']),
  '"score"')` expression index를 만들고, parser의 `n.payload.score::pg_bigint`도 같은
  expression surface로 낮춘다. 이 layout은 `IndexOptInfo.indexprs`의 `equal()` matching에
  직접 걸리며, nested agtype path index와 typed scalar opclass를 연결하는 첫 단계다.
- `numeric` property cast는 PostgreSQL `numeric` Datum으로 낮추지 않고
  `agtype_object_field_numeric_agtype(prefix, key)`가 numeric agtype 값을 반환하는 layout이다.
  따라서 `create_property_index(..., 'payload.gpa', 'numeric')`도 같은 numeric-agtype helper
  expression을 만들어야 parser의 `n.payload.gpa::numeric` surface와 `IndexOptInfo.indexprs`가
  직접 matching된다. 이 단계는 numeric value를 scalar slot으로 빼는 최종 구조는 아니지만,
  numeric property path도 typed descriptor/index handoff에 참여하게 만든다.
- typed property index DDL 생성도 `property type -> helper function`과
  `property path -> prefix object + terminal key`를 분리해야 한다. 이 분리는 Neo4j의
  cached property가 entity variable, property key, cached value 여부를 별도 descriptor로 들고
  가는 구조와 같은 방향이며, AGE에서는 다음 단계의 cached-property slot/index metadata가
  terminal descriptor를 재사용할 수 있게 한다.
- no-index nested property predicate도 같은 prefix/terminal descriptor를 쓴다.
  `n.payload.a = 1`과 `n.payload.a > 0`은 전체 terminal value를
  `agtype_access_operator(VARIADIC ARRAY[properties, payload, a])`로 만든 뒤 비교하지 않고,
  `agtype_object_field_equals/cmp(prefix_object, terminal_key, rhs)`로 낮춘다. 첫 시도에서
  prefix access의 `funcvariadic` flag가 빠져 nested pattern match 결과가 깨졌고, 이를
  `VARIADIC ARRAY[...]` surface로 고정해 semantic을 회복했다.
- ordered property projection의 ctid refetch는 `relid, ctid, key`만으로는 vertex
  properties attno를 암묵적으로 가정한다. edge relation까지 같은 구조를 쓰려면
  descriptor에 `properties_attno`를 포함해야 하므로
  `agtype_ctid_property_field_agtype(relid, ctid, properties_attno, key)` 형태로 일반화했다.
- ORCA식 lower/final 분리는 sort key type별 함수 나열이 아니라, lower target이
  `ctid + typed property sort expression`을 들고 final target이 `ctid` refetch로 agtype
  output을 만드는 descriptor로 보는 편이 맞다. AGE에서는 이 descriptor를
  `agtype_object_field_int8/float8/text_agtype/numeric_agtype` surface로 인식한다.
- GROUP count도 같은 관점에서 lower target은 typed property key와 raw `count(*)`를 들고,
  final target에서만 `count`를 agtype으로 감싼다. `pg_float8`, `pg_text`, `numeric` key도
  `pg_bigint`와 같은 lower/final split이 유지되는지 plan assertion으로 확인했다.
- 같은 scalar-to-agtype final output이 반복되면 lower target은 ORCA의 required column set처럼
  동일 physical scalar expression을 한 번만 요구해야 한다. duplicate `count(*)` final output은
  final `Result`에서 두 agtype 값을 만들 수 있지만, lower `HashAggregate`/`Sort` target은
  raw `count(*)` 하나만 carry하면 충분하다. 이는 cached-property slot/index handoff에서도
  같은 `(container, key path, value type)` descriptor를 중복 slot으로 만들지 않는 기준이 된다.
- `equal()`이 아닌 property signature match로 lower scalar descriptor를 합칠 때는 final
  wrapper도 canonical lower expression을 바라보게 바꿔야 한다. PostgreSQL setrefs는 expression
  tree identity로 subplan target을 찾기 때문에, lower target만 생략하고 final target에 원래
  surface를 남기면 `variable not found in subplan target list` 계열 실패가 날 수 있다.
- scalar-to-agtype final materialization의 lower target insertion도 planner orchestration에
  남기면 cached-property slot handoff가 다시 expression tree local rule로 갈라진다.
  `cypher_property_paths.c`가 `PathTarget` 안의 existing scalar expression을 property
  signature descriptor로 찾아 canonical lower expression을 돌려주면 final wrapper, expression
  index matching, cached-property slot 후보가 같은 `(container, key path, value type)`
  contract를 공유할 수 있다.
- numeric property collect는 generic `age_collect(agtype_object_field_numeric_agtype(...))`
  대신 `age_collect_numeric_property(properties, key)` descriptor aggregate로 낮출 수 있다.
  transition state는 numeric array를 직접 쌓고 final에서만 agtype list를 만들므로,
  property fetch -> numeric agtype wrapper -> collect transition 경로를 줄인다.
- `array_agg` property/map/list aggregate는 새 aggregate object를 계속 늘리지 않고 기존
  `age_array_agg_property`, `age_array_agg_map2_property`,
  `age_array_agg_map_property`, `age_array_agg_list_property`의 key 인자 contract를
  scalar key 또는 key-path agtype list로 확장하는 편이 ORCA식 descriptor handoff에 더
  맞다. planner는 `CypherPropertyAccessSignature(container, keys)`에서 root properties와
  path descriptor를 만들고, executor transition은 root properties에서 직접 path lookup을
  수행한다. 이 구조는 nested `array_agg(n.payload.a)`, `{a: n.payload.a, b: n.payload.b}`,
  `[n.payload.a, n.payload.b]`에서 prefix object materialization을 aggregate input으로
  carry하지 않는다.
- 이 aggregate rewrite가 `cypher_paths.c`에 남아 있으면 upper path hook 내부의 ad hoc
  함수 나열이 계속 늘어난다. collect/numeric/array_agg property aggregate rewrite는
  `cypher_property_paths.c`의 property signature descriptor 모듈에 모아야 cached-property
  slot이나 expression index handoff도 같은 `(container, key path, value type)` contract를
  재사용할 수 있다.
- `count(n.prop)` non-null rewrite도 같은 module로 이동했다. 이제 upper path hook은
  `PathTarget` 하나를 넘기고, collect/numeric/array_agg/count property aggregate rewrite와 관련
  OID cache는 property descriptor module에서 함께 관리한다.
- `array_agg` property/map/list handoff 이동은 이 방향의 첫 정리다. path hook은 lower target과
  aggregate path 조립을 맡고, property module은 aggregate descriptor detection, key-path slot
  expression, slots aggregate OID cache, target rewrite를 소유한다. 다음 slot-vector aggregate
  state 확장도 planner orchestration에 새 함수 나열을 더 쌓지 않는 방향으로 진행한다.
- aggregate child required target도 같은 boundary에 있어야 한다. typed collect와
  `array_agg` property narrow path의 group expr 보존, arg expression 선택, sortgroupref assignment는
  `cypher_property_paths.c`가 `CypherTypedCollectArgPlan`과 `CypherArrayAggPropertyArgPlan`으로 만들고,
  hook은 ORCA의 physical Agg처럼 child path와 AggPath 조립만 담당한다.
- expression index matching도 같은 이유로 `cypher_paths.c`의 hook-local helper로 두면
  cached-property slot/index handoff가 다시 planner orchestration에 묶인다.
  `cypher_property_paths.c`가 query property expression과 `IndexOptInfo.indexprs` 또는
  catalog expression index surface를 같은 property signature descriptor로 비교하고
  canonical index expression을 돌려주는 편이, restriction rewrite, final scalar descriptor,
  cached-property slot 생성 지점이 같은 물리 expression contract를 공유하기에 맞다.
- typed DISTINCT collect는 `pg_float8`도 int8/text와 같은 구조로 동작한다. regression은
  `age_collect_float8(DISTINCT ...)`가 properties object를 carry하지 않는지 확인한다.
- nested property count는 전체 nested value가 아니라 마지막 key의 non-null 여부만 필요하다.
  `n.payload.a`는 `prefix object(n.payload) + key(a)` descriptor로 낮추면 final value
  materialization을 피하면서 `count(expr)`의 nullness semantic을 유지할 수 있다.
- edge ordered property projection의 초기 시도는 endpoint join subpath에서 PostgreSQL
  `setrefs.c`의 subplan target lookup이 깨져 `variable not found in subplan target lists`가
  발생했다. 돌파구는 child path target을 기존 target 복사에서 시작하지 않고, required
  expression과 join key만 다시 구성하는 것이다. child rel에서 완전히 계산 가능한 typed
  property sort expression은 computed column처럼 child target에 두고, 그 내부
  `r.properties` raw var는 join output으로 다시 carry하지 않는다.
- `count(startNode(...).name)` 계열 VLE endpoint property count는 planner에서
  `agtype_object_field_exists_nonnull(entity, key)`로 낮아질 수 있다. 이를 guard로 막으면
  descriptor 확장이 다시 호출자별 예외로 갈라진다. 더 나은 선택은
  `agtype_object_field_exists_nonnull` 자체가 properties object뿐 아니라 scalar vertex/edge
  entity를 받아 내부 properties object에서 key 존재 여부를 확인하도록 contract를 넓히는 것이다.
- expression index matching도 key path만 비교하면 typed scalar helper와 generic property
  accessor가 같은 shape로 뭉개질 위험이 있다. property signature descriptor에 value type을
  포함하면 `agtype_object_field_int8(prefix, key)` 같은 typed expression index와 query
  property surface가 같은 physical scalar contract일 때만 matching된다. chained-prefix index와
  variadic/nested query surface가 `Index Scan`으로 연결되는지 `EXPLAIN` 출력으로 확인했다.
- partial expression index는 `IndexOptInfo.indpred`까지 같은 rewritten surface를 봐야 한다.
  typed partial index `agtype_object_field_int8(prefix, key) >= 10`은 query restriction을
  descriptor-matched index expression으로 바꾼 뒤 `check_index_predicates()`를 다시 실행해야
  `predOK`가 켜지고 `Index Scan` 후보가 생긴다. 이 경로는 `index` regression의 출력 plan으로
  고정했다.
- cached/non-null terminal property handoff도 별도 extractor를 유지하면 variadic accessor,
  direct object-field chain, typed scalar helper가 다시 갈라진다. terminal object/key는
  `CypherPropertyAccessSignature(container, keys, value_type)`에서 마지막 key만 분리하고,
  prefix object는 `agtype_object_field_agtype` chain으로 재구성한다. 이 layout은 partial/index
  descriptor와 no-index cached helper가 같은 prefix/terminal contract를 공유하게 한다.
- ordered property projection refetch도 단일 key contract에 머무르면 nested key에서 lower
  target이 prefix object를 carry해야 한다. key path descriptor를 final refetch builder에 넘기고,
  final expression을 `agtype_ctid_property_field_agtype(relid, ctid, attno, first_key)` 뒤
  `agtype_object_field_agtype(..., next_key)` chain으로 조립하면 `LIMIT` 아래에는 lookup key와
  typed sort key만 남는다. 이는 ORCA식 required/computed 분리에 더 가깝고, nested property
  final materialization을 path-wide descriptor로 다룰 수 있게 한다.
- parser의 scalar property typecast rewrite도 같은 terminal descriptor를 써야 한다. 이 경로가
  자체 nested prefix extractor를 유지하면 typed aggregate와 scalar final materialization에서
  `agtype_access_operator(VARIADIC ARRAY[...])` surface가 다시 나타난다. 공통 descriptor로
  바꾸면 `collect(DISTINCT n.payload.a::pg_bigint)`는 `age_collect_int8`를 유지하면서
  `agtype_object_field_int8(agtype_object_field_agtype(...), key)` surface를 쓰고, nested numeric
  collect도 direct object-field prefix를 aggregate input으로 받는다.
- nested numeric collect는 한 단계 더 줄일 수 있다. `age_collect_numeric_property(prefix, key)`는
  prefix object를 scan target에서 계산해야 하지만, `age_collect_numeric_path_property(properties,
  key_path_array)`는 aggregate transition 안에서 key path를 순회하므로 lower target은 원본
  properties만 남긴다. `collect(n.payload.a::numeric)` 결과와 plan 출력에서 이 contract를
  확인했다.
- GROUP/DISTINCT 잔여 variadic shape를 다시 추적한 결과, 일반 label relation의 nested
  property index descriptor보다 fixed path indexed edge/endpoint property와 VLE boundary
  property가 남아 있었다. 이 경우 이미 `r.properties`, `a.properties`,
  `age_vle_edge_properties_at(...)`, `age_vle_node_properties_at(...)`처럼 properties object
  descriptor가 확보되어 있으므로, 그 뒤 key path는 variadic accessor가 아니라 direct
  helper chain으로 낮추는 것이 index/expression surface를 단순하게 만든다.
- 다음 구조 후보는 helper를 더 늘리는 것이 아니라 final output descriptor와 typed/scalar
  physical key를 분리하는 것이다.

## Aggregation/projection materialization

관찰:

- int/bool/numeric/text scalar equality/hash/sort fast path는 개별 workload에서 개선이
  있었지만, generic key materialization 자체는 남아 있다.
- typed DISTINCT collect finalization과 narrow sort input은 개선 폭이 제한적이었다.
- ordered projection에서 sort key를 final output으로 직접 재사용하면 `RETURN n.i`와
  `ORDER BY n.i::pg_bigint`처럼 cast semantic이 갈리는 shape에서 값이 바뀔 수 있다.
- lower path가 output `agtype`를 함께 carry하는 접근은 sort width 증가로 악화될 수 있다.

결론:

- comparator micro path를 반복하지 않는다.
- HashAggregate/GROUP BY/DISTINCT 입력 width와 final output descriptor를 줄이는 구조를
  우선한다.
- `count(*)`를 agtype으로 감싸는 output은 aggregate transition 자체와 별개로 upper target
  projection에서 처리할 수 있다. `LIMIT`이 없는 grouped count에서도 lower path를 raw
  `count(*)`로 바꾸고 final `Result`에서만 `int8_to_agtype(count)`를 적용하면
  `HashAggregate`/`Sort`가 agtype count wrapper를 carry하지 않는다.
- fresh probe에서 `pg_bigint`, `pg_float8`, `pg_text`, `numeric` grouped count key 모두
  non-LIMIT plan의 lower `HashAggregate`/`Sort`가 typed key와 raw `count(*)`만 carry했다.
  남은 문제는 count 전용 fast path가 아니라 scalar-to-agtype final materialization을
  공통 descriptor로 보고 다음 property/cached slot handoff가 재사용할 수 있게 하는 것이다.
- final projection 직전까지 typed/scalar Datum을 유지하는 planner/executor handoff
  metadata를 설계한다.
- nested property key는 variadic `agtype_access_operator()` 하나보다
  `agtype_object_field_agtype()` chain이 lower target의 required expression surface를
  줄이고, ORCA식 computed column pruning에 더 가까운 형태다.
- `RETURN n.payload.a, count(*) ORDER BY n.payload.a`와
  `RETURN DISTINCT n.payload.a ORDER BY n.payload.a`는 같은 logical property access를
  sort/group expression matching에서 동일하게 볼 수 있도록 nested property signature
  helper로 해결했다. 이 방향은 helper 함수를 더 나열하는 것보다 descriptor 기반
  lower/final target 분리에 더 가깝다.

## VLE path/list semantics

관찰:

- `length(p)`, `count(p)`, endpoint-only, property-only consumer는 path/list 전체
  materialization 없이 처리할 수 있다.
- arbitrary VLE에서 `nodes(p)`/`relationships(p)` full list, list element projection,
  `.id` property semantics가 필요한 shape는 materialization fallback이 필요하다.
- fixed path arity-specific helper를 늘리는 방식은 유지보수성과 planner lowering을
  악화시킨다.

결론:

- `_agtype_build_path_label*` 계열을 늘리지 않는다.
- raw descriptor + `_agtype_build_path_raw` 또는 direct VLE helper를 사용한다.
- consumer별 contract를 명확히 나눠 materialization fallback 조건을 좁힌다.

## Graph DB / RPQ 참고

### Neo4j / Cypher planning

- Variable-length path planning은 endpoint bound 여부, path variable 사용 여부, uniqueness
  mode, projected field에 따라 execution shape를 달리한다.
- AGE도 consumer shape를 먼저 분류하고, path variable이 실제로 필요한 경우에만 path/list
  materialization을 유지해야 한다.

### SQL/PGQ / DuckPGQ

- SQL/PGQ 계열은 path pattern과 path output을 별도 semantic layer로 다룬다.
- AGE도 parser에서 바로 `agtype` container를 만들기보다, planner-visible descriptor를
  통해 final output 단계에서 materialize하는 구조가 더 적합하다.

### Packed adjacency / dense traversal state

- Packed adjacency와 dense edge/vertex state는 VLE cold-load와 repeated traversal에서
  locality를 개선할 수 있다.
- AGE의 current graph catalog/table layout과 완전히 호환되는 작은 변경만으로는 cold
  global graph load의 label-unrelated scan을 크게 줄이기 어렵다.
- `age_adjacency` payload cache는 fan-out 여부를 알기 위해 첫 source vertex를 이미 한 번
  스캔한다. 기존에는 이 scan 결과를 DFS frame push에만 쓰고 cache에는 남기지 않아 같은
  source vertex를 cycle/반복 traversal에서 다시 만나면 directory/main payload scan을 반복했다.
  첫 scan을 cache entry seed로 사용하면 dense traversal state와 payload cache contract가
  맞물리고, fan-out이 1개 이하인 workload에서는 payload 배열을 즉시 비워 no-cache 경로를
  유지할 수 있다.
- VLE traversal frame은 candidate push 시 `next_vertex_entry`를 이미 carry할 수 있다. terminal
  property direct output이 이 entry를 무시하고 id 기반 lookup helper를 다시 타면 frame/cache
  contract가 끊긴다. terminal property fetch를 `vertex_entry` 중심 helper로 통합하면 cached
  property hit, block prefetch, relation-cache fallback이 같은 entry pointer에서 시작한다.
- targeted edge-label VLE에서 `age_adjacency` index payload가 traversal endpoint를 공급하고
  `load_vertex_metadata=false`인 경우, global graph context는 vertex hashtable을 만들지 않는다.
  이때 전체 vertex label list는 `load_vertex_hashtable()`의 scan 대상 계산에만 쓰이므로,
  label list를 채우기 위해 `ag_label`을 다시 스캔하는 것은 adjacency-index load contract 밖의
  비용이다. vertex metadata를 실제로 로드하는 fallback에서만 vertex labels를 채운다.
- `edge_label_has_age_adjacency_indexes()`는 targeted load마다 edge relation의 index list를 열어
  outgoing/incoming `age_adjacency` index를 검사했다. edge label OID와 label cache generation을
  함께 캐시하면 같은 label의 반복 VLE load에서 relcache/index open loop를 피하면서, label/index
  DDL 후에는 generation 변경으로 다시 판정할 수 있다.
- VLE graph/label lookup은 단일-entry cache만으로도 같은 query 반복에는 충분하지만, 여러 edge
  label을 번갈아 탐색하는 workload에서는 바로 miss가 난다. 작은 fixed-size cache를 graph cache
  generation/label cache generation과 함께 쓰면 invalidation contract는 유지하면서 alternation
  workload의 graph OID와 label relation OID lookup 반복을 줄일 수 있다.

결론:

- 예전 data layout 하위 호환성은 이 브랜치의 제약이 아니다.
- 필요하면 adjacency payload layout, dense traversal state, load contract 변경을 직접
  실험한다.

## agtype GIN path ops

관찰:

- 기존 `gin_agtype_ops`는 nested containment에서 후보가 많고 heap recheck 비용이 크다.
- PostgreSQL `jsonb_path_ops`와 유사하게 path hash를 사용하는 opclass가 nested scalar
  containment 후보 수를 줄인다.
- 깊은 nested single-pair recheck는 direct fixed path lookup으로 `agtype_deep_contains()`
  fallback을 피할 수 있다.

결론:

- `gin_agtype_path_ops`는 nested containment workload에서 유지할 가치가 있다.
- path hash stack allocation은 inline stack + overflow 구조가 적합하다.
- multi-key 또는 semantic이 복잡한 nested constraint는 deep fallback을 유지한다.

## Extension upgrade compatibility

- 이 최적화 브랜치는 fresh install 기준 성능/동작을 우선한다.
- extension upgrade catalog parity, 예전 data layout 보존, upgrade template completeness는
  gate가 아니다.
- `age_upgrade` regression은 test 목록에 남아 있지만 하위 호환성 검증이 아니라 정책 smoke
  test다.

## PostgreSQL createplan: plain Agg lower projection

관찰:

- lldb에서 `try_narrow_typed_collect_path()`가 non-DISTINCT
  `collect(n.payload.a::pg_numeric)`용 새 `AggPath`를 반환했고, 같은 포인터가
  PostgreSQL `create_agg_plan()`의 `best_path`로 들어오는 것을 확인했다.
- `AggPath->subpath`는 `ProjectionPath(T_Result) -> SeqScan` 구조였지만,
  `create_agg_plan()`은 child plan을 `CP_LABEL_TLIST`로 만들고,
  `create_projection_plan()`은 label이 없는 target에 대해 `use_physical_tlist()`를 허용했다.
  그 결과 lower projection이 사라지고 `Seq Scan Output: n.id, n.properties`가 유지됐다.
- `setrefs.c`는 child tlist의 non-var expression을 `OUTER_VAR`로 치환할 수 있지만, 그 전 단계에서
  child tlist 자체가 physical tlist로 되돌아가면 aggregate input scalar slot을 만들 수 없다.

결론:

- plain aggregate input을 줄일 때는 path 선택 비용만 조정하는 것으로 부족하다.
- lower target이 실제 executor-visible slot이 되려면 PostgreSQL `create_projection_plan()`이
  physical tlist fallback을 선택하지 못하도록 label metadata까지 함께 넘겨야 한다.
- 이번 변경은 non-DISTINCT typed collect lower target에 label을 부여해 child scan output을
  scalar property expression 하나로 고정했다. 다음 확장은 grouped aggregate와 multi-aggregate에서도
  같은 lower/final descriptor boundary를 유지하는 방향이다.
- grouped `AggPath`에서는 `create_agg_plan()`이 `extract_grouping_cols()`로 child tlist의
  `ressortgroupref`를 찾는다. 따라서 collect input만 줄이더라도 lower target은 group key expression과
  sortgroupref를 원형대로 보존해야 한다. `AGG_HASHED`/`AGG_SORTED`에 대한 확장은 subpath
  `PathTarget`에서 groupClause sortgroupref가 붙은 expression을 먼저 복사하고, collect input scalar
  expression을 추가하는 방식이 PostgreSQL grouping contract와 맞는다.
- 여러 typed collect가 같은 aggregate target에 있을 때 단일 `CypherTypedCollectHandoff` detector는
  좋은 path 후보를 포기하게 만든다. ORCA식 required/computed column 관점에서는 aggregate별
  computed scalar input을 list로 수집하고, 하나의 lower target에 필요한 scalar slot을 모두 싣는
  편이 맞다. DISTINCT collect는 aggregate별 ordering metadata가 얽히므로 multi path에서는 아직
  제외하고, non-DISTINCT multi collect부터 같은 lower/final descriptor boundary로 낮춘다.
- 단일 property `array_agg`는 `age_array_agg_property(properties, key_path)` descriptor aggregate가
  per-row final `agtype` materialization은 피하지만, child scan width는 full `properties`에 묶어 둔다.
  큰 payload workload에서는 aggregate 내부 lookup보다 lower tlist width가 더 큰 병목이 될 수 있으므로,
  path 단계에서 `key_path` const를 canonical property expression으로 복원하고 `array_agg(scalar_expr)`로
  되돌리는 편이 ORCA식 computed column pruning에 더 가깝다. map/list aggregate는 여러 property slot과
  final map/list materialization contract가 필요하므로 같은 path-const 복원 helper를 기반으로 별도 확장한다.
- map2/list `array_agg`는 우선 per-row `agtype_build_map_nonull`/`agtype_build_list`를 aggregate input으로
  만들었다. 이 선택은 aggregate 내부 descriptor lookup보다 row expression materialization을 앞당기는
  tradeoff가 있지만, full `properties` varlena carry를 제거한다. 다음 구조 개선은 map/list row expression도
  aggregate final 단계까지 늦출 수 있도록 output key descriptor와 scalar slot list를 함께 넘기는
  multi-slot final descriptor다.
- lldb 확인 결과 map2/list rewrite는 PostgreSQL planner의 `create_grouping_paths()` 이후
  `create_upper_paths(UPPERREL_GROUP_AGG)` hook에서 추가한 AGE custom path 후보 안에서 실행된다.
  따라서 실패 지점은 executor aggregate 함수가 아니라 `add_narrow_array_agg_property_paths()`의
  target rewrite와 child target 구성 경계로 봐야 한다.
- 3개 이상 map aggregate도 같은 결론을 따른다. ORCA의 `CColRefSet`/`CPhysicalComputeScalar`
  방향처럼 array descriptor aggregate를 required input으로 유지하지 않고, key path별 computed
  property expression을 lower target에 드러내면 PostgreSQL `create_projection_plan()`이 full
  `properties` carry 대신 필요한 computed expression만 physical tlist로 잡을 수 있다. Neo4j
  slotted runtime의 cached/indexed property offset 구조도 같은 방향이다. property value를
  container와 분리된 slot metadata로 다루는 쪽이 다음 multi-slot final descriptor의 근거다.
- `age_array_agg_map_slots`/`age_array_agg_list_slots`는 이 방향의 중간 단계다. 표준
  `array_agg(anynonarray)`는 하나의 input Datum만 받기 때문에 map/list row expression을 미리
  materialize해야 하지만, variadic slots aggregate는 child target을 key/value 또는 value scalar
  slot들로 유지한다. 아직 transfn에서 row별 agtype element를 만들지만, aggregate 호출 contract가
  slot vector 형태가 되었으므로 다음 단계에서 state를 element array가 아니라 descriptor + slot
  values layout으로 바꿔 final materialization을 더 늦출 수 있다.
- slot aggregate state를 실제 slot-vector layout으로 바꾸면 row별 map/list element 생성은 finalfn까지
  늦춰진다. 이 구조는 `ArrayBuildState`보다 combine/serialize contract를 별도로 설계해야 하지만,
  큰 hash aggregate에서 transition 단계의 agtype object/list serialization을 피할 수 있는 더 나은
  boundary다. 다음 판단은 이 state layout을 cached-property slot/index descriptor와 공유하고,
  parallel aggregate가 필요하면 descriptor + flattened slot values serialize format을 추가하는 것이다.
- combine contract는 slot-vector state끼리 직접 append하는 방식으로 먼저 연결했다. PostgreSQL
  `array_agg_combine`처럼 aggregate context로 state를 복사하거나 기존 state 뒤에 붙이되, map/list
  element를 만들지 않는다. serialize/deserialize는 worker 간 internal state transfer가 실제 plan에서
  필요해지는 시점에 descriptor + flattened slot values format으로 설계한다.
- slot-vector aggregate state의 serialize format은 version, map/list flag, slot count, row count,
  optional map key strings, flattened `(null flag, agtype varlena bytes)` values로 둔다. 현재 slots aggregate의
  value input은 planner가 만든 `agtype` slot expression이므로 generic Datum type table까지 넣지 않는다.
  이 좁은 format은 partial aggregate transfer에서도 row별 map/list element materialization을 피하면서,
  나중에 cached-property slot/index metadata를 붙일 수 있는 descriptor boundary를 남긴다.

## VLE terminal batch와 skeletal vertex cache

- targeted fallback VLE는 endpoint vertex table scan을 피하기 위해 skeletal `vertex_entry`를 먼저
  만든다. 이후 materialization이 필요하면 vertex id index로 lazy hydrate한다.
- terminal property batch fetch는 같은 vertex label table tuple을 이미 읽고 있으므로, 이 시점에
  `vertex_entry.tid`, `cached_properties`, terminal scalar property cache까지 채우는 것이 더 낫다.
  결과를 위한 property extraction과 materialization fallback을 위한 lazy hydrate가 같은 heap tuple을
  중복으로 읽지 않게 된다.
- 이 변경은 terminal-property workload에 직접 맞지만, 구조상 다음 확장은 terminal property가 아닌
  materialization-heavy path의 vertex id 목록을 label별로 모아 같은 batch hydrate helper를 호출하는
  방향이다.

## VLE adjacency payload cache policy

- 기존 `age_adjacency` payload cache는 방향별 전역 `decided/enabled` flag로 동작했다. 첫 source
  vertex의 fan-out이 1개 이하이면 같은 방향의 이후 source가 dense fan-out이어도 cache seed 자체를
  만들지 못하는 구조였다.
- typed start/end endpoint를 traversal root로 정규화한 뒤에는 cache policy도 root/source 단위로
  내려가는 편이 맞다. ORCA의 required column 판단처럼 전역 shape 하나가 아니라 실제 required
  input key에 가까운 `(index_oid, source_vertex_id)`가 cache contract가 된다.
- 따라서 payload replay 여부를 `VLEAdjacencyPayloadCacheEntry` 존재와 payload 배열 유무로 판단하도록
  바꿨다. fan-out이 2개 이상인 source는 첫 scan payload를 entry에 저장하고 재방문 시 replay하며,
  fan-out이 1개 이하인 source는 payload 배열을 비워 no-cache 경로를 유지한다.
- payload cache key/entry lifecycle을 `age_vle_adjacency_cache.c`로 분리했다. traversal callback은
  아직 `age_vle.c`에 남겨 graph state와 edge-state flag 접근을 유지하지만, cache allocation/free
  boundary는 독립 모듈이 되었으므로 다음 단계에서 traversal state 또는 materializer hydrate 후보를
  같은 source entry에 붙일 수 있다.
- adjacency prefetch 진입도 `VLEAdjacencyRootDescriptor`로 좁혔다. 이전에는 source vertex, index oid,
  outgoing 여부, self-loop skip policy가 helper argument로 흩어져 있어 cache key와 scan source가 같은
  root contract를 공유한다는 점이 코드에 드러나지 않았다. descriptor 전환 뒤에는 missing-vertex fallback과
  loaded-vertex adjacency expansion 모두 같은 root descriptor를 만들고, payload cache key도 그 descriptor의
  `(index_oid, source_vertex_id)`를 사용한다.
- materializer cache도 같은 방향으로 분리했다. `age_vle_materializer_cache.c`는 graph OID별
  function-context cache와 vertex/edge object/typed object hash table을 관리하고, `age_vle.c`는
  graph semantic builder callback만 넘긴다. 이는 materialization-heavy path에서 object cache layout을
  바꾸더라도 traversal/path container builder를 덜 건드리게 만드는 경계다.
- traversal stack도 `age_vle_traversal.c`로 분리했다. 아직 DFS policy는 `age_vle.c`에 남아 있지만
  frame stack과 path stack mutation이 독립 API가 되면서, 다음 단계에서 edge-state flags/local edge
  index를 traversal state object로 묶거나 endpoint/root descriptor를 stack seed contract로 넘길 수
  있는 위치가 생겼다.
- 그 다음 단계로 dense edge-state flags와 local edge-index hash를 `VLELocalEdgeState`에 묶어 같은
  traversal 모듈로 옮겼다. DFS 함수는 `VLE_EDGE_STATE_USED/MATCHED` 같은 policy bit를 직접 갱신하지만,
  dense index allocation과 bounds validation은 storage API가 담당한다. 이 경계는 향후 endpoint/root
  descriptor별 traversal-state seed나 adjacency payload replay와 edge-state allocation을 연결할 수
  있는 기반이다.
- DFS의 backtrack/consume 반복을 `age_vle_consume_next_frame()`으로 `age_vle_traversal.c`에 옮겼다.
  `age_vle.c`는 cached terminal vertex handoff만 얇게 감싸며, path stack mutation과 used-edge
  backtracking은 traversal storage API에 붙었다. 다음 단계는 acceptance/output policy도 descriptor와
  함께 traversal module로 넘기는 것이다.
- next-vertex expansion과 found terminal result cache handoff도 helper로 통합했다. 이는 DFS loop에서
  traversal mechanics, expansion policy, acceptance policy, output policy를 분리하는 중간 단계이며,
  terminal-property 전용 DFS의 direct scalar cache만 별도 policy로 남긴다.
- length/end acceptance 조건을 helper로 분리하면서도, accepted path를 반환하기 전에 next depth candidate를
  push하는 기존 SRF traversal order는 유지했다. 이 순서는 cached iterator가 successive call에서 deeper
  path 후보를 이어서 탐색하기 위한 contract로 남긴다.
- ORCA의 `CExpressionPreprocessor::PexprPruneUnusedComputedCols`와
  `CPhysicalComputeScalar::PcrsRequired`는 caller별 조건을 scattered helper로 늘리지 않고 required
  column set을 descriptor처럼 전달해 project list pruning과 child requirement를 결정한다. Neo4j의
  `VarLengthExpandPipe`도 var-expand mechanics와 output `writer`, min/max/end-node validation,
  `TraversalPredicates`를 분리하고, physical planning의 `SlotConfiguration`은 row layout을 immutable
  descriptor로 고정한다. AGE VLE도 같은 방향으로 length/end acceptance를 `VLETraversalAcceptance`
  descriptor로 묶어 traversal 모듈로 내렸다. 이로써 `paths-from`, `paths-between`, terminal-property
  traversal이 같은 path consume mechanics를 공유하면서 caller별 terminal requirement만 descriptor로
  바꿀 수 있다.
- 같은 판단을 terminal-property output에도 적용했다. 기존에는 일반 DFS found-result cache,
  terminal-property 전용 DFS의 char fast path, iterator의 final property emit이 서로 다른 위치에서
  `emit_terminal_property`/`reverse_output_path`를 직접 확인했다. `VLETerminalOutputPolicy`는 이 조건을
  direct-property 가능 여부와 char fast path 여부로 고정해 DFS와 result handoff가 같은 output contract를
  보게 한다.
- batch materialization의 첫 단계도 같은 방식으로 정리했다. terminal id/result/null array와 emit cursor를
  `VLETerminalPropertyBatchState`로 묶고 reset helper를 도입해, cached context cleanup과 batch emit
  lifecycle이 낱개 `VLE_local_context` 필드가 아니라 단일 batch state contract를 따르게 했다. 이어서
  batch fetch/label scan/tuple extraction helper가 `VLETerminalPropertyBatchFetch` descriptor를 받도록
  좁혔다. 이 descriptor는 graph oid, global graph context, property key만 담으므로 다음 모듈 분리에서
  traversal context 전체를 끌고 가지 않아도 된다. 실제 구현도 `age_vle_terminal_property_batch.c`로
  옮겨 `age_vle.c`의 traversal state와 batch property table scan을 분리했다.
- materializer cache API도 같은 기준으로 바꿨다. ORCA는 required/computed column 정보를 operator
  boundary에서 한 descriptor로 보고, Neo4j slotted runtime도 slot/value writer contract를 runtime
  helper 호출마다 다시 추론하지 않는다. AGE VLE materializer도 graph context, relation cache, build
  callback을 `VLEMaterializerHandoff`로 묶어 cache lookup에 넘기면 path/list/indexed typed
  materialization이 같은 handoff contract를 본다. 이 단계는 성능 개선 자체보다 다음 root/source
  hydrate와 output-requirement materialization 지연을 붙일 위치를 만드는 구조 변경이다.
- 그 handoff를 실제 indexed materializer helper까지 보존하려면 `VLE_path_container`가 단순 graphid
  array wrapper에 머물 수 없다. ORCA의 plan node private metadata나 Neo4j slot configuration처럼,
  AGE VLE binary container도 traversal root와 output requirement를 들고 가야 final materializer가
  SQL wrapper 이름에서 의미를 재추론하지 않는다. 그래서 fresh install 정책에 맞춰 container layout을
  확장하고, `offsetof(..., graphid_array_data)` 기준 size 계산으로 layout 변경을 명시화했다.
- callback boundary도 중요하다. cache API만 handoff를 받고 실제 builder가 다시
  `(ggctx, id, relation_cache)`를 받으면 root/output metadata가 materializer 내부에서 끊긴다.
  `VLEMaterializerBuildObject`를 handoff 입력으로 바꾸면 다음 terminal hydrate, root-local object cache,
  output-requirement별 property fetch 생략을 builder 내부 판단으로 연결할 수 있다.
- root만으로는 materializer hydrate 후보를 표현하기에 부족하다. path traversal root와 Cypher 출력
  terminal vertex가 다를 수 있고, terminal-only output은 graphid array가 단일 vertex만 가진다. 따라서
  container/handoff에 별도 candidate vertex를 두고, vertex object cache가 이 candidate를 먼저 seed하게
  했다. 이는 바로 property fetch 횟수를 줄이는 최종 단계라기보다, terminal scalar cache reuse와
  full-property hydrate 생략을 붙일 구체적인 cache seed 위치를 만든다.
- candidate seed는 무조건 full vertex object를 만드는 prefetch가 아니다. `get_vertex_entry_cached_properties()`
  probe를 추가해 global graph에 full properties가 이미 hydrate된 candidate만 materializer object cache로
  승격한다. terminal-property batch hydrate처럼 이미 tuple을 읽은 경로는 object materialization에서 재사용되고,
  그렇지 않은 path는 seed 단계에서 새 heap fetch를 만들지 않는다.
- indexed node property helper도 같은 cache contract를 보게 했다. 기존 `age_vle_node_property_at()`은
  full properties object를 만든 뒤 key lookup을 했지만, 이제 `get_vertex_entry_property_with_cache()`를
  통해 terminal-property direct output과 같은 vertex-entry scalar cache를 먼저 확인한다. 이는 key/value
  descriptor를 container metadata로 옮기기 전 단계의 shared scalar-cache boundary다.
- edge indexed property도 같은 방향으로 맞췄다. 기존 `age_vle_edge_property_at()`은 edge properties
  object를 매번 만든 뒤 key lookup을 수행했으므로, path length가 길거나 같은 edge property를 반복 참조하는
  workload에서 row width와 property payload 크기에 비례한 비용이 남았다. `VLEIndexedPropertyLookup`은
  node/edge entity kind, graphid, key descriptor를 함께 들고, edge entry는 vertex entry와 같은 scalar
  property cache와 relation-based fetch API를 제공한다. 작은 regression 데이터에서는 차이가 작아도,
  넓은 edge property payload와 repeated indexed lookup이 있는 큰 데이터셋에서는 full object materialization
  회피 효과가 커질 수 있으므로 이 contract를 우선 유지한다.
- fixed path indexed node/edge property regression도 hidden assertion에서 visible `EXPLAIN` 출력으로
  바꿨다. plan expected가 helper surface와 CustomScan descriptor를 직접 보여줘, 이후 key/value
  descriptor 확장이 plan evidence에 드러나는 형태를 유지한다.
- key는 `VLE_path_container` 생성 시점에는 존재하지 않고 helper 호출의 argument로 들어오므로, container
  layout에 무리하게 key를 저장하는 대신 runtime `VLEVertexPropertyLookup`을 둔다. 이 descriptor는
  container의 candidate vertex와 property key를 결합해 indexed node property와 terminal-from-path
  property가 같은 scalar-cache/relation-cache handoff를 공유하게 한다.
- terminal-property direct output도 같은 key descriptor 방향으로 맞췄다. `VLEPropertyKeyDescriptor`는
  agtype key와 1-byte char fast metadata를 분리해 담고, `VLETerminalPropertyLookup`은 graph context,
  relation cache, block prefetch set, prefetch budget을 함께 들고 간다. 이로써 DFS output policy,
  batch fetch, indexed-from-path helper가 runtime key를 서로 다른 `VLE_local_context` 필드에서 다시
  추론하지 않고 같은 key/cache handoff를 사용한다. 다음 CustomScan 확장은 planner-derived key slot을
  이 descriptor에 직접 채우는 방향이 맞다.
- 그 확장도 같은 원칙으로 진행했다. ORCA/Neo4j에서 slot descriptor가 runtime helper 호출마다 다시
  계산되지 않듯이, AGE `AGE VLE Stream` output descriptor가 terminal key length와 char-fast 여부를
  보관한다. executor는 이 metadata를 `AgeVLEInput`에 싣고 `build_local_vle_context()`는 known key slot에서
  key 길이 재계산 없이 `VLETerminalPropertyLookup`을 초기화한다. `EXPLAIN` expected에는 `len`과
  `char-fast`를 드러내 plan evidence가 runtime key/cache contract를 직접 보여주게 했다.
- ORCA `PexprPruneUnusedComputedCols`는 required column set에 없는 project element를 제거하고,
  `CPhysicalComputeScalar::PcrsRequired`는 child required columns를 scalar project list와 분리해 계산한다.
  AGE VLE도 같은 방향으로 `AgeVLEOutputRequirement`를 CustomScan output descriptor에 추가했다. 이제
  executor/VLE context는 `nargs`와 grammar id를 다시 해석해 path/terminal output을 추론하지 않고,
  planner가 정한 path/terminal-vertex/terminal-property requirement를 `AgeVLEInput`으로 직접 받는다.
  verbose plan도 `requirement=...`를 출력하므로 output contract가 plan evidence에 남는다.
- 같은 이유로 VLE compact materializer regression을 hidden assertion에서 visible plan evidence로 바꿨다.
  ORCA의 required/computed column 판단처럼 AGE도 helper 선택 자체가 구조 contract이므로, `age_vle_path_length`,
  `age_vle_list_slice_count`, tail-last id/endpoint helper, single edge/node materializer, endpoint join
  presence/absence를 expected plan에 직접 남기는 것이 맞다. 실패하면 assertion 문자열이 아니라 plan shape
  diff로 깨진 contract를 볼 수 있다.
- visible plan evidence가 드러낸 single materializer/direct helper 경계에서는 count와 negative index
  해석이 반복되어 있었다. `VLE_path_container` 자체가 edge/node count와 index normalization contract를
  제공하게 바꾸면, helper별로 `(graphid_array_size +/- 1) / 2`와 negative index 보정을 다시 쓰지 않는다.
  이는 큰 데이터셋에서 micro arithmetic 때문이 아니라 materializer/output requirement가 같은 container
  descriptor를 보게 만드는 기반이다.
- 이 변경은 작은 synthetic path에서는 차이가 작을 수 있지만, 큰 데이터셋에서 low fan-out root와
  high fan-out root가 섞인 VLE workload의 cache 기회를 보존한다. 다음 확장은 source entry에 terminal
  vertex/materializer hydrate 후보를 함께 붙이는 방향이다.
- materialization-heavy path에서는 단일 candidate seed만으로 부족하다. `path`/`nodes(p)` consumer는
  path 안의 모든 vertex object가 full properties를 요구할 수 있으므로, per-vertex lazy fetch를 그대로
  반복하면 같은 label relation을 여러 번 열고 id index/heap fetch를 반복한다. ORCA의 required column
  pruning처럼 consumer가 실제로 full vertex object를 요구하는 materializer boundary에서만 prefetch를
  붙이고, Neo4j slotted runtime처럼 runtime row slot을 먼저 정규화한 뒤 writer가 값을 채우는 방향이
  맞다. 그래서 `prefetch_vertex_entry_properties_by_ids()`는 path vertex id를 중복 제거하고 label relation
  단위 scan으로 `vertex_entry` full properties cache를 채운다.
- 이 prefetch는 작은 path에서 이득을 강제하지 않는다. materializer relation cache가 이미 있고 같은 label
  relation에 충분한 uncached vertex 후보가 모인 path/node-list 경로에서만 relation scan을 선택한다. typed
  single vertex helper처럼 relation cache 수명 관리가 없는 경로는 건드리지 않는다. 작은 regression
  데이터에서는 runtime 차이가 작아도, 큰 cardinality와 넓은 property payload가 있는 cold-cache path
  materialization에서는 per-vertex lazy hydrate를 label relation batch hydrate로 바꾸는 구조적 효과가 우선이다.
- prefetch policy는 런타임 helper 안에 숨기지 않는다. `AGE VLE Stream` output descriptor는 path
  materialization에서 vertex-prefetch 가능 여부와 `AGE_VERTEX_PROPERTY_PREFETCH_MIN_REL_CANDIDATES`를 함께
  보관하고, verbose `EXPLAIN`은 `path-container, vertex-prefetch=label-batch(min-rel-candidates=...)`를
  출력한다. hidden assertion이 아니라 expected plan 출력이 materializer hydrate contract를 보여주도록
  유지한다.
- fallback edge table scan 자체를 줄일 때 global graph를 endpoint 부분집합으로만 채우는 방식은 맞지
  않다. VLE depth가 2 이상이면 첫 vertex의 edge만으로 다음 vertex 확장을 할 수 없고, path/edge object
  materialization이 필요한 consumer에서는 edge metadata가 빠진 partial global context가 semantic을 깨뜨린다.
  따라서 terminal-only, label-constrained, property-constraint-free shape에서 global edge metadata load를
  생략하고, DFS가 vertex를 확장하는 순간 edge label의 `start_id`/`end_id` btree index를 endpoint id로
  probe하는 local source를 붙였다. PostgreSQL btree `ScanKey`는 heap attno가 아니라 index key position을
  요구하므로 endpoint index scan은 key position 1을 사용한다.
- local endpoint index scan은 edge metadata를 전역으로 만들지 않으므로 traversal이 만나는 vertex를
  skeletal `vertex_entry`로 보강해야 terminal-from-path property helper와 lazy property hydrate가 같은
  vertex cache contract를 본다. 이 때문에 vertex metadata를 lazy로 두는 global graph context에서는
  vertex hash freeze를 건너뛰고, `ensure_vertex_entry_skeleton()`이 label cache 기반 skeleton을 on demand로
  넣게 했다.
- plan-visible contract는 런타임 edge source 선택을 숨기지 않아야 한다. `AGE VLE Stream`은 planner가
  알 수 있는 graph/label/property/range/direction/output descriptor와 index 존재 여부로
  `VLE Edge Source`를 출력한다. 동적 값 때문에 source가 확정되지 않는 경우는 `dynamic`으로 남기고,
  terminal-only label/index shape가 확인된 경우만 `local-index-candidate`와 `state=dense-local`을
  출력한다. 이렇게 해야 hidden assertion 대신 normalized plan output이 VLE load contract를 검증한다.
- dense-local edge-state sizing은 고정 1024 entry보다 endpoint fan-out 통계가 더 맞다. PostgreSQL의
  `pg_class.reltuples`와 `pg_statistic.stadistinct`는 endpoint equality probe의 평균 fan-out을 추정할 수
  있고, bounded VLE는 upper depth로 reachable edge count를 relation tuple 수 안에서 cap할 수 있다. 통계가
  없거나 distinct가 1 미만/tuple 수 초과로 흔들리는 경우에는 clamp 후 fallback을 사용해 작은 graph에서
  과한 allocation을 피하고, 큰 fan-out graph에서는 hash growth와 flag-array repalloc을 줄인다. 초기
  allocation은 1M entry로 cap하고 이후 실제 traversal이 더 많은 edge를 만나면 기존 growth path로 확장한다.
- 이전 short-path optimization은 candidate provider가 현재 path stack을 선형 scan해 작은 path에서 dense
  flag lookup을 피하려는 구조였다. 하지만 now-local endpoint/adjacency source는 candidate 생성 시점에
  dense edge index를 만들거나 이미 갖고 있고, full metadata source도 dense index가 있다. 따라서 cycle
  prevention을 `VLE_EDGE_STATE_USED` flag로 통일하면 path stack scan과 special-case fast path가 사라지고,
  edge-isomorphism contract가 traversal consume path와 candidate provider에서 같은 state bit로 표현된다.
- frame stack, path edge-id stack, path edge-index stack, path vertex stack, dense edge-state flags를
  `VLETraversalState`로 묶었다. PostgreSQL executor 쪽에서는 여러 포인터를 계속 넘기는 API보다 state
  boundary가 낫다. consume/reset/free가 같은 state를 받으면 path depth cache와 used-edge flag clearing이
  한 전이로 보이고, 다음 단계에서 frame payload와 terminal output handoff를 더 줄일 수 있다.
- `VLE_local_context`의 root/bounds/source/index/scan 필드를 `VLEContextTraversalRootState`로 묶었다.
  `VLETraversalState`가 DFS stack mutation을 담당하고, root substate가 traversal root descriptor, range bound,
  source index/layout, adjacency scan/cache lifecycle을 담당하게 분리했다. ORCA의 required/computed column
  boundary처럼 output policy와 traversal root/source contract는 다른 변경 축이므로 같은 flat struct에 계속
  섞어두면 caller가 layout detail에 붙잡힌다. 이번 전환은 성능을 즉시 바꾸기보다 이후 DFS caller와
  CustomScan descriptor handoff가 context field를 직접 열지 않도록 만드는 구조 기반이다.
- root substate를 만든 뒤 곧바로 raw field 접근을 그대로 방치하면 구조 분리가 이름 변경에 머문다. 그래서
  cleanup, source-index refresh, traversal start reset, empty/zero/range acceptance, multi-start cursor,
  cached terminal vertex handoff를 `age_vle_context.c` API로 낮췄다. DFS/search/materializer caller는 root
  layout 조합 대신 context contract를 호출하고, adjacency candidate provider만 아직 fixed source layout과
  scan/cache 포인터를 직접 본다. 다음 단계는 이 남은 provider surface를 source cursor/cache descriptor로
  묶는 것이 맞다.
- cleanup API도 같은 원칙을 따른다. `age_vle.c`가 graph name, terminal key, relation cache, prefetch block,
  edge constraint cache, traversal state layout을 직접 해제하면 context substate 분리가 lifecycle에서 다시
  깨진다. runtime resource cleanup과 full context cleanup을 context API로 묶어 callback cleanup과 normal free가
  같은 ownership vocabulary를 쓰게 하는 것이 다음 source/output handoff 분리의 기반이다.
- adjacency candidate provider도 `VLEContextSourceCursor`로 옮겼다. cursor는 source vertex, direction,
  self-loop policy, fixed source kind/index, property constraint 여부를 담고, `age_adjacency` visible scan과
  payload cache entry는 context API가 제공한다. 이제 `age_vle.c` expansion path는 root layout을 직접 열지
  않고, source cursor가 정한 AGE adjacency/endpoint-btree source만 dispatch한다. 이는 다음에 frame push와
  edge-state/output carry를 descriptor로 묶어 `VLE_local_context` 전체 전달을 줄이기 위한 선행 구조다.
- candidate push path도 traversal/context API로 낮췄다. `VLETraversalCandidate`는 edge id/index, next vertex,
  optional frame vertex entry를 담고, traversal module이 `USED/MATCH_CHECKED/MATCHED` flag와 frame push를
  처리한다. output carry 여부는 context API가 결정한다. 이 구조는 in-memory adjacency, `age_adjacency`
  payload, endpoint-btree source가 같은 edge-state transition을 사용하게 하며, 다음 단계에서 property
  constraint edge lookup과 relation-cache handoff를 candidate validation descriptor로 분리하기 쉽게 만든다.
- `VLEEdgePropertyMatchContext`는 property constraint agtype/datum/hash, cached constraint pair, relation
  cache를 한 handoff로 묶는다. edge property matcher는 더 이상 `VLE_local_context` 전체를 받지 않고 이
  descriptor와 edge entry만 본다. source dispatch도 raw relation cache가 아니라 이 descriptor를 공유한다.
  이로써 provider가 context layout에 의존하지 않고, 다음 단계에서 edge-entry fetch/label/source validation을
  별도 candidate validation descriptor로 넓힐 수 있다.
- `VLEAgeAdjacencyCandidateValidation`은 `age_adjacency` payload callback에 남아 있던 edge TID fetch,
  label OID 확인, source/end vertex 방향 검증, local edge-state 여부, frame-entry carry 정책을 하나의
  descriptor로 묶었다. payload cache replay와 fresh scan 모두 같은 candidate init helper를 타므로, 다음에는
  packed adjacency path의 edge-entry lookup/property match도 같은 validation vocabulary로 합칠 수 있다.
- packed adjacency path도 `VLEPackedAdjacencyCandidateValidation`을 사용한다. property constraint가 없으면
  기존처럼 edge entry fetch 없이 candidate를 만들고, constraint가 있을 때만 `get_edge_entry()`를 수행해
  `VLEEdgePropertyMatchContext`로 검증한다. 이로써 packed adjacency, endpoint-btree, `age_adjacency` payload
  source가 모두 traversal candidate descriptor를 만들고 traversal module에 push를 위임하는 구조가 맞춰졌다.
- label-constrained packed adjacency list 선택도 context/source boundary로 낮췄다. provider가
  `edge_label_name_oid`와 `get_vertex_entry_adj_edges_*_for_label()` 조합을 직접 알면 direction, label,
  source layout이 다시 flat context 접근으로 새어 나온다. `VLEContextPackedAdjacencyLists`는 ORCA의
  derived property handoff처럼 caller가 선택 결과만 받게 하고, endpoint-btree source도 edge-label accessor를
  통해 table open 대상을 얻는다. 다음 구조 변경은 relation cache lifecycle과 edge-entry fetch source를 같은
  validation/source descriptor로 밀어 넣는 것이다.
- endpoint-btree source는 `VLEEndpointIndexSourceScan`과 `VLEEndpointIndexCandidateValidation`으로 분리했다.
  provider가 `table_open()`, `index_beginscan()`, `ExecFetchSlotHeapTuple()`, endpoint column extraction,
  local edge-index 생성, skeleton vertex handoff를 모두 직접 수행하면 source별 iterator contract가 생기지
  않는다. source scan helper가 relation/index/slot lifecycle과 tuple-to-candidate 변환을 맡고 provider는
  candidate push만 수행하므로, 다음에는 packed adjacency/`age_adjacency`/endpoint-btree를 하나의 source
  iterator 형태로 묶을 수 있다.
- `age_adjacency` access method도 callback-only visible payload scan에서 pull-style key scan API를 제공하게
  바꿨다. `age_adjacency_visible_payload_scan_begin_key()`가 directory/main/delta phase를 초기화하고,
  `age_adjacency_visible_payload_scan_next()`가 기존 main page cache와 visibility/property fetch helper를
  재사용해 payload를 한 건씩 반환한다. 기존 callback API는 pull API 위에 남겨 외부 debug/candidate 함수의
  surface를 유지한다. VLE는 `VLEAgeAdjacencySourceScan`으로 payload cache replay와 fresh scan을 같은
  descriptor에서 처리하므로, 다음 packed adjacency 전환 때 세 source 모두 `next_candidate` 형태로 합칠 수 있다.
- packed adjacency도 `VLECandidateSource` contract에 편입했다. source는 `next_candidate`를 반환하기 전에
  property constraint와 edge-state match bit를 처리하고, provider는 `push_candidates_from_source()`로
  traversal push만 수행한다. endpoint-btree, `age_adjacency`, packed adjacency가 같은 push loop를 타므로,
  이제 남은 구조 병목은 source contract가 아직 `age_vle.c` 안에 모여 있다는 점이다. 다음 전환은 source
  descriptors와 iterator helpers를 별도 module로 분리해 VLE executor 본문에서 expansion source 세부 구현을
  떼어내는 것이다.
- `age_vle_candidate_source.c`로 candidate source contract를 분리했다. 새 module은 packed adjacency,
  `age_adjacency`, endpoint-btree source descriptor, relation/index/payload scan lifecycle, candidate
  validation, match-state marking을 맡고, `age_vle.c`는 `age_vle_push_candidates_from_context_source()`와
  `age_vle_push_candidates_from_packed_adjacency()`만 호출한다. property match semantic은 여전히 VLE 본문
  쪽 API로 남겨 source module이 Cypher property constraint 해석을 복제하지 않게 했다. 다음 구조 변경은
  source selection/load policy도 source module로 낮춰 provider의 out/in/self suppression decision을 줄이는 것이다.
- context build lifecycle도 `age_vle_apply.c`로 이동했다. cache storage 자체는 `age_vle.c`의 global list에
  남기되, `VLETraversalContextCacheOps` callback으로만 노출한다. 이로써 새 context 생성, cached refresh,
  graph context load, setup apply, activation이 같은 module boundary에서 처리되고, `age_vle.c`는 DFS stack
  load와 cache 저장소 같은 runtime side effect만 제공한다. ORCA의 physical operator가 child expression과
  required state를 명시적 handoff로 받는 구조처럼, VLE CustomScan runner도 broad function body 대신 typed
  setup/apply descriptor를 소비하는 방향으로 좁아진다.
- source selection/load policy도 source module로 낮췄다. missing vertex fallback은 source module이
  local-edge-state/label/no-property 조건을 확인하고 out/in source를 dispatch한다. normal vertex expansion도
  out source, in source, packed fallback suppression을 source module에서 계산한다. `age_vle.c`는 이제
  `age_vle_push_candidates_from_missing_vertex_source()` 또는 `age_vle_push_candidates_from_vertex_entry()`만
  호출한다.
- `VLEEdgePropertyMatchContext` lifecycle은 context API와 source module 사이의 handoff로 낮췄다. expansion API는
  raw constraint field를 직접 읽지 않고 `age_vle_context_init_edge_property_match()`로 constraint metadata,
  cached pair descriptor, relation cache, semantic matcher를 받는다. `age_vle.c`는 edge property match
  descriptor를 만들지 않고, property constraint semantic helper만 제공한다. 이어서 candidate source의
  traversal mutation도 context API로
  낮췄다. source module이 `VLETraversalState *`를 저장하거나 `&vlelctx->traversal`을 넘기면 local edge-state
  layout, match bit policy, push policy가 source provider contract에 새어 나온다. `age_vle_context_*`
  handoff는 runtime stats/explain snapshot 소유권과 traversal state 소유권을 같은 context boundary에 묶고,
  다음 단계에서 validation/source descriptor가 graph context raw field를 덜 열도록 만들 기반을 준다.
- candidate validation의 graph/source handoff는 `VLECandidateGraphAccess`와 `VLECandidateSourceIdentity`로
  나눴다. endpoint-btree와 `age_adjacency`는 둘 다 source vertex, direction, self-loop 검증을 수행하지만,
  이를 source별 validation field로 직접 반복하면 tuple source와 payload source가 같은 traversal identity를
  공유한다는 사실이 코드에 드러나지 않는다. 공통 descriptor는 ORCA의 operator input descriptor처럼
  source-specific scan state와 graph lookup/carry policy를 분리하고, 다음 relation/index/cache lifecycle
  descriptor 전환의 기준점을 만든다.
- source scan/cache lifecycle도 context descriptor로 낮췄다. `VLEContextAgeAdjacencyPayloadSource`는 cache
  replay, fresh payload scan, fan-out 2 이상 cache seed, fan-out 1 discard 정책과 runtime counter update를
  함께 보관한다. opaque `VLEContextEndpointIndexSource`는 relation/index/slot/index scan lifecycle과 tuple
  field extraction을 맡고, source module에는 endpoint tuple만 반환한다. 이 경계는 PostgreSQL executor의 scan
  state처럼 resource ownership을 한 module에 두고, candidate source가 validation과 traversal handoff에
  집중하도록 만든다.
- packed fallback source plan도 `VLEContextPackedAdjacencySource`로 낮췄다. 이전에는 source module이 context
  API에서 받은 adjacency list를 직접 `NULL`로 지우며 suppression과 empty-source 판단을 계산했기 때문에,
  label/direction list selection ownership과 fallback suppression ownership이 갈라져 있었다. context descriptor가
  list selection, suppression, has-source 판단을 같이 보관하면 runtime counter와 source iterator가 같은 source
  lifecycle vocabulary를 사용한다.
- normal vertex expansion orchestration은 `VLEContextExpansionSourceRun`으로 묶었다. source module이
  `used_out_source`, `used_in_source`, `used_index_source` local boolean을 직접 조합하면 incoming self-loop skip과
  packed fallback suppression이 call order에 숨어 있다. run descriptor는 outgoing/incoming source 결과를 명시적으로
  기록하고, packed source descriptor가 그 결과에서 suppression을 계산하게 한다. 이 구조는 이후 missing vertex
  fallback을 같은 source policy vocabulary로 흡수할 수 있는 중간 단계다.
- missing vertex fallback도 `VLEContextExpansionSourceRun`으로 흡수했다. fallback eligibility는 local edge-state,
  label constraint, property constraint 여부의 결합이고, incoming self-loop skip은 bidirectional traversal 여부에
  따라 달라진다. 이를 source module의 `if`와 `edge_direction` 비교로 두면 normal expansion과 다른 policy surface가
  남는다. context run descriptor가 eligibility와 cursor policy를 계산하면 missing/normal source dispatch는 같은
  helper를 타고, source module은 run 결과에 따라 missing-hit counter만 기록한다.
- candidate property match lifecycle은 `VLECandidateMatchResult`로 묶었다. 이전 구조는 packed adjacency,
  `age_adjacency`, endpoint-btree path가 constraint 여부 확인, edge fetch handoff, match bit mark를 각자 반복했고,
  `edge_entry **edge_for_match` raw out-param이 candidate construction 밖으로 새어 나왔다. result descriptor는
  source-specific candidate construction과 traversal match marking 사이의 contract를 명시하고, 다음 단계에서
  context/traversal handoff가 validation result를 직접 받도록 확장할 수 있는 발판이다.
- ORCA `CPhysicalComputeScalar::PcrsRequired`와 `PexprPruneUnusedComputedColsRecursive`는 required column set을
  operator 경계에 전달하고, project element 자체가 pruning policy를 직접 흩어 갖지 않게 한다. 같은 관점에서
  VLE source module도 match bit policy를 직접 해석하지 않고 `VLECandidateMatchResult`를 context/traversal
  handoff로 넘기게 했다. `age_vle_context_apply_candidate_match_result()`가 needs-check, semantic property match,
  match bit mark를 한 경계에서 처리하므로 source-specific path는 candidate construction과 match용 edge fetch
  contract에 집중한다.
- DFS consume payload도 `VLETraversalStep`으로 묶었다. 기존 out-param 세트는 `next_vertex_id`,
  `next_vertex_entry`, `path_length`가 terminal acceptance, upper-bound 판단, expansion, terminal output cache에
  반복 전달되어 frame payload와 output handoff 사이의 실제 contract가 보이지 않았다. step descriptor는
  traversal이 만든 다음 vertex state를 한 값으로 유지하고, context consume API가 vertex-entry cache update까지
  같이 처리하게 해 DFS caller가 traversal layout이나 cache side effect를 직접 알지 않게 한다.
- source/cache lifecycle도 opaque source family로 더 좁혔다. `VLEContextAgeAdjacencyPayloadSource`는 payload
  scan, cache entry, replay cursor, pending payload seed policy를 public header에 노출하지 않고 context module의
  begin/next/end API 뒤에 둔다. packed adjacency도 out/in/self list와 suppression 결과, iterator index를
  candidate source에 넘기지 않고 context-owned packed source가 next `GraphEdgeAdjEntry`만 반환한다. 이 경계는
  PostgreSQL scan state처럼 resource ownership과 source iteration policy를 한 module에 두고, candidate source가
  tuple/payload를 candidate로 바꾸는 역할만 맡게 한다.
- iterator output handoff는 `VLEIteratorOutputTarget`으로 시작했다. 이전에는 terminal scalar property,
  terminal full properties, batch property emit, container materialization이 각각 `Datum *result`와 `bool *is_null`
  pair를 직접 썼다. output target descriptor와 setter를 쓰면 materialization kind 선택과 final output write가
  같은 contract를 공유하고, path/container builder dispatch를 별도 output module로 옮길 때 `age_vle.c`의
  terminal builder static dependency를 단계적으로 줄일 수 있다.
- final materialization dispatch도 iterator materialization module로 이동했다. ORCA의 physical operator가 child
  required/provided column contract를 따로 계산하듯, VLE iterator는 materialization kind 선택과 result target
  write를 `age_vle_iterator_emit_result()`에 맡기고 `age_vle.c`는 terminal scalar/full-properties/container builder
  callback만 제공한다. path/container dispatch가 DFS/search loop 밖으로 나가면서 다음 단계의 terminal builder
  file split은 callback implementation 축소 문제로 좁혀졌다.
- source runtime stats도 context API ownership으로 낮췄다. reset, snapshot, source scan/candidate/push,
  missing vertex, packed suppression, `age_adjacency` payload replay/scan/cache seed counter는 context module이
  갱신한다. source module은 counter layout을 직접 알지 않고 `VLEContextSourceStatsKind`와 record helper를
  사용하므로, 다음 traversal candidate handoff 분리 때 runtime explain snapshot contract를 보존할 수 있다.
- PostgreSQL CustomScan의 explain hook은 `EXPLAIN ANALYZE`에서 executor state를 볼 수 있지만,
  `EndCustomScan` 이후 iterator-local context는 이미 정리될 수 있다. 따라서 VLE source runtime descriptor는
  source module의 local context counter, iterator 종료 전 snapshot, CustomScan state의 마지막 snapshot을
  분리해야 안정적이다. 이 구조로 `VLE Source Runtime`을 출력하면 assertion block 없이도 endpoint-btree,
  packed fallback, `age_adjacency` payload replay가 실제로 얼마나 쓰였는지 plan output에서 비교할 수 있다.
- `VLE Source Runtime`이 endpoint-btree candidate 1건 뒤 packed source scan 1회/candidate 0건을 보여줬다.
  이는 executor hot path에서 불필요한 iterator setup이 남는 신호라, packed fallback을 source plan으로
  승격했다. plan은 suppression 이후 실제 list size를 확인하고 실행 가능한 source가 없으면 iterator를 만들지
  않는다. 이 판단은 작은 fixture에서의 runtime 감소보다 큰 fan-out workload의 scan 노이즈와 call count를
  줄이는 방향을 우선한 것이다.
- payload cache seed도 같은 기준으로 조정했다. 기존 구현은 fan-out 1 source에서도 cache array를 만들고
  `finish_age_adjacency_source_scan()`에서 discard했기 때문에 큰 traversal에서 저차수 vertex가 많을수록 allocation
  noise가 생긴다. 첫 payload를 scan-local pending slot에 두고 두 번째 payload가 확인될 때 cache entry를 seed하면
  fan-out 2 이상 source만 replay 대상이 된다. `cypher_vle` visible `EXPLAIN ANALYZE` fixture는 age_adjacency
  source 3회, payload scan 6회, cache seed 3회를 출력해 이 contract를 expected에 남긴다.
- endpoint-btree와 `age_adjacency`가 모두 있는 VLE source 선택은 replay depth를 기준으로 나눈다. 단일 step
  `*1..1`은 같은 source vertex를 재방문하지 않으므로 `age_adjacency` payload cache/replay의 장점이 작고,
  endpoint-btree local source가 dense edge-state와 직접 결합한다. 반대로 `*1..2` 이상은 같은 label/source key의
  payload replay 가능성이 생기므로 `age_adjacency` source를 우선 유지한다. 이 판단은 guard 추가가 아니라
  source descriptor 선택 contract 변경이며, `EXPLAIN (ANALYZE, VERBOSE)`의 `VLE Edge Source`와
  `VLE Source Runtime` 출력으로 확인한다.
- direct runtime selector에서 relcache/statistics fanout을 다시 읽어 source 선택을 바꾸는 방식은 traversal
  lifecycle과 맞지 않아 깊은 VLE regression에서 crash를 만들 수 있다. ORCA의 required/provided property
  handoff처럼 planner가 계산한 source 후보와 cost evidence를 CustomScan descriptor로 넘기고, executor는 그
  descriptor를 실행/출력 contract로 읽는 쪽이 더 맞다. 이번 단계에서는 `AGE VLE Stream` descriptor에 edge label
  `reltuples`, `start_id`/`end_id` fanout evidence를 넣고 `EXPLAIN`에 노출했다. 이 값은 아직 runtime source
  선택을 바꾸지 않지만, 다음 단계의 endpoint-btree/`age_adjacency` cost adjustment가 읽을 안정적인 marker다.
- Neo4j의 `VarExpand`/`BFSPruningVarExpand` 흐름은 planner가 expansion mode와 pruning shape를 명시하고 slotted
  runtime이 slot descriptor를 읽어 실행한다. AGE VLE도 `age_vle` SRF wrapper를 보강하는 대신 `AGE VLE Stream`
  CustomScan descriptor가 source layout, slot/output requirement, source cost evidence를 한 곳에 담도록 밀어야
  한다. 작은 fixture에서 바로 row count 개선이 보이지 않아도 큰 fan-out workload에서는 source 선택/scan noise
  제거의 근거가 된다.
- `reltuples`와 endpoint fanout은 planner explain 출력, local edge-state capacity, `age_adjacency` path costing에서
  같은 의미로 쓰이지만 이전에는 caller마다 직접 조합했다. `VLESourceFanoutEvidence`로 묶으면 ORCA의 physical
  property처럼 같은 computed evidence를 여러 decision point가 읽게 되고, 이후 source selection이 바뀌어도
  statistics lookup과 direction/index filtering policy가 흩어지지 않는다.
- runtime source layout input 자체에 fanout field를 직접 추가하면 deep property VLE에서 iterator/root lifecycle과
  충돌할 수 있다. ORCA식으로 computed evidence를 plan descriptor에 먼저 싣고, executor contract를 그 descriptor를
  읽는 방향으로 바꿔야 한다. 그래서 `AGE VLE Stream`은 `VLE Edge Source`에 cost policy recommendation을 출력하고,
  directed source enum recommendation도 CustomScan private descriptor에 보관한다. 실제 source 전환은 runtime
  selector input layout이 아니라 이 descriptor handoff를 통해 진행할 다음 단계로 남긴다.
- planner-computed policy를 executor에 적용할 때는 ORCA의 required/provided property처럼 plan descriptor와 runtime
  layout descriptor가 같은 effective source vocabulary를 공유해야 한다. `AGE VLE Stream`의 directed policy enum을
  `AgeVLEInput` -> `VLETraversalContextApply` -> `VLETraversalSourceLayoutInput`으로 전달하고, selector는 후보 index가
  실제로 있는 방향에서만 override한다. 이렇게 하면 runtime selector input이 statistics를 다시 계산하지 않으면서도
  plan의 `fixed-source`와 `VLE Source Runtime` counter가 같은 source 선택을 설명한다.
- cached context refresh도 같은 source property contract를 유지해야 한다. initial setup만 descriptor policy를 읽고
  refresh가 기본 layout으로 재계산하면 같은 plan node의 반복 실행에서 source 선택이 달라질 수 있다. 그래서
  `VLE_local_context`가 source policy descriptor를 보관하고 refresh root descriptor가 이를 다시 읽는다. runtime
  feedback 역시 마지막 iterator만 보는 값이 아니라 CustomScan execution 전체 누적값이어야 다음 cost 보정의
  근거가 된다.
- source policy는 fixed fanout threshold 문자열보다 work model descriptor로 설명해야 한다. endpoint-btree는
  start/end btree probe 수가 fanout과 depth에 따라 커지고, `age_adjacency`는 큰 fan-out에서 payload source가
  더 안정적이다. 그래서 policy text는 `endpoint-work=current/limit`을 출력하고, planner/executor가 같은
  decision을 읽는다. 작은 fixture의 절대 runtime보다 큰 fan-out에서 선택이 바뀌는 구조를 우선 검증한다.
- source 선택 사유도 threshold 밖에 숨기지 않는다. `reason=out:.../in:...`은 endpoint-btree가
  누적 work limit 안에 있어 유지됐는지(`endpoint-work`), `age_adjacency`로 전환된 이유가 work 초과인지
  (`work-exceeds-limit`), fanout evidence가 없어 adjacency source를 보존하는지(`unknown-fanout`),
  또는 source availability fallback인지(`endpoint-only`, `adjacency-only`, `no-source`, `layout`)를
  regression-visible contract로 남긴다.
- endpoint-btree source work는 최종 depth의 leaf 수만 보면 안 된다. `*1..2`는 depth 1 probe와 depth 2 probe를
  모두 수행하므로 fanout 2의 work는 `2^2 = 4`가 아니라 `2 + 4 = 6`이다. ORCA가 child required property를
  한 단계 결과가 아니라 operator boundary 전체의 요구로 계산하는 것처럼, VLE source policy도 bounded traversal
  전체 누적 work를 descriptor에 담아야 한다. 그래서 `VLEStreamSourceCostDecision`은 선택 enum과 explain text를
  같이 만들고, `AGE VLE Stream`은 `endpoint-work=sum(...)`을 출력한다. Neo4j의 pruning var expand도 expansion
  operator 자체가 pruning/expand shape를 가진다는 점에서, AGE도 SRF 내부 guard보다 CustomScan source policy
  descriptor가 누적 traversal work를 설명하는 쪽이 맞다.
- depth 2 cap은 큰 fan-out workload 우선순위와 충돌한다. finite upper에서 overflow-safe 누적 work를 이미 계산할
  수 있다면, depth가 커졌다는 이유로 layout fallback으로 되돌아가면 planner-computed descriptor가 실제 source
  choice를 설명하지 못한다. 따라서 property constraint가 없는 bounded VLE는 모든 finite depth에서 costed policy를
  적용하고, fanout 3/depth 3 fixture로 `39/14` work comparison을 regression에 남긴다.
- fanout estimate에는 값과 신뢰도를 분리해 둬야 한다. PostgreSQL `pg_class.reltuples`와 `pg_statistic.stadistinct`
  lookup은 관계 tuple 수가 0인지, 통계가 아직 없는지, endpoint distinct estimate가 실제로 가능한지를 서로 다른
  상태로 만든다. ORCA의 physical property가 required/provided state를 값 하나로 뭉개지 않듯, AGE VLE source
  policy도 `fanout=0`과 `fanout unknown`을 분리해야 endpoint-btree를 잘못 신뢰하지 않는다.
- endpoint-btree와 `age_adjacency`의 누적 work가 같은 tie일 때, 단일 step은 endpoint-btree를 유지하지만
  multi-step VLE는 `age_adjacency`를 선택한다. Neo4j의 variable expand가 깊이와 pruning shape를 operator property로
  다루는 것처럼, AGE도 `*1..2` 이상에서는 payload replay 가능성과 반복 source scan density를 source policy에
  반영해야 한다. `reason=work-tie`는 이 경계 결정을 regression-visible하게 남긴다.
- runtime counter도 threshold 보정 후보를 사람이 숫자만 보고 해석하게 두면 안 된다. `endpoint-direct`는 단일
  source probe가 직접 후보를 만든다는 뜻이고, `adjacency-cache-seeded`는 같은 source key에서 payload cache/replay
  가능성이 생긴다는 뜻이다. 이 class/recommendation을 `VLE Source Runtime`에 노출하면 benchmark harness와 focused
  regression이 같은 vocabulary로 source policy 보정 근거를 기록한다.
- planner policy도 runtime feedback과 같은 vocabulary를 가져야 한다. `work-tie`와 `work-exceeds-limit`은
  `age_adjacency`를 선택한다는 점에서는 같지만, 전자는 threshold 경계이고 후자는 명확한 초과다. 따라서
  `VLE Edge Source`에는 direction별 reason과 별도로 plan 전체의 `class/recommendation`을 둬서 benchmark가
  planner decision과 runtime feedback을 같은 summary table에서 비교하게 한다.
- ORCA의 `CReqdColsRequest`는 required column set뿐 아니라 child index와 scalar child index를 request key에
  포함한다. VLE source policy도 source enum 하나만으로는 부족하고, 어떤 consumer requirement와 traversal
  boundary에 대한 decision인지 함께 보존해야 한다. Neo4j의 `PruningVarExpand`/`BFSPruningVarExpand`도
  distinct/min-depth 같은 horizon requirement를 보고 expansion operator shape를 바꾸므로, AGE의
  `VLEStreamSourceCostInput`에도 `AgeVLEOutputRequirement`를 포함하고 `EXPLAIN` policy text에
  `consumer=... consumer-class=...`를 남긴다.
- undirected VLE는 outgoing/incoming fanout을 별도 endpoint scan으로만 보면 실제 frontier work를 낮게 본다.
  `CYPHER_REL_DIR_NONE`에서는 start/end fanout을 더한 combined fanout으로 bounded work를 다시 계산하고,
  combined work가 endpoint-btree budget을 넘거나 multi-step tie에 걸리면 `age_adjacency`를 우선한다.
  다만 path materialization은 edge object를 만들어야 하므로 local edge-state source만으로는 아직 충분하지 않다.
  edge object materializer가 global edge metadata를 요구하는 shape는 terminal/local-safe source policy와
  분리해서 다뤄야 한다.
- cached VLE context refresh는 start/end vertex만 바꾸는 갱신이 아니다. planner가 새 CustomScan descriptor에
  source policy를 넣었다면 refresh root descriptor도 같은 policy를 읽어야 한다. 그렇지 않으면 plan의
  `VLE Edge Source`와 `EXPLAIN ANALYZE` runtime counter가 서로 다른 source decision을 설명하게 된다.
- cached-property projection도 같은 descriptor 안에서 중복 key path를 식별해야 한다. ORCA식 computed column
  reuse 관점에서 같은 `(container, key path)`를 여러 physical type으로 최종 변환할 때 heap properties object를
  다시 탐색할 필요가 없다. executor slot descriptor가 이전 slot을 source로 참조하면 raw property lookup과 final
  type conversion boundary가 분리되고, `Cached Property Slots` explain이 reuse 관계를 직접 보여준다.
- property index handoff와 cached-property slot은 같은 `(container, key path, semantic value type, physical
  result type, final/index expression)` signature를 공유해야 한다. slot이 field들을 복사해 들고만 있으면
  projection delay, aggregate lower target, index canonicalization이 같은 property access를 서로 다른 비교식으로
  판단하게 된다. 따라서 slot descriptor에 `CypherPropertyHandoffDescriptor`를 함께 보관하고, caller는 helper로
  key source/physical signature를 비교한다.
- VLE path materialization이 local source를 쓰지 못했던 병목은 traversal source 자체가 아니라 object
  materializer의 metadata contract였다. ORCA식으로 보면 source selection의 required property는 "edge traversal에
  필요한 endpoint id stream"이고, path output의 required property는 "최종 object materialization 가능성"이다. 두
  요구를 global graph full metadata load 하나로 묶으면 큰 label-constrained workload에서 local index source를 막게
  된다. Neo4j slotted runtime도 expand operator의 slot stream과 final projection object creation을 분리하므로,
  AGE VLE도 `VLEMaterializerHandoff` 뒤에서 label-row fallback을 제공하고 planner source policy는 path/cached
  grammar consumer까지 local-index 후보를 열어 두는 쪽이 맞다.
- ORCA `CReqdPropPlan`은 required column, order, distribution, rewindability를 하나의 plan property request로
  계산하고 child physical operator가 그 request를 해석한다. AGE VLE source policy도 source enum 하나가 아니라
  consumer requirement를 포함한 request로 봐야 한다. 그래서 terminal-only consumer와 path-materialized consumer의
  endpoint-btree fanout budget을 분리했다. Neo4j `pruningVarExpander`도 `VarExpand`를 그대로 두지 않고 distinct,
  max length, emitted depth 같은 downstream requirement를 보고 `PruningVarExpand`/`BFSPruningVarExpand`로 바꾼다.
  AGE에서도 path output은 final object materialization과 label-row fallback 비용을 포함하므로 terminal scalar
  output보다 더 이른 `age_adjacency` 전환 기준을 갖는 것이 맞다.
- planner policy와 runtime feedback을 SQL benchmark summary에서만 join하면 cached refresh나 executor handoff가
  어긋난 순간을 원본 plan output에서 바로 볼 수 없다. ORCA의 required/provided property 검증처럼 AGE VLE도 plan
  descriptor가 요구한 source와 runtime dominant source의 관계를 operator explain에 직접 싣는 편이 맞다. 그래서
  `VLE Source Runtime`은 `planned=out:.../in:... source-match=...`를 출력하고, benchmark는 이 값을 그대로 추출한다.
- 같은 이유로 planner feedback class도 benchmark SQL에서만 재조합하면 부족하다. Neo4j VarExpand/PruningVarExpand
  테스트가 plan operator 이름과 depth/output mode를 함께 확인하듯, AGE VLE도 operator explain line 안에서
  runtime class와 planned class를 함께 보여야 한다. `VLE Source Runtime`은 planner policy text의 stable
  `class`/`recommendation` token을 `planned-class`/`planned-recommendation`으로 되싣고 `class-match`를 출력한다.
  이 값이 false이면 source family는 맞아도 fanout/depth threshold나 cache seed class가 runtime evidence와 어긋난
  것이다.
- ORCA의 required property object처럼 AGE VLE source policy도 fanout budget 숫자 하나를 인자로 넘기는 방식이면
  consumer class, finite depth, cache seed 가능 여부가 다시 흩어진다. `VLESourcePolicyProfile`은 output requirement,
  consumer class, fanout budget, cost eligibility, cache seed eligibility를 묶어 policy decision과 explain formatter가
  같은 request를 읽게 한다. 특히 `cache-seed=eligible`은 단순 depth>1이 아니라 finite upper bound와
  `age_adjacency` source availability가 함께 있을 때만 true로 둬야 한다.
- cache seed가 가능한 multi-step source에서는 endpoint-btree work가 기존 limit 안에 있다는 이유만으로 endpoint를
  유지하면 payload cache/replay 가능성을 계속 놓칠 수 있다. ORCA의 enforcer 선택처럼 lower-cost direct path와
  downstream property 제공 사이에 headroom을 둬야 한다. AGE VLE는 generic cache seed에는
  `endpoint-headroom=0.75`를 유지하되, planned empty lifecycle이 있는 source는 repeated empty completion도
  제공 property로 보므로 `endpoint-headroom=0.50`을 policy에 싣는다. work가 이 limit를 넘으면
  `empty-lifecycle-headroom` reason으로 `age_adjacency`를 선택한다. 이는 작은 synthetic 차이가 작아도 큰 fan-out
  dataset에서 cache seed/replay와 empty source-run completion을 우선하기 위한 공격적인 threshold다.
- terminal output도 하나의 required property로 뭉개면 안 된다. `terminal-property`는 scalar field만 만들 수 있지만
  `terminal-vertex`와 `terminal-properties`는 vertex/properties materialization을 요구한다. ORCA식 required column
  관점에서는 후자가 더 넓은 final object requirement이므로, AGE VLE source policy는 이를 `terminal-object` class로
  분리하고 path materialization과 같은 낮은 endpoint fanout budget을 적용한다.
- planner가 만든 policy feedback을 runtime explain에서 다시 쓰기 위해 `VLE Edge Source` 문자열을 파싱하는 것은
  ORCA식 property handoff와 맞지 않는다. ORCA는 required/provided property를 explain text에서 역파싱하지 않고
  operator descriptor로 전달한다. AGE VLE도 `policy_class`, `policy_recommendation`, `cache_seed_eligible`,
  `endpoint_headroom_percent`를 `CustomScan` private descriptor에 싣고, explain text는 사람이 보는 surface로만 둔다.
  이렇게 해야 다음 threshold 보정에서 문자열 token 변경과 executor feedback contract가 서로 묶이지 않는다.
- ORCA `CReqdPropPlan`은 child request마다 필요한 property만 강제하고, Neo4j `BFSPruningVarExpand`도 expand 방향과
  depth requirement를 operator shape에 반영한다. AGE VLE source policy도 directed VLE에서 반대 방향 fanout을 같은
  policy evidence로 취급하면 benchmark가 실제 runtime source와 무관한 threshold를 비교하게 된다. 따라서
  `VLESourcePolicyProfile`은 `active=out|in|both`를 포함하고, inactive side는 `none/inactive-direction`으로 고정한다.
  undirected VLE에서만 combined outgoing/incoming work를 보정 대상으로 둔다.
- ORCA `CReqdPropPlan`과 `CPhysicalIndexScan`은 required property와 index output column을 debug string에서
  재구성하지 않고 operator field로 보관한다. Neo4j `SlottedIndexedProperty`도 indexed value가 필요한 경우에만 cached
  property slot offset을 붙인다. AGE VLE source policy의 consumer class, active direction, fanout budget도 같은
  방식으로 `AgeVLEStreamEdgeSource` descriptor field가 되어야 한다. `policy=` 문자열에 이 profile을 계속 섞어두면
  benchmark SQL과 executor feedback이 사람이 보는 explain text token에 묶인다.
- benchmark가 ORCA식 property request 검증 역할을 하려면 plan text substring이 아니라 active source descriptor끼리
  비교해야 한다. `out=none/in=age-adjacency` 같은 left VLE에서 단순 `LIKE`는 inactive side와 active side를 구분하지
  못한다. benchmark summary는 planner policy와 runtime planned source를 각각 active side로 분해해
  `active_planner_source`, `active_planned_source`, `source_match`를 계산한다.
- local-source terminal vertex output에서 `age_vle_terminal_vertex()`가 global graph vertex metadata만 읽으면 ORCA식
  required property 분리가 깨진다. traversal source의 required property는 endpoint id stream이고 terminal object
  materialization은 label-row fallback으로 해결할 수 있다. 따라서 `build_vle_vertex_value()` 자체가 global metadata
  miss 시 graphid label id로 label relation row를 찾아 vertex value를 만든다.
- ORCA식 required/provided property를 AGE VLE에 적용하려면 source selection과 final materialization 가능성이 plan에서
  따로 보이고, 동시에 같은 descriptor contract로 연결되어야 한다. `VLE Materialization` explain이
  `object-source=global-metadata|label-row-fallback` 또는 `vertex-source=...`를 출력하면 local edge-state source가
  endpoint id stream만 제공해도 final object/properties materialization은 label relation row fallback으로 충족된다는
  점이 raw plan evidence로 남는다. 이는 guard/assertion보다 planner/executor property handoff를 검증하기 좋은 surface다.
- runtime payload seed counter는 실제 cache activity지만, planner profile에서 `cache_seed=ineligible`인 depth 1
  shape까지 `adjacency-cache-seeded` class로 올리면 feedback vocabulary가 threshold 보정 근거를 흐린다. runtime
  feedback은 `AgeVLEStreamEdgeSource.cache_seed_eligible` descriptor를 함께 읽어 class를 정한다.
- Neo4j slotted runtime이 indexed property value 필요 여부를 slot metadata로 나누듯, AGE VLE source policy도
  fanout work가 같은 tie에서 consumer materialization requirement를 분리해야 한다. `terminal-property`는 scalar value
  output이므로 endpoint-btree tie를 유지하고, `path` 또는 `terminal-vertex`는 object/path materialization이 이어지므로
  tie를 `age_adjacency`로 밀어 source/layout decision과 final materialization requirement를 같은 policy profile에
  반영한다.
- ORCA의 property enforcement는 단순 pass/fail이 아니라 어떤 enforcer가 필요한지 optimizer request에 남긴다.
  AGE VLE runtime feedback도 `class_match=true`에서 멈추면 다음 최적화 압력을 잃는다. `pressure/action`을
  `VLE Source Runtime`에 추가하면 source family와 class가 맞더라도 low density, cache seed miss, endpoint fanout 같은
  다음 조정 방향을 plan evidence로 유지할 수 있다.
- fan-out smoke에서 `age_adjacency`가 dominant source이고 planner/runtime class도 맞는데
  `age_adjacency` scan 9회 중 8회가 empty였다. ORCA식 property request로 보면 이것은 source family 선택 실패가
  아니라 child operator setup/probe lifecycle의 불필요한 enforcer 비용에 가깝다. Neo4j var-expand도 방향, depth,
  distinct requirement를 operator shape에 반영해 불필요한 expand를 줄이므로, AGE VLE의 다음 후보는 broad
  threshold rollback이 아니라 root/source descriptor가 empty 가능성이 높은 source cursor 생성을 억제하거나 cached
  source layout에서 empty probe를 건너뛰는 구조다. 작은 synthetic dataset에서 runtime 차이가 작아도 큰 fan-out
  dataset에서는 empty source setup이 root 수만큼 반복되므로 이 압력을 우선한다.
- 첫 suppression은 access method의 `begin_key` contract를 VLE source lifecycle로 끌어올리는 방식이 맞다.
  `age_adjacency_visible_payload_scan_begin_key()`가 false이면 main run도 delta scan도 없다는 뜻이므로, 이것을
  candidate source failure로 돌리면 ORCA식 required property가 깨져 packed/global fallback이 다시 열린다. 따라서
  "chosen source가 empty로 완료됨"을 별도 상태로 보관하고 source-run은 used로 기록해야 한다. 이 구조는 guard가
  아니라 child source가 제공한 physical property를 executor source contract에 반영하는 것이다.
- suppressed empty source feedback도 전체 counter 하나로는 부족하다. ORCA required property는 child별 request와
  provided property를 분리하고, Neo4j var-expand도 expand 방향을 operator shape로 보관한다. AGE VLE도 right/left
  source policy가 서로 다른 directed source를 가질 수 있으므로, suppression counter는 out/in 방향별로 보관해야
  planner가 inactive direction과 active direction의 pressure를 혼동하지 않는다.
- suppressed source가 planned directed source와 일치하는지도 plan evidence로 올려야 한다. ORCA는 enforcement가
  필요한 property와 child가 제공한 property의 mismatch를 optimizer/executor 내부 descriptor에서 비교하지,
  debug string의 전체 plan 모양으로 추론하지 않는다. AGE VLE도 `suppression-match=true|false`를 runtime feedback으로
  출력하면 assertion regression 대신 source handoff가 맞는지 직접 볼 수 있고, mismatch가 아니면 다음 작업을
  source enum rollback이 아니라 repeated source completion batching/root descriptor 보정으로 좁힐 수 있다.
- repeated empty completion을 줄이는 첫 구조 변경은 access method probe 결과를 activation-local scan state가 아니라
  source cache physical property로 보관하는 것이다. ORCA의 provided property가 child operator lifetime을 넘어
  상위 request 판단에 쓰이듯, AGE VLE의 `known_empty` payload cache state도 cursor lifetime보다 길게 살아야 한다.
  따라서 payload scan cursor cleanup과 payload cache cleanup을 분리하고, `empty-cache` plan evidence로 실제 cache
  hit을 검증한다. 이 방식은 단일 guard가 아니라 source-run lifecycle을 descriptor/cache boundary로 올리는 단계다.
- 서로 다른 frontier source의 empty completion은 같은 source 반복 cache보다 한 단계 더 상위 property다. ORCA식으로
  보면 `age_adjacency` scan이 현재 key의 payload stream뿐 아니라 directory cache range 안에서 "다음 key는 main/delta
  payload가 없다"는 negative provided property도 제공한다. 이 정보를 active scan 중 hash cache에 바로 쓰면 source
  entry lifetime과 cache mutation이 섞이므로, VLE source는 frontier empty key를 source-local batch로 모은 뒤 source
  종료 시점에 payload cache에 반영한다. Neo4j var-expand pruning처럼 frontier 확장에서 다음 source-run 생성을
  줄이는 방향이며, source enum rollback이나 단일 guard보다 큰 fan-out dataset에서 반복 empty completion을 줄이는
  구조 변경이다.
- lldb로 확인한 SIGSEGV는 frontier batching 자체가 아니라 header struct layout 변경 뒤 stale object가 남은
  incremental build 문제였다. `AgeVLESourceStats` field 추가 뒤 `age_vle.c`가 재컴파일되지 않아
  `cleanup_callback` offset을 잘못 읽었고, `make clean` 후 전체 재빌드로 해소됐다. VLE context/source stats처럼
  cross-module struct를 바꾸는 작업은 focused regression 전에 clean `COPT=-Werror` build를 우선해야 한다.
- frontier known-empty cache를 source object begin 단계에서만 소비하면 ORCA식 property request가 아직 너무 낮은
  레이어에 남는다. `add_valid_vertex_edges()`가 missing vertex fallback으로 내려가기 전에 active direction cursor가
  모두 `known_empty`인지 확인하면, source-run 자체를 "이미 제공된 empty property"로 처리할 수 있다. 이 경우
  `age_adjacency` scan object를 만들지 않고, missing-vertex attempt도 줄어든다. `empty-run` evidence는
  `empty-cache`보다 한 단계 위의 lifecycle skip으로, source family mismatch가 아니라 root/source-run planning
  개선 근거다.
- `missing-vertex-source` runtime class를 모든 missing fallback hit에 우선 적용하면 ORCA식 provided property 해석이
  다시 흐려진다. planned source와 dominant source가 일치하고, planned class가 `adjacency-cache-seeded`이며,
  empty suppression/frontier/run evidence가 있으면 runtime은 missing fallback이 아니라 planned adjacency lifecycle을
  충족한 것이다. 따라서 runtime feedback class를 planned class로 정규화하고 pressure를
  `adjacency-empty-suppressed`로 남겨야 planner threshold 보정이 source handoff mismatch가 아니라 empty source
  lifecycle 개선으로 이어진다.
- ORCA의 `CPhysicalIndexScan`/required property 구조와 Neo4j의 `bfsPruningVarExpand` planner shape는
  index/source lifecycle eligibility를 debug string에서 재구성하지 않는다. AGE VLE도 empty source lifecycle을
  `cost_policy` 문자열에만 두면 benchmark와 runtime feedback이 사람이 보는 surface에 묶인다. 따라서
  `empty_lifecycle_eligible`와 `empty_lifecycle_depth`를 `AgeVLEStreamEdgeSource` typed descriptor로 올리고,
  EXPLAIN은 `empty-lifecycle`/`empty-plan` surface로만 노출한다. 이것이 다음 root descriptor threshold 보정에서
  `empty-cache`, `empty-frontier`, `empty-run` evidence를 planned lifecycle과 직접 비교할 기반이다.
- benchmark도 같은 원칙을 따라야 한다. ORCA property enforcement처럼 planned property와 runtime provided
  property를 별도 column으로 비교해야 하며, `policy=` 문자열만 보는 join은 lifecycle mismatch를 분리하지 못한다.
  따라서 benchmark summary는 planner의 `empty_lifecycle/depth`와 runtime의 `empty_plan/depth`를 분리하고,
  `empty_plan_match`를 source/class/suppression match와 같은 1차 판단 축으로 둔다.
- raw EXPLAIN에도 같은 판단 축이 있어야 plan assertion 없이 원인을 볼 수 있다. `empty-plan match=true`는
  empty evidence가 없거나, empty evidence가 있고 planner descriptor가 empty lifecycle을 eligible로 갖는다는 뜻이다.
  `suppression-match=true`는 directed source가 맞는지를, `empty-plan match=true`는 lifecycle eligibility가 맞는지를
  분리하므로 threshold 조정 전에 source handoff 문제와 lifecycle batching 문제를 나눌 수 있다.
- missing-vertex fallback에만 known-empty source-run precheck를 두면 provided empty property가 graph metadata miss
  path에 갇힌다. ORCA식 property handoff에서는 child가 제공한 negative property를 상위 source request가 직접
  소비해야 하므로, 일반 vertex_entry expansion도 cursor 생성 직후 payload source object를 만들기 전에
  `known_empty` cache를 확인해야 한다. 이렇게 하면 frontier directory hint가 남긴 empty property가 다음 expansion의
  source-run boundary에서 소비되고, `empty-cache`가 아니라 `empty-run` evidence가 된다.
- reverse fanout에서 frontier hint가 생기지 않았던 이유는 `age_adjacency` AM이 현재 directory cache page 범위 안의
  missing key만 known-empty로 보았기 때문이다. delta chain이 없으면 directory lookup 자체가 complete negative
  property이므로, cache range 밖 key도 directory binary search로 확인해 empty frontier로 올릴 수 있다. 이는
  source enum 변경이 아니라 AM-provided physical property를 더 정확히 상위 VLE source-run에 전달하는 변경이다.
- planned empty lifecycle과 runtime empty evidence가 이미 일치하는데 `pressure=adjacency-empty-suppressed`로만
  남기면 다음 threshold 보정이 source handoff 문제인지, frontier/run 단계에서 정상적으로 소비한 negative
  property인지 구분하기 어렵다. ORCA의 physical property enforcement와 Neo4j pruning var-expand처럼 제공된
  property의 stage를 별도 vocabulary로 드러내야 한다. 따라서 raw EXPLAIN/benchmark에 `empty-evidence`를 추가하고,
  planned lifecycle match가 true인 경우 pressure를 `adjacency-empty-frontier` 또는 `adjacency-empty-run`으로
  분리한다. 이 상태는 source enum rollback 후보가 아니라 root/source lifecycle batching과 threshold feedback
  후보로 본다.
- root/source descriptor가 empty lifecycle을 threshold 입력으로 소비하려면 generic cache seed headroom과 repeated
  empty completion headroom을 분리해야 한다. `age_adjacency`가 payload replay만 제공하는 경우에는 endpoint-btree
  유지 여지를 넓게 둘 수 있지만, empty frontier/run evidence가 반복되는 workload에서는 source setup 자체를 상위
  lifecycle에서 접는 것이 더 큰 dataset에서 유리하다. 그래서 planned empty lifecycle eligible source는 endpoint
  유지 headroom을 75%가 아니라 50%로 낮추고, budget 안쪽이지만 이 headroom을 넘는 선택은
  `empty-lifecycle-headroom` reason으로 `age_adjacency`를 고른다.
- runtime feedback은 같은 statement의 이미 끝난 planning을 바꿀 수 없으므로 ORCA식 physical property처럼 다음
  request의 input property로 남아야 한다. 이번 단계에서는 catalog나 upgrade surface를 만들지 않고 backend-local
  feedback cache를 사용한다. key는 `graph,label,edge-label-oid,consumer-class,active-direction`으로 잡아
  source family가 아니라 같은 label relation과 consumer/lifecycle request에만 headroom feedback을 적용한다.
- ORCA의 required/provided property는 반복 관찰된 enforcement 결과를 다음 request의 강한 입력으로 해석한다.
  AGE VLE도 첫 `root-empty-saturated`는 empty batch lifecycle 근거지만, 같은 graph/label/direction/consumer에서
  이후 `root-empty-observed`가 계속 나오면 우연한 한 번의 saturation이 아니라 반복 empty source completion
  property다. 따라서 backend-local threshold cache는 이를 `root-empty-repeat-observed`로 승격하고 endpoint headroom을
  30%까지 낮춘다. 반복 saturation은 더 강한 `root-empty-repeat-saturated`로 보고 headroom 25%와 확장 batch를
  제공한다. 이 class는 `VLE Source Threshold Input`, planner `VLE Source Policy`, runtime `VLE Source Plan`에서
  같은 vocabulary로 드러나야 class mismatch가 source handoff 문제가 아니라 threshold request 문제인지 바로 보인다.
- `threshold-input=runtime-cache/headroom:N/batch:N`는 cache가 단순 explain 문자열이 아니라
  `VLESourcePolicyProfile`의 `cache_seed_endpoint_headroom`과 `empty_lifecycle_batch_size` 계산에 들어왔다는
  evidence다. 작은 regression에서는 source family가 그대로일 수 있지만, saturated root empty feedback은 다음
  planning에서 headroom을 0.35로 낮추고 batch를 현재 capacity의 2배 또는 관측 completion 수까지 키운다. 큰
  fan-out에서는 이 입력이 dense-local/`age_adjacency` threshold를 더 공격적으로 낮추고 repeated source
  completion을 작은 queue growth가 아니라 source lifecycle capacity로 흡수하는 근거가 된다.
- ORCA의 required/enforced physical property처럼 runtime feedback cache도 단순 최대값 누적이면 안 된다. 이미
  제공된 empty lifecycle property가 다음 request의 input으로 들어오되, 새 statement가 non-saturated completion을
  제공하면 batch를 관측 completion의 2배 수준으로 낮출 수 있어야 한다. 그래서 cache state를
  `observed/saturated/relaxed`로 나누고, `threshold-cache=...`를 planner surface에 드러낸다. 이는 source enum
  rollback이 아니라 lifecycle property가 현재 workload에 맞게 재조정됐는지 보는 evidence다.
- source completion batching은 단순히 frontier mark count를 세는 것과 다르다. ORCA식 property handoff 관점에서는
  frontier known-empty key들이 하나의 provided negative-property batch로 flush됐는지도 알아야 다음 request의
  lifecycle capacity를 판단할 수 있다. 따라서 `empty-frontier` mark 수와 별도로
  `empty-frontier-batch=flushes:N/out:N/in:N/keys:N/max:N`를 출력해 queue flush 폭을 benchmark join에서 비교한다.
- payload cache도 tuple count만으로는 planner feedback 입력이 부족하다. 같은 payload tuple 8건이라도 source-run
  1회 scan 뒤 seed된 경우와 source-run 8회 반복 scan은 dense-local/`age_adjacency` threshold 의미가 다르다.
  ORCA의 required/provided property처럼 cache replay 가능성은 source key lifecycle property이므로,
  `payload-cache=runs:scan/replay/seed`와 `tuples:scan/replay/seeds`를 분리해 benchmark join에서 본다. 다음
  threshold 보정은 payload tuple 수보다 scan/replay run 비율과 seed event를 우선 입력으로 삼는다.
- 같은 이유로 payload run evidence는 runtime 설명에만 남기지 않고 다음 planner request의 input property로도
  보관한다. `payload-input=runtime-cache/...`는 ORCA식 provided property가 다음 physical decision에 들어왔다는
  표시이며, Neo4j var-expand pruning처럼 반복 expansion source의 cache 가능성을 별도 vocabulary로 유지한다.
  현재는 seed/replay evidence가 endpoint headroom을 낮추는 쪽으로만 작동하고, 다음 단계에서 replay run 비율과
  scan run 비율을 분리해 더 공격적인 dense-local/`age_adjacency` threshold로 확장한다.
- directional feedback도 ORCA의 required/provided property와 같은 방식으로 해석한다. `active=both` 실행 결과를
  `out`/`in` key에 투영하는 것만으로는 충분하지 않고, directed 실행에서 먼저 관측된 repeated empty completion도
  후속 undirected request의 physical lifecycle input이 되어야 한다. 따라서 exact `both` feedback이 없으면
  `out`/`in` cache entry를 합쳐 더 낮은 headroom, 더 큰 empty batch, 더 강한 lifecycle class를 선택한다. 이는
  source enum을 되돌리는 fallback이 아니라 방향별 negative source completion을 undirected expansion의 provided
  lifecycle property로 승격하는 것이다.
- 다만 ORCA식 property handoff에서도 required property의 scope가 다르면 enforcement를 그대로 재사용하지 않는다.
  directed `out` 또는 `in` empty completion은 해당 방향의 provided negative property이지 undirected expansion 전체의
  exact property는 아니다. 그래서 AGE VLE는 family feedback을 소비할 때 lifecycle class/batch는 유지하되 headroom은
  40% floor로 완화한다. 이는 mixed direction evidence를 버리지 않으면서도 한 방향의 empty source가 다른 방향의
  productive source 선택을 과도하게 억제하지 않기 위한 contract다.
- Property projection도 같은 required/provided property 관점으로 본다. 같은 key path를 여러 physical result type으로
  요구하면 required output은 여러 개지만 provided raw property lookup은 하나일 수 있다. `AGE Property Projection`
  summary의 `heap-lookups`와 `reused`는 이 차이를 raw EXPLAIN에 드러내며, final materialization weight와 heap lookup
  weight를 분리해 이후 aggregate final descriptor 또는 partial materialization boundary로 넘길 수 있는 surface가 된다.
- aggregate property handoff도 같은 구분을 가져야 한다. `array_agg` map/list payload는 output semantic 때문에 repeated
  argument slot을 보존하지만, 같은 container/key source를 공유하는 slot은 lower target에서 하나의 raw lookup으로
  제공될 수 있다. 그래서 planner DEBUG descriptor는 total final materialization weight와 unique heap lookup
  `heap-final-weight`를 함께 출력한다. row-scaled credit은 output semantic을 위해 total final slot weight를 유지하되,
  repeated source lookup 제거분을 `reuse-weight`로 별도 더한다. 이는 final output slot 수와 heap source lookup 수를
  따로 소비하는 첫 단계이며, partial aggregate state layout 변경 없이 cost policy가 reuse descriptor를 읽게 한다.
- PostgreSQL `AggGetAggref()`는 aggregate transition support function에서 aggregate input expression을 볼 수 있게 한다.
  final function 관련 field는 공유 transition 때문에 불안정하지만, value argument expression identity는 slot-vector
  state의 source group을 만드는 데 충분하다. AGE는 같은 aggregate value expression을 같은 source group으로 기록하고,
  serialize format v4에 source group vector를 추가한다. 이로써 repeated output slot semantic은 보존하면서도
  partial aggregate combine/serialize boundary가 source reuse descriptor drift를 raw summary에서 드러낸다.

## 2026-06-05: runtime EXPLAIN 밀도와 terminal property precheck boundary

- VLE source runtime evidence는 benchmark가 읽을 수 있어야 하지만, 한 줄에 planned source, source match,
  class match, pressure/action을 모두 넣으면 index-heavy shape에서 사람이 읽는 plan surface가 길어진다. ORCA의
  required/provided property처럼 runtime provided property와 planner required property는 같은 node 아래에 두되
  별도 field로 분리하는 편이 낫다. 그래서 `VLE Source Runtime`은 observed runtime summary로 줄이고,
  `VLE Source Plan`은 planned out/in source와 class match만 담당하게 했다.
- `AGE Adjacency Match`의 right property residual은 metadata-backed property source index가 있어도 기존에는
  join 위 residual로만 남았다. constant property value와 terminal label id가 있으면 executor가 graph oid로
  label relation을 열고 vertex id btree로 후보 endpoint row를 확인할 수 있다. 이는 composite index seek까지
  가기 전 단계의 executor boundary precheck이며, 큰 fan-out에서 false endpoint를 slot emission 전에 줄이는
  구조다.
- 이 precheck는 SQL-visible predicate를 제거하지 않는다. property extraction/cast semantic은 상위 predicate가
  계속 보장하고, executor precheck는 metadata-backed index request와 AGTYPEOID constant value가 있는 shape에서만
  conservative row pruning으로 작동한다. dynamic right property value는 다음 descriptor/value slot handoff 후보로
  남긴다.
- ORCA의 `CPhysicalIndexScan`은 index descriptor와 required column set을 physical operator contract로 분리하고,
  Neo4j의 `NodeIndexSeek`/relationship seek도 expansion operator와 index seek evidence를 별도 logical surface로
  유지한다. AGE의 terminal property pruning도 scan node 내부 helper로 묻지 않고
  `AgeAdjacencyMatchTerminalPropertyRequest`와 opaque lookup module로 분리했다. 이 boundary가 다음
  label+property composite seek나 dynamic value slot handoff를 얹을 자리다.
- property source index가 AGTYPEOID expression index이면 terminal lookup은 adjacency payload row마다 vertex id
  btree를 타기 전에 property index를 한 번 스캔해 matching vertex id set을 만든다. 이는 Neo4j `NodeIndexSeek`
  결과를 expand의 provided endpoint property로 넘기는 방향과 유사하고, 큰 fan-out에서 terminal property false
  candidate가 많을 때 per-row heap lookup을 줄인다. typed property index는 expression result type이 다르므로
  현재는 id-btree fallback으로 둔다.
- metadata-backed terminal property index는 이제 단순 precheck helper가 아니라 planner/executor evidence surface다.
  planner는 `right_property_index_metadata_backed`와 index oid가 있으면 endpoint fanout row estimate와 recheck cost를
  낮추고, executor는 `prefetch-matches`, payload 후보 수, terminal filter 수, emitted row 수를
  `Adjacency Terminal Runtime`에 출력한다. 이 값은 다음 typed property index 또는 label+property composite seek가
  실제 후보 수를 얼마나 줄였는지 비교할 기준이다.
- `INDEX.md`의 compressed/sorted adjacency 방향에서 가장 즉시 줄일 수 있는 중복은 main posting의 endpoint key다.
  main pages는 directory entry가 `(key, first block/offnum, count)`를 이미 보관하므로 run scan 중 posting마다
  `key`를 다시 저장할 필요가 없다. 따라서 v4 main posting은 key를 제거하고, delta posting만 unordered insert
  scan을 위해 key를 유지한다. 이 구조는 CSR의 offset array가 source vertex를 한 번만 저장하고 destination list에는
  payload만 두는 방식과 같다.
- runtime visible payload main cache도 같은 원칙을 따른다. page cache entry는 full
  `AgeAdjacencyPostingData`가 아니라 `(heap_tid, edge_id, next_vertex_id)` compact payload를 보관하고, active key는
  visibility/property check 직전에 stack-local posting으로 재구성한다. 따라서 on-disk compression이 executor cache에서
  다시 풀려 memory width가 되돌아가는 것을 막는다.
- visible payload cache는 directory entry의 `first_offnum/posting_count`를 사용해 현재 key의 run window만 읽는다.
  main page에 여러 endpoint run이 같이 있어도 page 전체를 cache하지 않으므로 CSR offset/list boundary가 executor
  cache width와 allocation count에도 반영된다.
- sorted adjacency/CSR-style layout의 다음 중복은 page line pointer다. posting 하나를 page item 하나로 두면
  `(heap_tid, edge_id, next_vertex_id)` payload는 compact해도 `ItemIdData`와 per-item loop가 fan-out 수만큼 남는다.
  bulk-built main run을 run-local packed block으로 바꾸면 directory entry가 source key와 logical count를 한 번만
  들고, block item은 destination/edge payload array를 연속 저장한다. 이 구조는 CSR의 offset array와 destination
  array에 더 가깝고, PostgreSQL page 내부에서는 item 수를 fan-out 수보다 작게 유지한다.
- graphid는 16-bit label id와 48-bit entry id로 구성된다. edge-label adjacency index에서는 edge label id가 block
  안에서 대부분 고정되고, destination vertex label도 sorted run 안에서 자주 구간 단위로 고정된다. 따라서 main block은
  edge/next label id가 같은 chunk를 만들고 label id를 block header에 올린 뒤 각 posting에는 48-bit entry id만 저장한다.
  mixed label chunk는 full graphid block으로 fallback하지만, build는 label-homogeneous boundary로 chunk를 나눠 compact
  block을 우선 만든다.
- `age_adjacency_debug_main_probe()`는 main page logical offset 총량, active run window offset, packed block item
  수, compact/full block item 수, cached entry 수를 분리한다. 이는 sorted adjacency/CSR-style layout의 효과를 결과 row
  count가 아니라 cache와 page item lifecycle evidence로 확인하기 위한 surface다.
- delta posting은 append-only insert 때문에 directory run을 즉시 유지하기 어렵지만, page opaque의
  `min_key/max_key/posting_count` summary는 scan contract가 직접 소비한다. fixed payload scan과 VLE visible cursor는
  key range 밖 delta page를 tuple loop 전에 건너뛰므로, clustered insert workload에서 delta scan 폭을 줄일 수 있다.
  `age_adjacency_debug_delta_probe()`는 page visited/skipped와 tuple scan 수를 분리해 이 효과를 plan/result assertion이
  아니라 index lifecycle evidence로 보여준다.
- delta의 다음 폭 감소는 page-local label triple이다. append-only delta는 directory run을 즉시 유지하기 어렵지만,
  page 단위로 key/edge/next label id가 같도록 split하면 item에는 세 graphid의 48-bit entry id와 TID만 저장할 수
  있다. 이는 full graphid triple item보다 `MAXALIGN` 이후에도 page당 posting 수를 늘리고, `delta_tuples_per_page`
  maintenance evidence가 이 density를 보여준다. label triple이 바뀌면 새 delta page를 시작하므로 unordered insert
  semantics는 유지하면서 mixed-label fallback을 page split으로 처리한다.
- PostgreSQL `IndexPath.indexclauses`는 index key expression을 left operand로 정규화한 `IndexClause`를 cost hook에
  전달한다. `age_adjacency` cost hook은 first key equality qual의 right operand가 graphid `Const`이면 delta probe를
  실행해 tuple CPU cost를 key range가 실제로 scan할 delta posting 수로 계산한다. linked delta page를 따라 page
  opaque를 확인하는 비용은 아직 남으므로 random page cost는 유지하고, tuple loop CPU만 줄이는 것이 현재 AM
  runtime과 일치한다.
- reindex threshold도 단순 boolean stats보다 lifecycle action이 필요하다. `age_adjacency_debug_delta_maintenance()`
  는 delta가 없으면 `none`, single-page delta는 `observe-delta`, multi-page delta는 `range-skip-delta`, threshold
  이상이면 `reindex-delta`를 출력한다. 이는 실제 `REINDEX`를 자동 실행하지 않지만, benchmark와 향후 maintenance
  policy가 같은 action/reason vocabulary를 보게 해 compaction/rebuild decision을 plan/debug text 파싱에 묻지 않게
  한다.
- benchmark harness는 이 action을 실행 입력으로 사용한다. delta-heavy insert 뒤 action이 `reindex-delta`일 때만
  `REINDEX INDEX`를 실행하므로, threshold boolean을 사람이 읽는 진단값으로 두는 것이 아니라 maintenance lifecycle
  policy로 소비한다. DB 내부 자동 rebuild는 lock/transaction semantics를 더 검토해야 하므로 다음 단계 후보로 남긴다.
- `age_adjacency_reindex_if_needed()`는 이 검토를 명시적 SQL entry point로 좁혀 해결한다. 함수는 action이
  `reindex-delta`인 경우에만 PostgreSQL public `reindex_index()`를 호출하고, 그렇지 않으면 no-op 결과를 반환한다.
  background/autonomous rebuild가 아니므로 caller transaction과 PostgreSQL reindex lock semantics를 그대로 따른다.
- ORCA의 `CReqdPropPlan`/`CPhysical::CReqdColsRequest`는 required column/property를 문자열 reason으로 재해석하지
  않고 plan property 객체로 child handoff에 넘긴다. VLE source policy도 이 방향을 따른다. runtime feedback의
  `root-empty-*`, `payload-replay-*` reason은 사람이 읽는 detail로 유지하되, planner가 소비하는 값은
  `threshold_input_class`와 `payload_input_class`로 정규화한다. 이렇게 해야 EXPLAIN text 파싱 없이
  `adjacency-empty-batch`, `adjacency-cache-seeded`, `adjacency-replay` 같은 lifecycle class가
  policy decision, descriptor, runtime plan mismatch 비교에 같은 vocabulary로 전달된다.
- 같은 원칙을 property aggregate에도 적용한다. `array_agg` map/list property rewrite는 lower target에
  scalarized property expression을 넣고 끝내면 final materialization 비용이 aggregate handoff에서 사라진다.
  `CypherArrayAggPropertyHandoff`가 cached-property slot vector와 총 materialization weight를 보관하게 하면,
  map/list aggregate의 slot 수와 final agtype materialization 비용이 planner path 선택에 들어간다. 이는 ORCA식
  required/computed column property를 AGE의 aggregate final descriptor로 옮기는 첫 단계다.
- typed collect도 같은 boundary를 따라간다. cached-property slot이 있으면 path hook이 expression tree를 다시
  추론하지 않고 `CypherTypedCollectArgPlan`의 handoff descriptor에서 final materialization weight를 읽는다.
  aggregate credit helper를 `cypher_property_paths`에 두면 path hook은 ORCA physical operator처럼 lower/final target과
  child path assembly만 담당하고, computed property의 비용 vocabulary는 descriptor module이 유지한다.
- map/list aggregate 안에서 같은 property path가 반복되는 경우도 같은 관점으로 본다. aggregate state layout을
  바로 바꾸면 map/list output 순서와 duplicate key/list element semantic을 함께 다뤄야 하므로 blast radius가 크다.
  먼저 ORCA식 lower required expression boundary에서 동일 slot expression의 `sortgroupref`를 공유해 base
  scan/projection target 폭을 줄인다. aggregate argument vector는 그대로 유지하므로 final semantic은 보존되고,
  EXPLAIN의 child `Output`에서 중복 property lookup이 제거됐는지를 직접 확인할 수 있다.
- partial aggregate state도 같은 방향으로 봐야 한다. ORCA식 physical property가 lower/final boundary만이 아니라
  exchange/parallel aggregate boundary까지 전달된다고 보면, slot-vector aggregate의 serialization 폭은 큰 데이터셋에서
  직접적인 materialization cost다. null flag를 per-slot byte stream으로 두면 slot 수와 row 수에 비례해 불필요한
  전송 폭이 생기므로, null bitmap과 non-null payload stream으로 나누는 것이 descriptor 기반 slot layout과 더 맞다.
- ORCA의 `CLogicalIndexGet`/`CPhysicalIndexScan`은 index expression을 단순 rewrite helper가 아니라 index descriptor와
  required column/property contract로 다룬다. Neo4j도 index seek plan과 slotted runtime에서 indexed/cached property를
  별도 runtime value로 넘긴다. AGE의 property index rewrite도 query expression을 catalog expression으로 바꾸는 데서
  멈추면 안 되고, 실제 catalog index expression에서 얻은 cached-property slot metadata를 handoff에 보관해야 한다.
  이번 변경은 `CypherPropertyIndexHandoff`가 matched index OID와 index expression 기반 cached slot을 갖게 하여,
  후속 projection/aggregate/index handoff가 query surface가 아니라 catalog surface를 직접 소비할 수 있는 기반을 만든다.
- ORCA의 `CReqdColsRequest`는 required column set뿐 아니라 target child index와 scalar child index를 함께 key로
  사용한다. AGE VLE feedback cache도 query text나 policy reason만으로 묶으면 안 되고, active direction과 실제
  observed source direction을 분리해야 한다. Neo4j의 `pruningVarExpander`도 VarExpand를 BFS/Pruning으로 바꿀 때
  distinct/aggregation horizon dependency를 별도 객체로 보존한다. AGE는 이 원칙을 따라 undirected(`both`) 실행의
  root-empty feedback을 `out`/`in` directional cache key로도 투영해, 다음 directed traversal이 같은 physical lifecycle
  property를 재사용하게 했다.
- ORCA의 `CIndexDescriptor`는 index key column과 included column을 descriptor로 보관하고,
  `CPhysicalIndexScan`은 provided order와 residual predicate count를 physical scan property로 분리한다. Neo4j의
  `RelationshipIndexSeekPlanProvider`와 `DirectedRelationshipIndexSeekPipe`도 relationship type, indexed property,
  cached property request를 plan/runtime contract에 명시적으로 전달한다. AGE `age_adjacency`도 terminal label을
  executor callback 뒤 residual filter로만 두지 않고, physical payload order와 block descriptor가 직접 줄일 수 있는
  predicate로 올리는 것이 맞다.
- 이번 변경은 `age_adjacency` bulk build ordering을 `(endpoint key, heap TID)`에서
  `(endpoint key, next label id, edge label id, heap TID)`로 바꿨다. 같은 endpoint 안에서 terminal label이 섞인
  fan-out은 label별 compact main run block으로 묶이고, visible payload cursor는 compact block의 `next_label_id`
  descriptor로 label-mismatched block 전체를 건너뛴다. full block은 homogeneous label descriptor가 아니므로
  per-posting filter로 내려간다.
- terminal property prefetch도 emission 직전 callback에만 두지 않고 main cache load 단계에서 소비한다. metadata-backed
  property source index가 matching terminal vertex id set을 미리 갖고 있으면, main run block에서 posting을 cache에
  싣기 전에 `next_vertex_id`를 확인해 cache 폭 자체를 줄인다. 이는 Neo4j slotted runtime의 cached property request와
  ORCA residual predicate count 분리 원칙을 AGE source/cache lifecycle에 옮긴 것이다.
- 이 cache-load pruning은 label과 property를 같은 합산 counter로만 보면 다음 index 구조를 잘못 고를 수 있다.
  ORCA의 residual predicate count와 Neo4j의 label/type seek 분리는 선택도가 어느 predicate에서 왔는지 남기는 쪽에
  가깝다. AGE도 `cache-label`과 `cache-property`를 분리해 label-homogeneous block skip, property source prefetch,
  향후 label+property composite seek의 효용을 같은 runtime surface에서 비교한다.
- planner descriptor도 같은 전략을 이름 붙여 전달해야 한다. `terminal-source=label-block+property-source`는
  label-homogeneous adjacency block과 typed property source index를 결합한 strategy이고,
  `property-recheck`는 indexable source가 아니라 residual verification이다. 이 구분이 있어야 cost 보정과 benchmark
  join이 plan text의 여러 token을 재해석하지 않고 physical source contract를 직접 비교할 수 있다.
- property source index prefetch는 endpoint key와 무관하게 전체 matching vertex id set을 만들 수 있으므로,
  작은 endpoint run에서는 오히려 id-btree-cache recheck가 싸다. ORCA의 physical property가 child cardinality를 본 뒤
  enforcement를 선택하는 것처럼 AGE도 endpoint directory run을 연 뒤 active posting count를 보고 prefetch를 준비한다.
  이 경계가 있어야 label+property composite seek도 "항상 source prefetch"가 아니라 run-size-aware source lifecycle로
  설계할 수 있다.
- runtime evidence는 `Adjacency Terminal Prefetch: candidate-count=N threshold=M skipped-small=K`로 분리했다.
  작은 run에서 `skipped-small=1`이면 composite seek 설계는 더 넓은 source prefetch가 아니라 vertex-cache/id lookup
  계열을 유지해야 한다. 이 recheck 경로는 `agtype_object_field_equals`의 fmgr cache에 의존하지 않는 local equality
  helper를 사용한다.
- terminal property source request는 이제 value source도 descriptor로 갖는다. `value=const`와
  `value=runtime-slot`은 metadata-backed property index가 있으면 prefetch eligible로 다루고, 다른 MATCH 변수처럼
  현재 CustomScan의 `required_outer` 밖 relid에 의존하는 값은 `value=none prefetch=ineligible`로 낮춘다. 이는
  assertion/guard가 아니라 planner/executor handoff가 실제 runtime value slot 가능성을 표현하게 하는 변경이다.
- property source index metadata는 `options.property_type`도 보존한다. typed index(`int8`, `float8`, `numeric`,
  `text`)는 AGTYPE equality prefetch와 같은 key domain이 아니므로, executor는 index expression의 실제 첫 key
  type을 읽고 agtype scalar runtime value를 typed scan key로 변환한다. 변환 실패는 prefetch 비활성화로 낮춰
  semantic recheck 경로를 유지한다. 이는 ORCA의 index descriptor가 key type과 residual predicate를 함께 들고,
  Neo4j slotted runtime이 cached property slot을 typed value로 넘기는 구조와 같은 방향이다.
- 작은 regression fixture에서는 같은 endpoint의 6개 posting이 label별 2개 compact block으로 묶이는지 확인한다.
  실제 목표는 큰 fan-out에서 label-mismatched payload cache width, heap visibility check, terminal property prefilter
  입력을 block 단위로 줄이는 것이다.
## 2026-06-05: typed aggregate payload domain을 cost 입력으로 승격

- ORCA의 required/computed column 관점에서는 lower target이 어떤 physical domain을 제공하는지가 enforcement와 cost
  선택의 입력이다. AGE typed `array_agg` slot-vector도 final projection만 typed/agtype을 구분하면 부족하고,
  transition state가 읽는 payload domain을 planner handoff에 보존해야 큰 hash/partial aggregate shape에서 width와
  materialization 비용을 다르게 볼 수 있다.
- `CypherArrayAggPropertyHandoff`는 이제 cached-property slot count 외에 typed/agtype payload slot count와 payload
  materialization weight를 들고, row-sensitive materialization credit은 이 descriptor를 직접 소비한다. 이는 새
  aggregate helper 함수를 나열하는 방향이 아니라 slot-vector/final materialization contract를 넓히는 방향이다.
- runtime `age_array_agg_*_slots_summary`도 `final-weight`와 `materialization-weight`를 출력한다. ORCA식으로 보면
  lower target이 제공한 payload domain과 final operator가 요구하는 materialization domain을 같은 state header에서
  비교할 수 있게 된 것이며, partial aggregate boundary에서 typed/agtype slot mismatch를 plan/runtime surface로
  올리는 다음 단계의 기준이다.
- slot-vector type mismatch surface는 guard가 아니라 aggregate state header contract 검증이다. PostgreSQL의
  polymorphic aggregate argument type은 query plan 안에서 고정되므로 정상 SQL fixture가 같은 slot의 타입을 row마다
  바꾸지는 않지만, partial aggregate serialize/combine boundary나 descriptor drift가 깨지면 slot index와
  expected/actual type detail이 바로 드러나야 한다. 이는 hidden assertion보다 raw runtime output을 우선한다는
  regression 원칙과 맞다.
- resolved argument descriptor 함수는 planner hook을 새로 만들지 않고도 PostgreSQL이 정한 `variadic any` input type
  vector를 runtime state header와 같은 text contract로 드러낸다. 이 단계는 완전한 cost/explain descriptor가 아니라
  partial aggregate boundary의 비교 기준을 먼저 고정하는 것이다. 다음 index-domain handoff는 이 vocabulary를 그대로
  사용해 catalog expression surface가 aggregate payload vector를 어떻게 줄였는지 보여줘야 한다.
- PostgreSQL standard `AggPath`에는 AGE CustomScan처럼 별도 `ExplainPropertyText`를 붙일 수 없으므로, raw EXPLAIN
  surface는 target expression과 child plan으로 만든다. index-backed aggregate fixture는 descriptor call을 aggregate
  target에 함께 두고 child `Index Scan`의 catalog expression surface를 보여준다. planner 내부 evidence는 DEBUG2
  descriptor formatter가 보완하며, 다음 단계의 cost credit은 이 descriptor의 `index-matched`와 weight field를 직접
  소비해야 한다.
- index-domain match는 단순히 helper rewrite가 성공했다는 boolean이 아니라, aggregate lower target이 catalog expression
  surface와 같은 physical signature를 읽는다는 width signal이다. 따라서 `index-matched` slot count는 일반
  payload/final materialization credit에 섞지 않고, row 수와 `cpu_tuple_cost`에 비례하는 별도 width credit으로 더한다.
  큰 indexed aggregate에서는 이 credit이 expression index surface를 보존하는 narrow path를 더 빨리 선택하게 만드는
  근거가 된다.
- benchmark는 natural planner choice와 index-domain surface 관찰을 분리해야 한다. 작은 row count smoke에서는
  PostgreSQL이 selective predicate도 seq scan으로 고를 수 있으므로, `tools/aggregate_index_benchmark.sql`은 indexed
  selective aggregate case에만 `enable_seqscan=off`를 적용해 index-backed lower target과 descriptor surface를 안정적으로
  관찰한다. typed aggregate baseline은 기본 planner 선택으로 남겨 payload vector materialization 비용을 비교한다.
- 같은 harness에 natural selective shape를 따로 둔다. 강제 index shape는 "index-domain lower target이 어떤 surface를
  제공하는가"를 확인하고, natural shape는 현재 cost credit이 실제 planner 선택을 바꾸는지를 확인한다. `rows=20`
  smoke에서는 natural shape가 seq scan을 유지했으므로, 다음 보정은 작은 fixture가 아니라 큰 row count threshold를
  기준으로 해야 한다.
- `rows=100/250/500` smoke는 현재 통계와 cost setting에서 natural index 선택 threshold가 100과 250 사이에 있음을
  보여줬다. 이 값은 고정 상수가 아니라 width/index credit 보정의 관찰점이다. 다음 비교는 같은 threshold 근처에서
  typed payload vector aggregate와 index-backed single-slot aggregate의 row width/materialization 차이를 함께 봐야 한다.
- threshold sweep은 같은 session에서 여러 graph를 만드는 방식이 낫다. 같은 graph에 row를 누적하면 stats/history가 섞여
  threshold 원인이 흐려진다. `threshold_rows`는 row count별 graph를 분리해 natural/forced 선택과 execution time을
  같은 summary schema로 비교하게 한다.
- payload width 비교를 하려면 descriptor call도 shape별로 달라야 한다. indexed selective shape는 single `agtype` slot을,
  typed baseline은 `numeric,bigint` slot vector를 출력하게 해 EXPLAIN summary가 실제 aggregate payload domain을
  보여주도록 했다.
- threshold sweep은 row별 detail만으로 끝나면 다시 사람이 눈으로 비교해야 한다. harness는 마지막 natural seq scan과
  첫 natural index scan row count를 별도 summary로 출력해, 다음 cost 보정이 관찰된 boundary를 직접 소비하게 한다.
- 다만 이 threshold는 child relation scan 선택 boundary다. `cypher_array_agg_property_materialization_credit()`의
  index-domain width credit은 aggregate lower/final target 후보의 상대 cost를 바꾸는 signal이고, child `Index Scan`
  여부는 PostgreSQL base relation path 선택 결과다. 두 값을 한 `uses_index` boolean으로 읽으면 child scan threshold를
  aggregate narrow path 효과로 오해할 수 있으므로 benchmark summary는 `child_uses_index`와
  `aggregate_uses_slot_vector`를 분리한다. 다음 cost 보정은 child scan boundary를 관찰값으로 쓰되, 실제 조정 대상이
  aggregate payload width credit인지 base scan selectivity/cost surface인지 먼저 나눠야 한다.
- index-domain width credit은 matched slot count만으로는 약하다. ORCA식 physical property 관점에서는 index expression
  surface가 제공하는 값이 몇 개인지뿐 아니라 그 값이 final `agtype` materialization 전에 줄여 주는 payload width도
  cost 입력이어야 한다. 따라서 `index-matched` slot 수에 slot당
  `payload-weight + final-weight + type-vector-width` 평균을 곱한 `index-width-weight`를 계산하고, row-scaled
  `cpu_tuple_cost` credit은 이 weight를 소비한다. single-slot `agtype` aggregate는 기존 count-only credit보다 강하게
  보정되고, multi-slot aggregate에서 일부 slot만 index-domain match되는 경우는 전체 aggregate weight를 과대 적용하지
  않는다.
- benchmark summary는 EXPLAIN target expression만으로는 부족하다. 실제 `age_array_agg_slots_descriptor()` 결과를
  shape별로 실행해 `slot_count`, typed/agtype slot count, payload/final/materialization weight, `slot_types`,
  `aggregate_rows`를 함께 출력하게 했다. `threshold_rows=20,100,250` smoke에서는 child natural index boundary가
  100과 250 사이로 보였고, indexed single-slot `agtype` descriptor와 typed two-slot `numeric,int8` descriptor가 모두
  `materialization-weight=4`를 출력했다. 따라서 다음 보정은 단순 materialization weight 상수 조정보다 slot count/type
  mix와 aggregate input rows가 partial aggregate state width에 어떻게 반영되는지를 먼저 봐야 한다.
- slot-vector summary는 이제 실제 partial state serialize layout에서 `serialized-bytes`, `null-bitmap-bytes`,
  `value-bytes`를 계산한다. 20000-row parallel regression에서 scalar `agtype` 1-slot은 value bytes가 373352이고,
  typed 3-slot list는 888896, typed 3-slot map은 key bytes 차이를 포함해 serialized bytes 787547로 드러난다. 이는
  payload/final materialization weight가 같은 shape라도 partial aggregate state width는 row count, null bitmap,
  varlena payload, map key layout에 의해 달라진다는 evidence다. 다음 cost 입력은 `materialization-weight`만 보지 말고
  slot type별 wire width와 aggregate rows를 함께 소비해야 한다.
- planner는 runtime Datum payload를 알 수 없으므로 executor의 `value-bytes`를 그대로 cost에 넣을 수 없다. 대신
  `CypherArrayAggPropertyHandoff`가 slot type별 estimated wire width를 누적하고, null bitmap overhead를 더한
  `state-width-weight`를 row-scaled materialization credit에 포함한다. DEBUG2 descriptor는 `wire-width`와
  `state-width-weight`를 출력한다. 이 값은 benchmark의 실제 byte evidence와 비교해 조정할 cost input이며, 현재 목적은
  partial aggregate state 폭이 cost surface에 들어가게 만드는 것이다.
- 같은 estimate를 `age_array_agg_slots_descriptor()`와 summary aggregate도 출력한다. benchmark는
  `slot_estimated_wire_width`와 `slot_estimated_state_width_weight`를 `slot_value_bytes` 옆에 둔다. 따라서 다음 sweep은
  hidden DEBUG2 log 없이도 runtime byte와 planner-compatible estimate 차이를 row count/shape별로 비교할 수 있다.
- benchmark는 descriptor row와 별개로 label table의 direct slot-state summary도 수집한다. 이 row는
  `slot_state_value_bytes`, `slot_state_serialized_bytes`, `slot_state_value_bytes_per_row`,
  `slot_value_estimate_ratio`를 출력한다. `threshold_rows=20,100` smoke에서는 single-slot `agtype` payload가 28 bytes/row
  대 estimate 52, typed `numeric,int8` payload가 24 bytes/row 대 estimate 40으로 나왔다. 다음 estimate 보정은 이
  ratio를 row count별로 본 뒤 type별 varlena 기본폭을 낮출지, aggregate state width credit scaling을 조정할지 결정한다.
- smoke evidence를 기준으로 planner-compatible wire width estimate를 낮췄다. `agtype` scalar slot은 28 bytes,
  `numeric`은 12 bytes, short `text`는 20 bytes를 기본값으로 둔다. `threshold_rows=20,100` benchmark에서 single-slot
  `agtype`과 typed `numeric,int8` 모두 `slot_value_estimate_ratio=1.00`으로 맞춰졌다. 다음 비교는 이 작은 synthetic
  fixture에 과적합하지 않는지 더 큰 row count와 wider text/numeric payload에서 확인하는 것이다.
- handoff는 slot별 `payload_value_types` vector도 보관한다. narrow path 후보는 cached-property slot count와
  payload value type vector length가 같아야 하며, cost는 단순 slot count 대신 이 vector cardinality를 읽는다.
  aggregate cached slot은 같은 `CypherPropertyHandoffDescriptor`도 보존하므로, 다음 index join은 원본 expression을
  다시 해석하기보다 slot descriptor의 property signature, value type, aggregate OID를 기준으로 시작할 수 있다.
  property index handoff도 query/index cached slot의 physical signature match를 descriptor field로 보존한다.
  aggregate handoff refresh는 이 match 결과를 lower target 후보와 직접 연결해 catalog index expression surface를
  aggregate input으로 올리고, row-sensitive credit에 domain-matched index slot count를 반영한다. 다음 단계는 이
  source descriptor를 partial aggregate state header evidence까지 드러내는 것이다. EXPLAIN regression은 이미
  `array_agg(n.payload.a)` lower input이 nested field helper 대신 property source index의 catalog expression surface로
  바뀌는지를 보여준다.

- ORCA의 cardinality-sensitive enforcement 선택과 Neo4j의 label/type seek 분리 관점에서는 prefetch eligibility만
  보는 것도 부족하다. `age_adjacency` v6는 directory entry에 endpoint run의 terminal label distinct count를 추가해
  mixed-label fanout을 label-aware candidate estimate로 낮춘다. 이 값은 terminal property prefetch threshold에 직접
  들어가며, 작은 label slice에서는 global property source prefetch를 피하고 local id lookup/cache recheck를 선택한다.
  다음 단계는 이 근사치를 label+typed property composite seek의 선택도/비용 입력으로 바꾸는 것이다.
- executor/debug surface도 전체 run cardinality와 label-slice cardinality를 분리했다. ORCA의 physical property가
  enforcement 입력을 명시적으로 들고 Neo4j의 relationship seek가 type/property seek를 plan/runtime contract로 나누는
  것처럼, AGE도 `run-count`와 `candidate-count`, `main_label_groups`를 따로 보여야 composite seek가 어느 부분의
  cardinality를 줄였는지 판단할 수 있다.
- planner descriptor 역시 endpoint fanout과 terminal fanout을 분리했다. ORCA식으로 보면 `terminal-fanout`은 label
  source가 제공하는 required physical property의 cardinality estimate이고, Neo4j식으로 보면 relationship expansion 뒤
  terminal label/type slice의 seek 입력이다. 따라서 terminal property prefetch threshold와 row estimate는 endpoint 전체가
  아니라 terminal slice fanout을 사용해야 한다.
- threshold reason도 descriptor에 올렸다. ORCA의 enforcement reason처럼 `small-terminal-fanout`과
  `large-terminal-fanout`을 분리해야 feedback/cache/composite seek가 단순 threshold 숫자 변화에 묶이지 않는다. 작은
  terminal slice는 property source prefetch 회피의 근거이고, 큰 terminal slice나 edge payload required shape는 source
  prefetch 또는 composite seek를 더 적극적으로 검토할 근거다.
- runtime outcome/action은 reason이 실제로 어떤 source lifecycle로 실행됐는지 보여준다. `id-cache-small`은 작은
  terminal slice에서 property source prefetch를 쓰지 않는 정상 결과이고, 향후 composite seek는 이 class를 더 넓은
  global source scan으로 되돌리지 말고 label+typed key가 충분히 선택적인 경우에만 대체해야 한다.
- cost model도 planned lifecycle을 소비해야 한다. prefetch eligible이라는 catalog property만 보고 selectivity credit을
  주면 작은 terminal slice에서 실제 runtime이 `keep-id-cache`로 실행되는 shape를 과소평가한다. `planned=id-cache-small`
  surface와 cost 보정은 ORCA의 enforcement choice와 runtime property가 같은 contract를 보게 하는 조정이다.
- `lifecycle-match`는 planned enforcement와 actual runtime outcome을 직접 비교한다. 이는 ORCA식 required/provided
  property 검증과 Neo4j식 plan/runtime operator evidence를 AGE EXPLAIN에 맞춘 surface이며, 다음 feedback 작업에서
  mismatch만 policy correction 대상으로 삼게 한다.
- ORCA `CPhysicalIndexScan`은 `CIndexDescriptor`만 갖는 것이 아니라 `ResidualPredicateSize()`를 cost model에 넘긴다.
  Neo4j relationship index seek도 index-compatible predicate set과 hidden selection을 분리한다. AGE
  `Adjacency Pruning`도 boolean 나열만으로는 residual/index-solved 정도를 cost/feedback에서 직접 쓰기 어렵다.
  따라서 `index-solved=N`과 `residual-count=N`을 EXPLAIN surface에 추가해 label-block/property-source가 몇 개 predicate를
  source side에서 해결했고, 몇 개가 join/heap verification으로 남는지 같은 vocabulary로 보게 했다.
- 같은 count를 `cost_adjacency_match_custom_path()`에서도 소비한다. residual predicate가 많으면 CPU verification weight를
  올리고, label-block/property-source처럼 source side에서 해결한 predicate가 있으면 verification cost를 낮춘다. 작은
  fixture에서는 plan shape를 바꾸지 않아도, 큰 fan-out에서 residual-heavy path와 source-pruned path의 cost 차이를
  누적시키는 방향이다.
- `Adjacency Cost Input`은 이 cost vocabulary를 raw EXPLAIN에 올린다. `residual-weight`와 `index-credit`을
  percent 단위로 출력하므로 planner cost change가 hidden heuristic이 아니라 descriptor surface와 같은 값으로
  regression에 고정된다. planned prefetch threshold와 reason도 같은 line에 출력해 non-ANALYZE plan에서도
  terminal property source decision을 읽을 수 있게 했다.
- const-bound endpoint는 통계 추정만으로는 mixed-label run의 terminal slice를 알 수 없다. ORCA식 descriptor
  관점에서는 physical index가 이미 보유한 directory summary를 planner property로 올리는 편이 맞고, Neo4j식
  relationship expansion 관점에서는 bound node id seek 뒤의 relationship type/terminal label slice cardinality를
  따로 비용화해야 한다. AGE는 `age_adjacency` directory entry에서 endpoint run posting 수, terminal label posting 수,
  terminal label group 수를 읽어 `fanout-source=directory`로 descriptor에 싣기 시작했다. dynamic endpoint는 여전히
  `fanout-source=statistics`로 남긴다.
- 이 변경은 작은 fixture에서 plan shape를 강제로 바꾸는 목적이 아니라, 큰 데이터셋에서 label이 섞인 high fan-out
  endpoint를 만났을 때 terminal label 후보를 정확히 줄이는 cost 입력을 마련하는 작업이다. 다음 후보는 같은 directory
  evidence를 label+typed property composite seek와 VLE frontier source 후보 축소에 연결하는 것이다.
- VLE stream은 start/end arg가 runtime slot이어도 base vertex relation에 `id = const` restriction이 있으면 endpoint
  graphid를 증명할 수 있다. PostgreSQL planner 관점에서는 parameterized inner path의 outer var 그대로지만, graph
  operator descriptor 관점에서는 이미 bound endpoint key가 고정된 shape다. 따라서 relation-level distinct 통계 대신
  `age_adjacency` directory run posting 수를 source fanout evidence로 올리는 것이 ORCA식 required/provided property
  handoff에 더 가깝다. EXPLAIN은 `source=start:directory/end:statistics`처럼 evidence source를 분리해, 이후 feedback이
  statistics miss와 directory-backed decision을 구분하게 한다.
- terminal label constraint가 있으면 directory run posting 수도 아직 과대 추정이다. Neo4j식 relationship expansion 뒤
  node label seek와 ORCA식 residual predicate split 관점에서는 terminal label slice를 source-provided property로
  취급해야 한다. AGE VLE는 output descriptor의 terminal label id를 `age_adjacency` directory estimator에 넘겨
  `directory-label` fanout을 만든다. 이 단계는 label slice까지이고, 다음 단계의 composite request는 같은 endpoint run
  안에서 typed property source selectivity까지 함께 봐야 한다.
- VLE terminal-property direct output은 terminal label constraint를 항상 갖고 있지 않다. 따라서 첫 단계에서는
  property key만으로 `ag_graph_index` NODE `PROPERTY` metadata를 조회하고, matching label/source/provider/type/count를
  `VLE Terminal Property Source`로 노출한다. ORCA식으로 보면 아직 enforced index scan은 아니고 provided source
  metadata다. Neo4j식 label/property seek로 가려면 이 metadata를 terminal label slice와 결합해 source candidate
  label set을 줄이고, property value predicate가 있는 shape에서는 typed scan key와 frontier id set prefetch까지
  같은 descriptor에 묶어야 한다.
- ORCA `CPhysicalIndexScan`은 index descriptor, output column vector, residual predicate count, provided order를 함께
  physical operator contract로 출력하고 비용 입력으로 쓴다. Neo4j `RelationshipIndexSeekPlanProvider`도 relationship
  type, indexed property, solved predicate, provided order, cached indexed property를 seek plan에 묶는다. AGE fixed
  MATCH는 이 원칙을 따라 terminal label slice와 property source lifecycle을 `Adjacency Composite Source`로 분리했다.
  `terminal-fanout`은 label slice 후보 수이고 `composite-fanout`은 property source prefetch까지 반영한 cost 후보 수다.
  작은 terminal fanout에서는 `planned=id-cache-small`로 남기고, 큰 label slice나 edge payload requirement에서만
  source prefetch를 선택하게 한다.
- 이를 위해 VLE marker argument도 단일 union 슬롯에서 벗어나야 한다. `terminal-property` 슬롯 하나가 string key 또는
  label id를 번갈아 담으면 ORCA식 required/provided property처럼 label slice와 property source를 동시에 표현할 수
  없다. 이번 단계는 `terminal-label` 슬롯을 별도로 추가해 10-arg marker를 수용하게 만들고, 아직 label slot이 없는
  property-only direct output은 `missing-terminal-label` composite boundary로 남긴다. 다음 planner/parser 변경은
  named labeled terminal node가 label relation join으로 되돌아가는 shape를 10-arg VLE marker로 낮추는 것이다.
- PostgreSQL `selfuncs.c`의 equality selectivity는 MCV가 없을 때 `get_variable_numdistinct()` 기반
  `1 / ndistinct`를 기본 근거로 삼는다. AGE property source index는 실제 graph index metadata에 btree index OID를
  저장하므로, index expression stats의 `pg_statistic(starelid=index_oid, staattnum=1)`에서 `stadistinct`를 읽을 수
  있다. 이 값은 property source relation tuple 수가 아니라 typed value domain의 distinct 수이므로, VLE/fixed
  adjacency composite fanout을 줄이는 근거로 더 적합하다.
- PostgreSQL `selfuncs.c`는 const equality에서 MCV value를 equality operator로 비교하고 matching frequency를
  선택도로 쓴다. AGE도 property source index expression stats의 MCV slot을 읽어 const agtype value와 equality
  operator로 비교한다. MCV frequency가 fallback보다 선택적이면 `typed-mcv`를 쓰고, MCV가 common value라 fallback보다
  넓으면 `fallback-mcv-ceiling`으로 fanout 확대를 막는다. 작은 데이터에서 효용이 작아 보여도 큰 데이터셋의
  high-cardinality property source에서 정확한 value selectivity가 나타나는 구조를 우선한다는 브랜치 정책과 맞다.
  다음 연구 후보는 이 selectivity를 label+value composite request의 runtime fetch key로 내려 실제 후보 materialization
  폭까지 줄이는 것이다.
- ORCA의 required/provided physical property handoff 관점에서는 property prefetch 결과도 단순 callback이 아니라
  후보 집합의 물리 속성으로 전달해야 한다. 이번 sorted vertex id vector는 그 첫 단계다. Neo4j의 index seek/expand
  pruning도 solved predicate와 remaining expansion을 분리하므로, AGE에서는 property source가 solved한 terminal id
  set을 range/sorted descriptor로 adjacency expansion에 넘기는 것이 맞다. 아직 directory run 자체를 건너뛰지는
  않지만, hash callback보다 directory-compatible composite summary로 확장하기 쉬운 형태다.
- compact main block precheck는 이 descriptor를 posting emission보다 한 단계 위로 올린다. ORCA식으로는
  provided sorted terminal-id set이 block-local physical property와 교차하는지 확인한 뒤 child tuple 생산 여부를
  결정하는 구조다. Neo4j var-expand pruning 관점에서도 relationship expansion 뒤 residual filter를 반복하는 것보다
  solved terminal id set을 expansion cursor에 넘기는 쪽이 맞다. 현재는 compact block data를 한 번 훑어 교차 여부를
  판단하지만 heap fetch, payload cache fill, per-posting residual filter는 피한다. 다음 구조 후보는 block header나
  directory entry가 label+property summary를 직접 갖도록 올리는 것이다.
- directory v7의 next vertex id range summary는 sorted descriptor를 block보다 더 위로 올린 단계다. ORCA의 physical
  property로 보면 endpoint run이 제공하는 terminal-id range와 property source가 제공하는 candidate-id range의
  교차 여부를 scan open 시점에 판정한다. VLE처럼 run count를 먼저 읽고 prefetch set을 나중에 붙이는 executor
  lifecycle에서는 setter가 active directory entry를 다시 확인해야 한다. 이 contract가 없으면 directory summary는
  planner/debug surface에만 머물고 실제 cache fill을 줄이지 못한다.
- same summary는 known-empty precheck에도 들어가야 한다. VLE frontier empty batching은 payload scan을 실제로 열기
  전에 `key_known_empty()`를 물어보므로, terminal label만 보던 기존 contract로는 property source가 제공한
  candidate-id range를 source completion evidence로 승격할 수 없었다. range-aware known-empty는 directory summary를
  source lifecycle 입력으로 올리는 단계다.
- directory range pruning이 실제 candidate를 0으로 만들었을 때 이를 `empty-scan`으로 기록하면 planner feedback이
  source 선택 실패 또는 empty lifecycle 미적용으로 오해한다. ORCA식 physical property handoff 관점에서는 이 케이스는
  child scan 실패가 아니라 property source가 제공한 terminal-id set과 adjacency directory run의 물리 속성이 교차하지
  않은 것이다. 따라서 VLE runtime evidence는 `directory-filter`로 정규화하고, 다음 cost feedback은 이를
  `adjacency-composite-prefilter` class와 같은 성공 evidence로 소비해야 한다.
- 이 evidence는 cache-seed lifecycle과 독립적이다. 단일 step terminal-scalar VLE는 cache seed가 ineligible이어도
  terminal label/property composite prefilter 자체는 성공한 physical property다. 따라서 runtime feedback cache lookup은
  `cache_seed_eligible`만 gate로 쓰지 않고 `composite_prefilter_planned` profile도 허용하되, 해당 payload class는
  composite-planned profile에서만 소비하게 제한한다. 이렇게 해야 directory-filter 성공이 다음 composite plan input으로
  올라오면서도 unrelated terminal-scalar VLE로 전파되지 않는다.
- ORCA/Neo4j식 seek handoff 관점에서는 terminal label과 property source matched set을 별도 setter 순서로 조합하면
  composite seek request가 아니라 executor side effect가 된다. `AgeAdjacencyCompositeTerminalFilter`는 label id,
  property source vertex set, callback fallback을 하나의 physical request로 묶어 `age_adjacency` scan에 넘긴다.
  현재는 기존 range/sorted set을 담지만, 다음 확장은 같은 request에 property key/value summary나 directory-level
  value summary handle을 추가하는 방식이 되어야 한다.
- 이번 단계는 그 확장 지점에 property source OID, filter identity, matched count를 싣는 summary를 먼저 올렸다.
  아직 directory entry가 property value summary를 직접 보유하지는 않지만, executor handoff는 이제 label+property
  request를 식별할 수 있다. 따라서 다음 index layout 변경은 scan target 외부의 callback state가 아니라 composite
  request summary를 기준으로 directory-level value summary를 비교하면 된다.
- request-side value summary는 on-disk value posting summary로 가기 전의 좁은 stepping stone이다. ORCA식
  physical property 관점에서는 property source index가 제공한 matched entity id set이 이미 request descriptor에
  있으므로, 이를 callback residual로만 쓰지 않고 directory summary와 교차 가능한 provided property로 올리는 것이
  맞다. Neo4j식 label+property seek handoff와 비교해도 value identity가 있는 request와 label-only expansion은
  다른 physical request다. 현재 구현은 `property_filter_id`가 있는 composite request만 matched vertex id bloom을
  준비해 directory 64-bit/wide bloom과 교차하고, skip evidence를 `/value-summary:N`으로 분리한다. 아직 value
  자체를 index tuple에 저장하지 않으므로 false positive는 남지만, 다음 변경에서 terminal value identity별
  directory posting summary나 block-local value posting vector가 어느 residual을 줄이는지 같은 regression surface로
  비교할 수 있다.
- block-level value-summary counter는 range/exact/compressed/bloom/posting summary를 대체하지 않는다. ORCA식
  physical property로 보면 “어떤 block summary가 negative를 증명했는지”와 “그 negative가 어떤 required value
  identity request에서 나온 것인지”는 서로 다른 축이다. Neo4j식 relationship expansion에서도 seek predicate와
  expansion physical operator evidence를 분리해서 읽어야 한다. 그래서 `set-block-filter=.../compressed:N` 같은
  physical summary evidence는 유지하고, value identity request 축은 `/value-summary:N`으로 겹쳐 출력한다. 이
  surface가 있어야 다음 on-disk value posting summary가 단순히 기존 block summary skip을 재라벨링한 것인지,
  실제로 residual false positive를 줄였는지 구분할 수 있다.
- v16 directory compressed vector는 value-keyed posting summary 자체는 아니다. 하지만 exact graphid slot overflow와
  wide bloom false positive 사이에 있던 gap을 directory 단계의 48-bit `next_entry_id` exact summary로 줄인다.
  homogeneous terminal label run에서만 쓰므로 graphid label domain이 안정적이고, ORCA식 provided property 관점에서는
  “directory가 제공하는 exact endpoint-id set”을 더 넓힌 변경이다. 다음 단계에서 terminal value identity별 summary를
  붙일 때는 이 vector를 전체 run endpoint summary로 재사용하지 말고, property source index OID/filter id/value
  identity와 결합된 별도 posting key로 분리해야 한다.
- value identity 생성은 VLE payload cache와 fixed `AGE Adjacency Match` composite request가 공유해야 한다.
  VLE만 property index OID와 predicate value hash를 섞고 fixed MATCH는 property source OID만 넘기면, 같은
  label+property predicate가 source path에 따라 다른 pruning contract를 갖게 된다. 그래서
  `age_adjacency_property_filter_id()`를 공용 helper로 두고 VLE와 fixed MATCH terminal lookup이 모두 같은 helper를
  호출하게 했다. 다음 value posting summary는 이 identity를 cache key가 아니라 directory/block summary key로
  소비해야 한다.
- raw EXPLAIN에는 hash 값을 출력하지 않는다. value image hash는 executor-local identity이고 OID/value image에
  묶이므로 expected 파일에 숫자를 고정하면 source contract보다 incidental encoding을 검증하게 된다. 대신 fixed
  `AGE Adjacency Match` runtime은 `value-identity=present|none`만 출력해, value-keyed request가 만들어졌는지와
  not-indexable recheck path를 분리한다.
- directory-level pruning evidence도 value identity와 physical endpoint summary를 분리해야 한다. ORCA 관점에서
  range, exact graphid slot, terminal-label bloom, compressed entry vector, wide bloom은 index가 제공하는 physical
  property이고, `property_filter_id`는 consumer request identity다. 따라서 `set-directory-filter=N`을
  `/range:N`, `/exact:N`, `/label-bloom:N`, `/compressed:N`, `/value-summary:N`, `/wide-bloom:N`으로 나눠 출력한다.
  Neo4j식 relationship expansion에서도 relationship seek/expand operator의 physical pruning과 value predicate
  selectivity evidence를 분리해 읽는 편이 planner feedback에 유리하다. 이 surface가 있어야 다음 on-disk value
  posting summary가 기존 endpoint summary skip을 재라벨링한 것인지, value-keyed false positive를 실제로 줄인
  것인지 판별할 수 있다.
- `age_adjacency` index는 `(endpoint_id, edge_id, next_vertex_id)` payload index라 build 시점에는 terminal vertex
  property value를 알 수 없다. 따라서 property value 자체나 query별 `property_filter_id`를 index tuple에 저장하는
  설계는 현재 ownership boundary와 맞지 않는다. storage v17은 대신 request identity가 있을 때만 소비하는
  endpoint posting bloom을 directory/main block에 별도 저장한다. 이는 value predicate prefetch 결과가 만든
  matched vertex set과 index-side endpoint posting set의 교차를 더 큰 bloom에서 다시 확인하는 구조다.
- directory value-posting summary는 mixed-label run에서 전역 bloom을 그대로 쓰지 않는다. label-slice bitmap 없이
  전역 endpoint posting bloom을 적용하면 다른 terminal label의 endpoint가 false positive를 만들고, block-level
  range/compressed/posting evidence를 너무 일찍 가릴 수 있다. storage v18은 directory label slot마다 value-posting
  bloom을 같이 저장해 terminal label이 정해진 request만 label-slice membership을 소비한다. ORCA식 provided property로
  보면 v18 summary는 “terminal label slice의 endpoint posting membership”이고, slot이 없는 label은 안전하게 run/block
  residual path로 내려간다.
- VLE planner class는 value-posting summary의 존재만으로 승격하지 않는다. ORCA식 physical property 관점에서
  `value-posting=label-slice|run|none`은 index가 제공할 수 있는 property이고, `/value-posting:N` runtime counter는
  그 property가 특정 request에서 실제 negative evidence를 낸 결과다. 존재 여부만 보고
  `adjacency-composite-value-posting` class를 planned class로 만들면 작은 fixture나 false-positive workload에서
  planner/runtime class mismatch가 생긴다. 따라서 현재 contract는 source evidence를 `VLE Source Cost/Policy`와
  benchmark summary에 노출하고, 실제 counter가 관측된 실행만 runtime pressure/action에서 `keep-value-posting`으로
  분리한다. 다음 단계에서 feedback cache가 terminal label, property source index, value identity, value-posting
  source class를 함께 보관해야 planned class 승격이 의미를 가진다.
- 이번 단계에서 observed value-posting evidence를 backend-local feedback cache와 planner policy input까지 연결했다.
  ORCA식으로 보면 `/value-posting:N`은 provided physical property의 실제 사용 성공이며, 후속 plan은 이를
  `VLE Source Payload Input`의 class/reason으로 읽어 `keep-value-posting` recommendation을 선택한다. 단, 현재
  key는 raw value hash를 EXPLAIN-stable surface로 노출하지 않기 위해 graph/edge/source family 수준에 머문다.
  따라서 이 변경은 value-posting availability promotion이 아니라 observed success promotion이고, 다음 설계는
  value identity를 출력하지 않으면서 cache discriminator에는 포함하는 방식이어야 한다.
- feedback cache key에 terminal label, property source index OID, `property_filter_id`, value-posting source class를
  넣었다. 이는 ORCA의 physical property가 request identity와 함께 비교되는 구조와 맞다. raw value hash는 plan
  surface에 고정하지 않고 backend-local discriminator로만 사용해 regression이 incidental hash encoding을 검증하지
  않게 했다. 같은 property key라도 predicate value가 다르면 후속 plan이 value-posting success를 공유하지 않으므로,
  observed-success promotion이 다른 value predicate로 번지는 false promotion을 막는다.
- ORCA `CPhysical::CReqdColsRequest`는 required column set만 캐시 key로 쓰지 않고 child index와 scalar child index도
  함께 보관한다. 같은 required column set이라도 어느 child/scalar expression boundary에 대한 request인지가 달라지면
  다른 computed property이기 때문이다. AGE VLE feedback cache도 같은 관점에서 source direction, consumer class,
  terminal label/property identity, payload family를 key에 포함해야 하고, entry 안의 payload lifecycle은 마지막
  관측값으로 덮어쓰면 안 된다.
- replay/cache seed/value identity pruning은 서로 배타적인 enum이 아니라 같은 `age_adjacency` payload source가
  제공한 physical lifecycle property들이다. 따라서 cache entry는 scan/replay/seed/value-pruning count를 누적하고,
  policy class/reason은 rank가 더 강한 provided property를 보존해야 한다. 이번 변경은
  `adjacency-composite-value-posting`이 `adjacency-cache-seeded`나 일반 composite prefilter 관측 뒤에 들어와도
  후속 plan에서 살아남도록 payload feedback merge를 ORCA식 property merge에 가깝게 바꾼 것이다.
- VLE에서 `value-posting=start:label-slice`가 보이는 경우 실제 negative evidence는 항상 vertex-set
  `/value-posting:N` counter로만 나타나지 않는다. terminal-property direct output에서는 payload cache가 terminal
  label slice를 먼저 소비해 `cache-filter=label/property` counter로 후보를 줄일 수 있다. 이 경우 source descriptor가
  `value-posting=none`이면 일반 cache/property filter로 남겨야 하지만, active source가 label-slice value-posting을
  제공하면 같은 실행 evidence를 `payload-value-posting-observed`로 승격하는 것이 맞다. 이것은 source availability만
  보고 승격하는 것이 아니라 source descriptor와 runtime pruning counter가 동시에 맞을 때만 provided physical
  property로 인정하는 방식이다.
- replay와 value-posting을 같은 VLE feedback key에서 관측하려면 duplicate outer row를 붙이는 방식은 약하다. planner가
  VLE를 duplicate scan 앞에 배치하면 source rescan이 생기지 않고, result가 0 row이면 duplicate branch는 실행되지
  않는다. mixed-label explicit chain도 fixed-chain VLE rewrite 대상이 아니므로 일반 join tree로 풀린다. 신뢰할 수
  있는 fixture는 같은 label fixed chain을 유지해 parser rewrite를 보장하고, 데이터 fanout에는 다른 terminal label
  decoy를 넣어 runtime label-slice pruning을 만들며, 두 경로가 같은 source vertex로 합류하게 해 payload cache replay를
  발생시키는 구조다. 이 구조가 ORCA식 required/request boundary와 가장 잘 맞는다. query shape는 하나의 VLE request로
  유지되고, replay/cache seed/value-posting은 entry 안에서 merge되는 provided lifecycle property로 검증된다.
- ORCA식 관점에서 planned `adjacency-composite-prefilter`와 runtime `adjacency-composite-value-posting`은 서로 다른
  physical operator family가 아니라 같은 composite source request에서 제공된 property의 강도가 다른 경우다. 따라서
  runtime value-posting success를 class mismatch로 남기면 planner/source handoff 실패처럼 보이는 잘못된 신호가 된다.
  `class-match=true`로 정규화하고 recommendation만 `keep-value-posting`으로 올리는 편이 다음 threshold/headroom 보정에
  더 적합하다.
- 같은 이유로 후속 plan의 directed source reason도 `composite-prefilter`에 머물면 value-posting feedback이 실제
  decision에 들어갔는지 보기 어렵다. source kind를 새 enum으로 늘리지 않고 reason만 `composite-value-posting`으로
  세분화하면 CustomScan descriptor vocabulary는 유지하면서 provided lifecycle property가 policy decision에 소비된
  사실을 raw EXPLAIN에서 확인할 수 있다.
- value-posting feedback은 replay/cache seed가 같이 있을 때만 쓰는 보조 headroom signal로 두면 ORCA식 provided
  property merge와 어긋난다. `adjacency-composite-value-posting` class는 request identity와 value identity가 맞는
  후속 plan에서 이미 observed property이므로, replay/seed count가 0이어도 source profile의 endpoint headroom에 직접
  들어가야 한다. class rank는 pruning identity 보존에 쓰고, headroom selector는 materialization weight를 읽어
  terminal-scalar/path/object consumer별로 다른 aggressive threshold를 적용한다. 이렇게 해야 raw EXPLAIN의
  `headroom=N`이 benchmark threshold 조정의 직접 입력이 된다.
- composite prefilter plan의 missing-vertex evidence도 같은 원칙을 따른다. property source가 후보 terminal vertex를
  찾았지만 traversal source에서 reachable endpoint가 아니면 runtime에는 `missing-vertex` counter가 남을 수 있다. 이때
  dominant source와 suppression source가 planned `age_adjacency`와 일치하면 이는 source handoff 실패가 아니라 composite
  source가 negative endpoint probe를 제공한 결과다. 따라서 runtime class는 `missing-vertex-source`로 새 family를 만들지
  않고 planned `adjacency-composite-prefilter`를 유지해야 threshold table이 잘못된 tuning pressure를 만들지 않는다.
- anonymous VLE edge label은 Cypher 의미상 graph의 모든 edge label을 뜻하지만, AGE storage에서는 `_ag_label_edge`
  default relation이 그 global edge row source다. planner source lookup이 빈 label을 그대로 찾으면 runtime은
  `_ag_label_edge` `age_adjacency`를 쓰는데 plan descriptor는 `global-metadata`로 남아 source mismatch가 된다.
  따라서 source planning은 anonymous label을 `_ag_label_edge` relation으로 해석해야 한다. 다만 endpoint-btree만으로
  dense-local traversal source를 만들면 packed source setup contract와 충돌하므로, anonymous default relation은
  `age_adjacency` index가 실제로 있을 때만 local source candidate로 승격한다.
- `unknown-fanout`은 ORCA식 physical operator family가 아니라 cost evidence 부족의 reason이다. source kind가
  `age_adjacency`로 선택됐으면 provided source family class는 `adjacency-stream`으로 두고,
  `collect-endpoint-stats` recommendation을 통해 stats 수집 필요만 표현하는 것이 source/class mismatch보다 낫다.
- directional family feedback도 같은 required/provided property 관점에서 봐야 한다. `active=both` request가 exact
  `both` feedback 없이 `out`/`in` family feedback을 합쳐 쓸 때, 양방향 source enum을 통째로 rollback하면 반복 empty
  lifecycle evidence를 버린다. 대신 empty completion이 지배적인 방향은 `age_adjacency`와 empty-batch lifecycle을
  유지하고, 반대 productive 방향은 endpoint-btree로 분리하면 source/class contract를 보존하면서 mixed family의
  불필요한 양방향 `age_adjacency` 고정을 줄일 수 있다.

## 되돌린 접근 요약

## 2026-06-05: `adjacency_match` v2 opt-in 구조 제거와 기본 index path 채택

- 기존 `adjacency_match`는 `age.enable_adjacency_match*` GUC 뒤에 있었고, edge variable projection,
  edge property predicate, right property predicate가 있으면 CustomPath 후보를 만들지 않았다. 따라서
  `age_adjacency` 인덱스가 있어도 일반 MATCH에서 실제 효용 범위가 좁았다.
- ORCA의 `CLogicalIndexGet`/`CPhysicalIndexScan`은 index access를 실험용 toggle이 아니라 physical alternative로
  다루고, index descriptor, output column, residual predicate count, order property를 operator contract에 싣는다.
  AGE도 `age_adjacency`가 있으면 guarded one-hop/bound-endpoint shape에서 기본 physical alternative로 두는 것이
  맞다.
- Neo4j의 relationship index seek/expand planning도 edge projection이나 property filter가 있다는 이유로
  index path 자체를 배제하지 않는다. 필요한 payload column을 더 가져오고 residual predicate를 위에서 평가하는
  쪽이 큰 fan-out에서 더 유리하다.
- 이번 변경은 GUC gating과 SRF provider fallback을 제거하고, 정상 edge RTE를 유지한 채
  `AGE Adjacency Match` CustomPath를 기본 후보로 등록한다. 후보 제외 조건은 bound endpoint가 없거나
  `age_id()` accessor에 의존해 setrefs가 불안정한 경우처럼 planner/executor contract가 아직 안정적이지 않은
  경우로 좁힌다.
- executor는 한 endpoint의 payload를 `List`에 모두 materialize하던 방식을 버리고,
  `age_adjacency_visible_payload_scan_begin_key()`/`next()` cursor에서 한 tuple씩 streaming한다. 이 구조가
  큰 fan-out에서 memory growth와 first-row latency를 줄이는 기본 방향이다.
- `AGE Adjacency Match`는 이제 ORCA식 index operator surface에 더 가깝게 planner descriptor를 갖는다. descriptor는
  direction, endpoint key, estimated fanout, edge variable/properties requirement, right label/property residual,
  required payload columns를 executor EXPLAIN에 출력한다. 이 값은 plan 문자열에서 추론하는 debug token이 아니라
  CustomPath private descriptor로 전달된다.
- 다음 구조 후보는 이 descriptor를 cost 입력으로 더 소비하는 것이다. 특히 required payload column이
  `start_id/end_id`만 필요한 shape와 `properties`가 필요한 shape를 분리해 heap visibility/property fetch 비용을
  다르게 잡고, right-node property/label residual을 index-side pruning 또는 label+property composite request로
  끌어올린다. 단순히 GUC를 다시 만들거나 작은 guard로 회귀하지 않는다.
- `AdjacencyMatchPayloadRequest`를 CustomPath 생성 전에 만들도록 바꿔 actual required column vector가 cost와
  descriptor에 들어간다. `start_id,end_id`만 필요한 anonymous edge shape는 `payload=id-only
  required=start_id,end_id`로 계획되고, edge property predicate가 있으면 `properties`가 mask에 포함되어
  `edge-row` 비용을 사용한다. executor의 `custom_scan_tlist`도 같은 request에서 만들어 EXPLAIN의 planned
  descriptor와 actual payload columns가 같은 contract를 보게 했다.
- right label residual은 graphid label bits로 판단할 수 있으므로 vertex heap fetch 없이 adjacency payload cursor
  내부 pruning으로 끌어올렸다. parser가 terminal label id를 candidate에 싣고, executor가
  `AgeAdjacencyVisiblePayloadScan`에 terminal label id를 설정한다. EXPLAIN은
  `right-prune=label:yes/props:deferred`를 출력한다. right property residual은 아직 vertex property fetch/index
  request가 필요하므로 label+property composite lookup 후보로 남겼다.
- `INDEX.md` 조사 기준으로 DDL helper도 source 역할을 드러내도록 재정리했다. property index는
  `create_property_source_index` alias를 추가해 후보 source index임을 명시하고, edge traversal은
  `create_adjacency_index(graph, edge_label, direction)`와 `create_adjacency_indexes(graph, edge_label)`로
  방향별 `(endpoint,id,next)` `age_adjacency` index를 생성한다. 다음 구조 후보는 이 두 helper가 만든
  property source index와 adjacency source index를 planner descriptor에서 하나의 label+property terminal
  request로 결합하는 것이다.

- Generic comparator micro fast path 반복: benchmark 개선 폭이 작아 구조 후보로 전환.
- Sort key를 final output으로 직접 재사용: cast semantic이 바뀔 수 있어 되돌림.
- Lower path가 output `agtype`를 함께 carry: sort width 증가로 악화되어 되돌림.
- Broad terminal node skip: chained `*0..0` path를 깨뜨려 단일 VLE segment + constrained
  start 조건으로 제한.
- Helper-only property predicate rewrite: expression index matching을 깨뜨려 index-aware
  rewrite로 대체.
