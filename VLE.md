# VLE Optimization Log

## Goal

VLE(variable-length edge/path) 실행에서 불필요한 path/list/entity/property
materialization과 graph table refetch를 줄인다. 최종 출력 semantic이 필요한 경우만
`agtype` container를 만들고, count/length/endpoint/property-only consumer는 raw descriptor
또는 direct helper로 처리한다.

## Join Order 연결점

- VLE source policy evidence는 scan-local tuning 값에만 머물면 조인 오더 이득이 작다. start/end
  graphid const, terminal label/property selectivity, `age_adjacency` directory fanout,
  value identity matched count는 VLE를 먼저 펼칠지 selective node/property seek 뒤 ExpandInto처럼
  검증할지를 결정하는 join-order input으로 올려야 한다.
- 다음 VLE planner 확장은 `AGE VLE Stream` descriptor와 별도로 graph pattern component/connector
  descriptor가 같은 source evidence를 읽도록 만드는 것이다. 이때 VLE는 `vle-frontier-anchored`
  또는 `expand-into-verification` 같은 order property를 제공해야 한다.
- fixed `AGE Adjacency Match`에는 먼저 `Adjacency Join Order` surface를 추가했다. VLE도 같은 vocabulary를
  쓰려면 `VLE Source Cost`, terminal label/property source, value identity matched count를
  `vle-frontier-anchored` 또는 `expand-into-verification` property로 접어 graph component descriptor에
  전달해야 한다.
- `AGE VLE Stream`은 이제 `VLE Join Order` line을 출력한다. connector는 기본 `vle-expand`,
  양방향 active source의 `vle-bidirectional-expand`, terminal property composite source의
  `vle-composite-expand`로 나누고, order property는 `query-order`, `vle-frontier-anchored`,
  `index-anchored`로 정규화한다. 이 line은 `VLE Source Cost` 바로 뒤에 배치해 fanout/source evidence를
  조인오더 descriptor와 함께 읽게 한다.
- 이 EXPLAIN surface는 benchmark-only 문자열에 머물지 않는다. planner join hook은 `AGE VLE Stream` CustomPath의
  edge-source descriptor를 읽어, `required_outer`가 반대쪽 relids에 묶인 bound VLE path에서
  `vle-frontier-anchored`/`index-anchored` property를 nested join row estimate와 run cost 조정에 사용한다.
  PostgreSQL core가 hook 뒤 `set_cheapest()`를 실행하므로 이 cost 조정은 이후 join path 선택에 반영된다.
- VLE base CustomPath rows/cost도 marker `Values Scan`의 rows=1을 그대로 쓰지 않는다. planner는 active direction
  fanout, finite upper depth, composite prefilter fanout, materialization weight를 edge-source descriptor에서 읽고,
  `VLE Join Order rows`에 이 estimate를 드러낸다. fanout이 0이면 depth를 곱하지 않고 minimum 1 row로 유지한다.
- 양 endpoint가 모두 제공된 VLE는 `vle-expand-into` 또는 `vle-composite-expand-into` connector로 출력한다. 이때
  composite/index/frontier anchor가 없으면 order property는 `expand-into-verification`이 된다. runtime endpoint는
  surrounding plan에서 공급되는 bound id로 보고, NULL const endpoint는 bound로 보지 않는다.
- 다음 VLE 조인오더 작업은 이 row 보정을 넘어 `ExpandAll`과 `ExpandInto` 후보 비용을 직접 비교하는 것이다.
  Neo4j의 VarExpand Into/All heuristic처럼 두 endpoint가 이미 bound된 shape는 큰 Cartesian product 뒤 검증으로
  밀리지 않게 별도 property를 가져야 한다.
- 문서 탐색 결과 다음 구조는 VLE 단독 descriptor가 아니라 AGE graph join candidate table이다. VLE stream은
  `vle-expand`, `vle-composite-expand`, `vle-expand-into`, `vle-composite-expand-into` connector entry를 제공하고,
  entry마다 active direction fanout, finite depth, terminal label/property prefilter, materialization weight,
  source component rows를 함께 둔다. 두 endpoint가 bound된 shape는 `expand-into-verification`으로 표시하되,
  source rows가 크면 Neo4j `SINGLE_ROW` heuristic처럼 selective node/property seek나 `ExpandAll` 후보와 계속
  비교해야 한다.
- planner에는 `AGEGraphJoinCandidate` 구조와 serializable graph join descriptor를 추가했다. VLE `CustomPath`는
  기존 executor `custom_private` payload와 별도로 planner-only graph join descriptor를 들고, join hook은 이
  descriptor의 order property를 우선 읽는다. executor `CustomScan`에는 기존 VLE descriptor만 전달해 runtime
  contract는 유지한다.
- fixed adjacency 후보도 같은 graph join candidate builder를 통과한다. start/end endpoint가 previous vertex id
  restriction으로 const graphid까지 좁혀지면 executor key는 const로 바뀌고, CustomPath도 더 이상 해당 endpoint
  outer rel에 parameterized되지 않는다. 이는 future partial scan/parallel-safe 후보 판단의 첫 전제다.
- 이 unparameterized 판단은 candidate 원본이 아니라 선택된 `CustomPath`의 `PATH_REQ_OUTER()`와
  `parallel_safe`를 기준으로 graph join descriptor와 executor `CustomScan`에 전달한다. endpoint posting run을
  worker가 나누는 계약은 아직 없으므로 fixed adjacency는 `parallel-safe` metadata 보존까지만 진행하고
  `parallel-aware` partial scan은 별도 구조 변경으로 둔다.
- graph join 후보는 `AGEGraphJoinCandidateTable`에 등록한 뒤 cheapest 후보만 executor-visible descriptor로
  직렬화한다. 지금은 VLE/fixed adjacency 모두 1개 후보를 등록하지만, VLE `ExpandAll`/`ExpandInto`, composite
  property prefilter, fixed adjacency node/property seek를 같은 table에 추가할 준비가 된 상태다.
- candidate 등록은 `Path` 기반 helper로 이동했다. fixed adjacency와 VLE는 선택된 `CustomPath`의 rows/cost와
  physical property를 그대로 candidate로 만들고, parameterized property/node `IndexPath` 또는 bitmap index path는
  join-order walker에서 `index-anchored` 후보로 인식된다. 실제 실행 path가 index scan인 경우에만 index 후보로
  취급하므로 adjacency descriptor와 executor source가 어긋나지 않는다.
- bound endpoint VLE는 candidate table에 `vle-expand-into`/`vle-composite-expand-into` primary 후보와
  `vle-expand`/`vle-composite-expand` fallback 후보를 함께 등록한다. fallback은 같은 `CustomPath` executor로
  가능한 대안이지만 verification penalty를 더해 기존 선택을 유지한다.

## 현재 VLE execution boundary

- Parser/lowering 단계에서 consumer shape를 분류한다.
- `length(p)`, `count(p)`, endpoint-only, property-only consumer는 materialized path를
  요구하지 않는 경로로 낮춘다.
- Full `nodes(p)`, `relationships(p)`, path variable projection, arbitrary list element
  projection은 semantic 보존을 위해 materialization fallback을 유지한다.
- Fixed path arity-specific helper(`_agtype_build_path_label5/7/9` 등)는 제거했고 다시
  늘리지 않는다.
- 최종 path output은 raw descriptor와 `_agtype_build_path_raw` 계열을 사용한다.
- 고정 1-hop VLE terminal label marker가 있는 named terminal vertex는 label table을 다시 join하지 않는다.
  `(s)-[:R*1..1]->(n:N) RETURN n.i` 같은 shape는 hidden raw `edges` target을 통해
  terminal-property output으로 낮춰 terminal vertex `N_pkey` refetch를 제거한다. label-only marker의 AGTYPE integer
  label id는 parser retarget 단계에서 `terminal-property, terminal-label` 10-arg marker로 승격된다.
  `EXPLAIN (VERBOSE)`는 이 shape를 `VLE Shape: terminal-property`, `VLE Arguments: 10`,
  `VLE Composite Source: status=eligible reason=terminal-label-property`로 보여준다.
- terminal-property output descriptor가 알려진 key를 갖고 있으면 CustomScan qual도 같은 scalar slot을
  소비한다. `WHERE n.rare = true RETURN n.rare` 같은 shape는 marker child filter에 남아 있는
  `age_vle_terminal_vertex(edges).rare` access를 CustomScan boundary에서는 `edges = true` 비교로 rewrite해
  terminal vertex object를 predicate 평가 때문에 다시 만들지 않는다. `EXPLAIN (VERBOSE)`의 child
  `Values Scan` filter는 parser marker surface로 계속 보일 수 있지만 marker child는 VLE CustomScan에서 실행되는
  plan node가 아니다.
- 같은 const 또는 runtime-slot predicate와 terminal label, graph property index metadata가 모두 있으면 VLE edge-source
  descriptor가 `predicate=const|runtime-slot prefilter=eligible threshold=N`을 싣고, `age_adjacency` payload scan에
  `AgeAdjacencyMatchTerminalPropertyLookup` prefilter callback을 붙인다. `EXPLAIN ANALYZE`의
  `VLE Payload Runtime`은 `property-prefilter=runs/candidates/filtered`를 출력한다. `vle_index_probe`는
  `n.rare = true`와 `n.rare = s.rare`를 모두 고정해 const/runtime value가 같은 prefilter path에서
  `property-prefilter=1/7/6`을 만드는지 확인한다.
- property source index prefetch가 matching terminal vertex id set을 만들면 `VLE Payload Runtime`은
  `prefetch-matches=N`도 출력한다. 이 field는 0이면 생략하고, prefilter가 실제로 adjacency payload scan에 연결된
  경우에만 terminal label candidate 폭과 property-source matched set 폭을 비교하게 한다. 현재 단계의 의미는
  source fetch 자체를 완전히 composite seek로 바꾼 것이 아니라, 다음 label+value request가 posting/cache fetch
  경계에서 줄여야 할 폭을 regression-visible evidence로 만든 것이다.
- composite terminal request는 scan target에 property source OID, property filter id, prefetched match count를
  보존하고, directory begin-key/known-empty 판단도 같은 composite target helper를 읽는다. directory range miss로
  source를 접은 경우 `set-directory-filter=N` 총량과 별개로 `composite-directory-filter=N`을 출력해
  label+property request가 directory pruning boundary까지 내려갔는지 확인한다.
- 같은 request는 directory entry의 terminal-label candidate estimate와 property source matched count를 비교해
  `composite=request:N/dir-estimate:N`도 출력한다. 이 값은 actual filter count가 아니라 value-summary 상한이므로
  `property-prefilter=runs/candidates/filtered`의 candidate 폭을 바꾸지 않는다.
- compact main block이 sorted matched vertex set과 교차하지 않아 실제 cache fill 전에 skip되면
  `composite=request:N/block-filter:N`으로도 드러난다. `age_adjacency_debug_composite_probe()`는 VLE 없이 같은
  visible payload scan contract를 직접 실행해 block skip, directory skip, value-summary estimate를 한 row에서 비교한다.
- `age_adjacency` v8 directory entry는 endpoint run의 `next_vertex_id` bloom summary도 가진다. matched vertex set이
  directory min/max range 안에 있어도 bloom negative면 main block을 열기 전에 `set-directory-filter` /
  `composite=.../dir-filter:N`으로 접는다. v9 directory entry는 작은 label-local bloom slot도 보관해 global bloom이
  다른 terminal label 때문에 positive인 경우를 줄인다. bloom은 safe-negative summary라 slot이 없거나 false positive가
  있으면 block/posting residual filter가 계속 담당한다.
- v10 directory entry는 distinct terminal vertex가 작은 slot 안에 들어오는 run에 exact `next_vertex_id` vector를
  보관한다. 이 vector가 property matched set과 교차하지 않으면 bloom false positive가 남아도 main block을 열지 않고
  `set-directory-filter` / `composite=.../dir-filter:N`로 접는다. slot을 넘는 run은 overflow marker로 기존
  bloom/block/posting residual path를 유지한다.
- v11 main block header는 256-bit `next_vertex_id` bloom을 보관한다. VLE/fixed MATCH가 property source matched set을
  `AgeAdjacencyVertexSetFilter`로 내려보내면 block bloom이 먼저 교차 여부를 보고, negative면 compact/full posting
  payload를 읽기 전에 `set-block-filter`로 접는다. debug surface는 `set_block_bloom_filter`로 이 skip을 별도 출력한다.
- v12 main block header는 block-local exact `next_vertex_id` vector도 보관한다. distinct terminal vertex가 slot 안에
  들어오면 exact intersection이 bloom보다 먼저 실행되고, debug surface는 `set_block_exact_filter`와
  `set_block_bloom_filter`를 나란히 출력한다.
- v13 main block header는 `min/max next_vertex_id` range summary도 보관한다. property source matched set range가
  block range와 겹치지 않으면 exact/bloom보다 먼저 block을 접고, debug surface는 `set_block_range_filter`로 이
  deterministic skip을 출력한다.
- VLE payload cache key는 terminal property filter identity도 포함한다. filtered payload나 known-empty result는
  `(index_oid, source_vertex_id, terminal_label_id)`만으로 공유하지 않고 property index oid와 predicate value hash가
  같은 경우에만 재사용한다. property prefetch matched set이 비어 있으면 source begin 단계에서
  `empty_suppressed`로 접고, `EXPLAIN ANALYZE`는 `runs=scan:0`, `property-prefilter=1/7/0`,
  `suppressed=out:age-adjacency`, `class=adjacency-composite-prefilter`를 보여준다. 이는 residual filter 성공이 아니라
  source/cache lifecycle이 property predicate를 소비했다는 evidence다.
- payload feedback cache는 scan/replay/seed count, value-posting observed count, strongest payload class를 병합한다.
  같은 terminal filter/profile key 안에서 `adjacency-composite-value-posting` class와 replay/cache seed count가 같이
  쌓이면 `VLE Source Payload Input`은 value-posting class를 보존하더라도 headroom과 empty batch decision은
  replay/seed lifecycle을 함께 소비한다. `tools/vle_benchmark.sql`의 `value-posting-replay-seed` /
  `value-posting-replay` shape는 같은 `ValuePostingEdge` label에서 1-hop value-posting fixture와 3-hop hub replay
  fixture를 같이 만들되, start root는 분리해 `age_adjacency` posting run overflow 없이 replay/cache seed evidence를
  관찰한다.
- source cursor는 `target_path_length`를 보관한다. `age_adjacency` payload cache key의 terminal property filter id는
  final upper-depth expansion에서만 채우므로, terminal property prefilter가 중간 vertex expansion을 잘못 자르지 않는다.
  prefilter 준비도 filter id가 있는 source에서만 실행하므로, final-depth cache identity와 payload source prefilter가
  같은 terminal-only contract를 공유한다. 이는 깊이 있는 terminal property predicate를 VLE source로 내리기 위한 선행
  contract이며, 다음 단계에서는 terminal label marker를 all-depth label chain과 terminal-only endpoint constraint로
  분리해야 한다.
- terminal label descriptor는 `all-depth`와 `endpoint` mode를 갖는다. fixed/exact label-chain marker는 all-depth
  source pruning으로 유지하고, executor context는 endpoint-only label id를 별도 필드로 보관한다. variable range
  endpoint label은 `VLETraversalAcceptance` predicate로 분리되어 emitted path만 거르고, DFS expansion continuation은
  같은 label로 중간 vertex를 자르지 않는다. source/cursor label pruning은 exact fixed range에만 남긴다.
  reverse output `PATHS_TO` shape는 outer terminal label scan이 endpoint label을 보장하므로 step tail acceptance를
  생략한다. terminal property prefilter도 endpoint-only marker와 섞지 않고 all-depth terminal label mode에서만
  source contract로 내려간다.
- terminal property predicate는 residual CustomScan filter가 아니라 VLE descriptor와 executor acceptance/output
  boundary가 소유한다. planner는 terminal property access predicate의 key와 const/runtime-slot value expression을
  edge-source descriptor에 싣고, 같은 predicate qual을 `Custom Scan (AGE VLE Stream)`의 residual filter에서 제거한다.
  executor는 `AgeVLEInput`에서 key/value/null state를 root context로 넘기고, DFS path emission과 zero-bound emission
  전에 terminal property lookup 결과를 `agtype_eq`로 비교한다. 따라서 `EXPLAIN (VERBOSE)`에는 marker child
  `Values Scan` filter가 parser surface로 남을 수 있지만, VLE CustomScan 자체는 해당 predicate를 내부 acceptance로
  소비한다. `vle_index_probe` regression은 residual CustomScan filter가 사라진 뒤에도 `n.rare = true`,
  `n.isolated = true`, `n.rare = s.rare`의 `EXPLAIN ANALYZE` actual rows가 유지되는 것을 고정한다.
- planner source policy도 executor의 terminal property prefilter contract와 같은 label mode를 사용한다. endpoint-only
  terminal label은 emitted endpoint acceptance에서만 보장되므로 `age_adjacency` source prefilter/value-posting key로
  쓰지 않는다. 이런 variable range shape는 `VLE Composite Source: ... reason=endpoint-label-acceptance
  prefilter=ineligible`과 `VLE Composite Fanout: ... planned=metadata-only`를 출력하고, 후속 plan은 1-hop all-depth
  value-posting feedback을 잘못 소비하지 않고 empty/replay/cache-seed lifecycle evidence만 사용한다.
- explicit fixed label-chain collapse는 `terminal-label=N/all-depth` source pruning contract를 유지한다.
  `vle_fixed_label_chain` regression은 `age_adjacency (start_id, id, end_id)` index를 제공한 상태에서
  `fixed-source=out=age-adjacency/in=none`, `cache-seed=eligible`, `class=adjacency-cache-seeded`를 출력해,
  label-chain collapse가 endpoint btree fallback에 묶이지 않는지 고정한다. named terminal property를 붙인
  explicit chain도 마지막 terminal binding을 보존해 `AGE VLE Stream` terminal-property output으로 접힌다.
  marker child plan에서는 VLE가 소비한 terminal property predicate qual을 제거해 direct property output을 다시
  path predicate로 거르지 않는다. runtime에서는 property filter identity를 final vertex expansion에만 붙이고,
  local `age_adjacency` payload candidate가 skeleton vertex entry를 운반해 label-row fallback property lookup을
  사용할 수 있게 했다. regression은 `planned=property-prefilter` plan과 실제 `terminal_i = 5` 결과를 같이 고정한다.
- non-empty matched set도 main cache fill 전에 소비한다. `VLE Payload Runtime`의
  `cache-filter=total/label/property`는 `age_adjacency` visible payload scan이 cache에 넣기 전에 버린 posting 폭이고,
  `vle_index_probe`는 `cache-filter=6/0/6`으로 property prefilter가 7개 terminal label candidate 중 6개를 payload
  cache에 넣지 않았음을 고정한다. 다음 단계의 directory/posting iterator 변경은 이 callback 결과를 더 앞당겨
  block/window traversal 자체를 줄이는 방향이어야 한다.
- terminal property matched set은 이제 generic callback이 아니라 `AgeAdjacencyVertexSetFilter` scan descriptor로
  전달된다. `age_adjacency` main cache fill은 descriptor의 vertex-id hash를 직접 조회하고, VLE runtime은
  `vertex-set=1`을 출력한다. fixed `AGE Adjacency Match`도 같은 descriptor를 쓰므로 VLE/fixed MATCH가 property
  prefetch set handoff contract를 공유한다.
- `AgeAdjacencyVertexSetFilter`는 matched vertex id hash뿐 아니라 min/max range summary도 가진다. `age_adjacency`
  scan은 range 밖 terminal vertex를 hash lookup 전에 버리고 `set-range-filter=N`으로 출력한다. 현재 fixture에서는
  `set-range-filter=6`이라 property matched set range가 payload cache fill 후보 6개를 먼저 제거한다. 아직 main
  block/window traversal 자체를 skip하지는 않지만, descriptor가 hash-only set에서 range-aware set으로 넓어졌다.
- `VLE Composite Source`는 source metadata 상태와 predicate/prefilter 가능 여부만 유지하고, 후보 폭은 별도
  `VLE Composite Fanout` line으로 분리한다. `candidate`는 terminal label slice가 적용된 endpoint fanout이고,
  `composite`는 property prefilter가 실제 planned 상태일 때만 줄인 후보 폭이다. 따라서 property source relation의
  `property-tuples`를 value selectivity로 오해하지 않고, `candidate=7 composite=1 planned=property-prefilter`처럼
  label 후보 축소와 key/value prefilter handoff를 한 줄에서 비교할 수 있다.
- source policy는 이 composite fanout을 endpoint-btree 비용 절감으로 오해하지 않는다. property prefilter는
  `age_adjacency` payload scan에서 실행되므로, planned prefilter가 있으면 `VLE Source Policy`가
  `composite-work=planned(out:1,in:0) reason=out:composite-prefilter class=adjacency-composite-prefilter`를 출력한다.
  `EXPLAIN ANALYZE`의 runtime feedback도 `property-prefilter=1/7/6` counter가 있으면 같은 class/recommendation으로
  정규화해 `VLE Source Plan`의 `class-match=true`를 만든다.
- fixed `AGE Adjacency Match`도 같은 composite vocabulary를 쓴다. `Adjacency Composite Policy`는
  `class=adjacency-composite-prefilter|adjacency-composite-id-cache|adjacency-composite-recheck`와
  `recommendation=keep-property-prefilter|keep-id-cache|keep-recheck`를 출력하고, `ANALYZE`에서는 runtime class와
  `class-match`를 함께 보여준다. 이 line은 property predicate가 있는 composite source에만 출력해 label-only
  pruning line을 늘리지 않는다.
- `AGE VLE Stream`은 marker `Values Scan`을 단순히 대체하고 숨기는 scan이 아니라,
  PostgreSQL `CustomScan.custom_paths/custom_plans/custom_ps` contract에 marker child를 보존한다.
  따라서 `EXPLAIN (VERBOSE)`에서 VLE descriptor 아래에 `-> Values Scan ...` child가 보이고,
  `ANALYZE`에서는 이 marker child가 실행되지 않았다는 사실도 `never executed`로 드러난다.
- property/aggregate final materialization도 VLE의 path/object materialization boundary와 같은 방향으로 본다.
  typed collect와 `array_agg` property rewrite는 lower target에서 cached-property slot을 유지하고 final aggregate
  target에서만 `agtype` materializer를 적용하므로, planner credit은 final materialization weight와 aggregate input
  rows를 함께 소비한다. 작은 regression fixture에서는 base weight를 유지하지만, 큰 aggregate input에서는 row-scaled
  credit이 lower/final split path를 더 강하게 선택하게 한다.
- `AGE Property Projection`은 같은 key path를 여러 physical result type으로 출력할 때 raw properties lookup을
  하나로 공유한다. verbose EXPLAIN의 `Cached Property Summary`는 `heap-lookups`, `reused`, `final-weight`,
  `heap-final-weight`를 출력해 raw lookup boundary와 final materialization boundary가 분리됐는지 보여준다.
- slot-vector map/list aggregate state는 value slot을 `agtype` Datum으로 저장한다. typed scalar slot input은
  transition 시점에 `agtype`으로 정규화해 기존 combine/serialize/final layout을 보존한다. 이는 cached-property
  slot descriptor가 typed field helper를 lower target에 올릴 수 있게 만드는 중간 executor contract이며, 다음 단계의
  slot별 value type header/partial aggregate state 확장과 분리된다.
- slot-vector aggregate partial state는 이제 slot별 original value type OID와 source group vector를 header로
  보존한다. serialize format v4는 `is_map`, `nslots`, `nelems` 뒤에 value type vector와 source group vector를 두고,
  null bitmap/value payload는 계속 typed Datum wire layout을 사용한다. 따라서 큰 aggregate에서 typed slot metadata와
  repeated source metadata를 planner/executor handoff로 올릴 수 있는 자리가 생겼고, partial aggregate combine은 다른
  value type/source group layout을 섞지 않는다.
- `age_array_agg_*_slots_summary` aggregate는 같은 transition/combine/serialize state를 쓰고 final 단계에서
  `shape/slots/rows/typed/agtype/source-groups/reused-slots/payload-weight/final-weight/materialization-weight/types`를 출력한다. 이
  regression-visible state header가
  cached-property slot/index descriptor를 partial aggregate boundary까지 전달하는 다음 handoff의 기준 surface다.
- slot-vector aggregate type mismatch는 더 이상 generic combine/transition error로만 보이지 않는다. executor는
  mismatch slot index, transition/combine phase, expected/actual type name과 OID를 error detail에 출력한다. SQL
  type resolution 때문에 정상 query에서 같은 aggregate slot의 타입을 row마다 바꾸기는 어렵지만, partial state
  header가 깨졌을 때는 descriptor drift를 바로 좁힐 수 있다.
- `age_array_agg_slots_descriptor(variadic any)`는 PostgreSQL이 resolve한 aggregate input type vector를 summary
  aggregate와 같은 `shape/slots/typed/agtype/payload-weight/final-weight/materialization-weight/types` vocabulary로
  출력한다. `cypher_match` regression은 descriptor row와 runtime slot state row를 나란히 고정해 hidden assertion 없이
  planner/resolved type-vector와 aggregate state header를 직접 비교한다.
- aggregate property handoff formatter도 같은 vocabulary를 쓴다. planner DEBUG2 evidence는
  `slots/typed/agtype/index-matched/descriptor-slots/heap-lookups/reused/payload-weight/final-weight/heap-final-weight/reuse-weight/cost-weight/materialization-weight/types`를
  출력하고, index-backed aggregate regression은 descriptor call과 `Index Scan` lower target을 같은 verbose EXPLAIN
  output에 둔다. 이로써 property index surface가 aggregate payload vector와 연결되는지, 같은 key-source lookup을
  aggregate final descriptor가 반복 heap lookup으로 과대 해석하지 않는지 raw plan에서 확인할 수 있다.
- `index-matched` slot은 aggregate narrow path cost에서 별도 row-scaled width credit으로 소비된다. 이는 작은 fixture의
  출력 값을 맞추기 위한 보정이 아니라, 큰 indexed aggregate에서 catalog expression surface가 payload vector와 row
  width를 줄일 수 있다는 planner signal을 비용에 직접 반영하는 것이다.
- index-domain width credit은 matched slot count에 slot당 payload/final/type-vector width weight를 곱한
  `index-width-weight`를 사용한다. planner DEBUG2 handoff descriptor도 이 값을 출력하므로 child `Index Scan` 여부와
  aggregate narrow path credit을 분리해서 읽을 수 있다.
- `tools/aggregate_index_benchmark.sql`은 이 aggregate/index-domain signal 전용 harness다. indexed selective list
  aggregate는 `enable_seqscan=off` 구간에서 expression index-backed lower target을 강제로 관찰하고, typed list
  aggregate baseline은 기본 planner 선택으로 둔다. summary는 descriptor output, aggregate slot-vector output, child
  `Index Scan`, execution time을 함께 출력한다.
- harness는 natural selective shape와 forced index shape를 따로 실행한다. `rows=20` smoke에서는 natural shape가
  seq scan을 유지하고 forced shape만 expression index scan을 출력했다. 큰 row count 측정에서는 이 차이를 threshold
  보정 신호로 본다.
- 후속 smoke에서 `rows=100`은 natural seq scan, `rows=250`과 `rows=500`은 natural expression index scan으로
  전환됐다. benchmark summary는 `row_count`도 출력하므로 같은 harness 실행 결과를 threshold table로 모을 수 있다.
- `threshold_rows` psql variable을 주면 여러 row count를 한 번에 실행한다. harness는 row count별 graph를 따로 만들고
  `row_count/child_uses_index/aggregate_uses_slot_vector/execution_time`을 같은 summary에 출력한다. child scan 선택과
  aggregate slot-vector rewrite는 서로 다른 signal이므로 threshold 보정에서도 분리해 읽는다.
- typed aggregate baseline은 descriptor expression을 따로 받아 `numeric,bigint` type vector를 출력한다. index-backed
  selective aggregate의 `agtype` single-slot descriptor와 비교해 payload vector width 차이를 summary에서 바로 읽는다.
- harness는 실제 descriptor result도 실행해 `slot_count`, typed/agtype count, payload/final/materialization weight,
  `slot_types`, `aggregate_rows`를 출력한다. 이는 EXPLAIN target expression과 runtime descriptor result를 분리해,
  slot-vector rewrite가 켜졌는지와 partial aggregate state width가 어떤 shape인지 같은 threshold table에서 비교하기
  위한 surface다.
- `age_array_agg_*_slots_summary`는 실제 partial state serialize layout의 `serialized-bytes`, `null-bitmap-bytes`,
  `value-bytes`도 출력한다. descriptor row는 rows=0이라 byte fields가 0이고, runtime summary row는 row count와
  slot type별 payload width를 반영한다.
- planner handoff는 runtime value bytes 대신 slot type별 estimated `wire-width`와 `state-width-weight`를 계산해
  aggregate narrow path cost credit과 DEBUG2 descriptor에 반영한다.
- descriptor summary도 `estimated-wire-width`와 `estimated-state-width-weight`를 출력하고 benchmark는 이를
  `slot_estimated_wire_width`, `slot_estimated_state_width_weight`로 파싱한다.
- benchmark는 label table direct slot-state summary도 수집해 `slot_state_value_bytes_per_row`와
  `slot_value_estimate_ratio`를 출력한다. descriptor estimate와 실제 runtime value bytes가 같은 summary row에서
  비교된다.
- current synthetic aggregate benchmark에서는 single-slot `agtype`과 typed `numeric,int8`의 estimate ratio를 1.00으로
  맞췄다. `typed-wide-text-aggregate`는 `wide_text_width=64`에서 `numeric,text` row당 value bytes 84, estimate 32,
  `slot_value_estimate_ratio=2.63`을 출력한다. wide varlena payload는 fixed `text` estimate로 흡수하지 말고
  property width descriptor나 statistics/sample-aware scaling 후보로 다룬다.
- threshold sweep summary는 `last_natural_child_seqscan_rows`, `first_natural_child_index_rows`,
  `first_forced_child_index_rows`를 출력한다. `threshold_rows=20,100` smoke에서는 natural child index 전환점이 아직 없고
  마지막 child seq scan row count가 100으로 나왔다.
- typed map/list property aggregate lowering은 original `array_agg` expression에서 property access signature를 읽는다.
  `age_array_agg_list_slots(agtype_object_field_numeric(...), agtype_object_field_int8(...))`처럼 lower aggregate target이
  typed field helper를 직접 소비하므로, final map/list element materialization 전까지 scalar value type metadata가
  유지된다.
- slot-vector aggregate payload는 이제 가능한 경우 typed Datum으로 저장된다. `int8`/`float8` fixed-width value와
  `numeric`/`text` varlena value는 transition/partial state 안에서 `agtype`으로 선 materialize하지 않고, final map/list
  element를 만들 때 slot별 value type으로 `add_agtype()`을 호출한다. 이는 VLE path/object materialization 지연과 같은
  방향의 aggregate materialization boundary다.
- typed map/list aggregate lowering coverage는 2-field map, 3-field map, list를 모두 포함한다. map2 legacy shape도
  same slot-vector descriptor path로 내려가므로 aggregate arity별 helper를 늘리지 않고 같은 variadic slots aggregate를
  사용한다.
- 다른 AGE CustomScan과의 구분도 명확히 둔다. DML CustomScan은 `lefttree` input stream을 이미 갖고,
  property projection과 adjacency match는 relation-backed scan이므로 child plan이 없다. VLE는 marker stream
  source가 있었는데 planner/executor handoff에서 버려진 케이스라 child explain을 복원하는 것이 맞다.
- `age_adjacency` main run block은 `(heap_tid, edge_id, next_vertex_id)` posting payload와 별도로
  block-level `next_label_id`를 가진다. `AGE Adjacency Match`는 right terminal label constraint를 payload
  emission 뒤 filter로만 두지 않고, visible payload scan이 main block을 cache하기 전에 label-mismatched block을
  건너뛰게 한다. 이 counter는 `Adjacency Payload Runtime: visible=N label-filtered=M emitted=K`로 출력되어
  VLE/adjacency source handoff에서 label pruning이 실제 cache width를 줄였는지 확인할 수 있다.
- terminal property index prefetch set도 visible payload scan의 vertex filter callback으로 내려간다.
  `property-index-prefetch` 모드에서는 terminal vertex id가 matching set에 없으면 heap visibility check와 edge
  properties fetch 전에 posting을 버린다. `Adjacency Payload Runtime`은
  `property-filtered=N`을 별도로 출력하므로 label pruning과 property prefilter가 각각 source/cache 폭을 얼마나
  줄였는지 볼 수 있다.
- terminal property value는 descriptor의 constant만 쓰지 않고 `CustomScan.custom_exprs` value slot으로도 전달된다.
  scan key 평가 시점에 value expression을 평가하고 terminal property lookup의 prefetch cache를 재구성하므로,
  runtime expression value도 adjacency payload source prefilter를 사용할 수 있다.

## 적용된 VLE 최적화

### Terminal endpoint/property

- Terminal-only VLE property cache를 도입했다.
- `head/last(nodes|relationships).field` 단일 property access는 전체 properties object 대신
  `age_vle_*_property_at(vle, index, key)` direct helper로 낮춘다.
- terminal endpoint label/id/property lookup은 batch/cache 경로를 우선 사용한다.
- terminal property batch lookup에서 GCC shadow warning을 제거했다.

### Pathless/indexed consumer

- `length(p)`, `size(nodes(p))`, `size(relationships(p))`, `count(p)` 계열은 path/list
  materialization을 피한다.
- `size(tail(relationships(p)))`, `isEmpty(tail(relationships(p)))`,
  `size(relationships(p)[1..])`, `size(tail(relationships(p))[0..1])`처럼
  list count/empty helper가 direct helper로 낮출 수 있는 tail/reverse/slice wrapper도
  compact VLE consumer로 판정한다. 이 shape는 `age_vle_edge_tail_count`,
  `age_vle_list_is_empty`, `age_vle_list_slice_count`와 `AGE VLE Stream`을 사용하고
  endpoint join/path materialization을 요구하지 않는다.
- `count(last(tail(nodes(p))))`, `count(last(tail(relationships(p))))`,
  `count(startNode(last(tail(relationships(p)))))`,
  `count(endNode(last(tail(relationships(p)))))`, tail-last `label/type/properties`,
  endpoint `label/labels/properties`처럼 count가 entity 존재 여부만 필요한 tail-last
  consumer는 tail-last entity나 field object를 만들지 않고 id 또는 endpoint id helper를
  aggregate 입력으로 쓴다.
- `last(tail(reverse(e)))`, `head(reverse(tail(e)))`, `last(tail(tail(e)))`처럼
  `age_materialize_vle_slice_boundary`로 낮아지는 nested boundary consumer도 count 입력에서는
  entity/field mode를 id mode로 바꿔 edge/node object와 properties/label fetch를 피한다.
- `relationships(p)[0] IN relationships(p)`,
  `relationships(p)[0] = head(relationships(p))`,
  `reverse(relationships(p))[0] = last(relationships(p))`,
  `tail(reverse(relationships(p)))[0] = relationships(p)[0]` 같은 indexed consumer는
  가능한 raw target으로 낮춘다.

### Adjacency/global graph state

- VLE adjacency payload replay/cache를 적용했다.
- visibility map check cache와 edge TID mapping을 사용한다.
- vertex entry를 adjacency frame에 carry해 repeated lookup을 줄인다.
- no-property VLE 또는 count consumer에서는 edge property fetch와 path cache setup을
  생략한다.
- `age_adjacency` payload cache는 방향별 전역 enable flag가 아니라
  `(index_oid, source_vertex_id)` entry 단위로 seed/replay한다. 첫 root의 fan-out이
  작아도 이후 dense source vertex는 자체 payload cache를 만들 수 있고, fan-out이
  1개 이하인 source는 payload 배열을 비워 기존 no-cache 경로를 유지한다.
- fixed-source가 local edge-state, edge label, no-property constraint 조건에서 traversal 방향 전체를
  덮으면 packed adjacency fallback source setup을 생략한다. 이는 빈 packed list를 만든 뒤 skip하는
  것이 아니라 source policy가 이미 covered direction을 보장하는 경우의 lifecycle 자체를 줄이는
  contract다. `VLE Source Runtime`의 `packed` counter는 `scans/candidates/empty-skips/policy-skips`
  순서로 출력해 empty packed fallback과 policy-covered fallback을 구분한다.
- terminal property direct output은 traversal frame이 들고 있는 `vertex_entry`를 우선 사용한다.
  cached property hit, block prefetch, lazy relation cache fallback은
  `get_terminal_property_for_entry()`에서 공통 처리한다.
- terminal property direct output의 planner descriptor는 `ag_graph_index` property source metadata도 읽는다.
  `VLE Terminal Property Source: label=... source=graph-metadata:... provider=... type=... candidates=N`은
  terminal property key가 어떤 graph-local source index 후보와 연결되는지 보여준다. 이 line은 아직 runtime
  frontier pruning이 아니라 다음 `label+property` composite source request의 evidence surface다.
- VLE marker row는 `terminal-property`와 `terminal-label`을 별도 slot으로 표현할 수 있다. property-only direct
  output은 `VLE Composite Source: status=ineligible reason=missing-terminal-label`을 출력해 property source는 있지만
  terminal label slice가 없는 상태를 드러낸다. 10-arg marker가 실제로 쓰이면 이 line은
  `status=eligible reason=terminal-label-property`로 바뀌어 label summary와 property source를 같은 source request에서
  소비할 수 있다. 현재 `property-tuples`는 property source relation/cardinality evidence일 뿐 value predicate
  선택도는 아니다. const/runtime-slot predicate prefilter는 runtime 후보를 줄이지만, cost/selectivity는 아직
  raw reltuples를 value 선택도로 해석하지 않는다. 다음 확장은 typed value 통계가 같은 request contract를 쓰게
  만드는 것이다.
- targeted edge-label load에서 `load_vertex_metadata=false`이면 vertex label list를 채우지
  않는다. `age_adjacency` index payload로 traversal endpoint를 얻는 경로는 vertex
  hashtable을 만들지 않으므로, 전체 `ag_label` vertex scan도 생략한다.
- targeted edge-label load decision은 typed start/end endpoint descriptor를 직접 본다. bound start뿐
  아니라 bound end만 있는 paths-to shape도 reverse traversal root로 사용할 수 있으므로 vertex metadata
  scan 생략 후보가 된다.
- targeted edge label의 outgoing/incoming `age_adjacency` index 존재 여부는 label cache
  generation과 함께 캐시한다. 반복 VLE load에서 같은 edge label relation의 index list를 다시
  열지 않는다.
- graph-name/label-name lookup cache는 단일 entry가 아니라 작은 generation-aware cache를 쓴다.
  여러 graph/edge label을 번갈아 탐색하는 반복 VLE workload에서 graph OID와 edge label relation
  OID lookup miss를 줄인다.
- VLE 실행 쪽 outgoing/incoming `age_adjacency` index OID lookup도 label cache generation 기준
  pair cache를 사용한다. init의 metadata load decision과 cached/local context refresh가 같은
  edge label에 대해 index list를 방향별로 반복 scan하지 않는다.
- edge property metadata는 edge property constraint가 있을 때만 cold edge scan에서 로드한다.
  cached VLE query라도 constraint가 없으면 edge properties count/size/hash를 계산하지 않고,
  path/edge output이 실제로 필요한 edge만 TID 기반 lazy fetch로 properties를 읽는다.
- targeted edge-label fallback VLE는 bound start, no-property-constraint, 8-argument cached path
  shape에서 endpoint graphid만으로 skeletal vertex entry를 만든다. traversal adjacency는 이
  skeleton에 link하고, vertex object/property materialization이 실제로 발생하면 vertex label
  relation의 id index로 TID/properties를 lazy hydrate한다. lazy hydrate 결과는
  `vertex_entry.cached_properties`에 저장해 같은 vertex materialization의 반복 heap fetch를 피한다.
- terminal property batch fetch는 이미 vertex label table tuple을 읽는 시점에 skeletal
  `vertex_entry`의 TID, full properties, terminal scalar property cache도 함께 채운다. 이후 같은
  terminal vertex가 materialized entity로 필요해지면 id-index lazy hydrate를 다시 수행하지 않는다.
- terminal scalar/full property output, direct property result cache, block prefetch, batch
  materialization decision은 `age_vle_terminal_output` module이 맡는다. DFS loop는
  `VLETerminalOutputPolicy`와 `VLETraversalStep`을 넘기고, output module은 result target write와
  terminal property cache/prefetch contract를 처리한다.
- path/list materialization consumer도 local edge-state source를 사용할 수 있다. global graph context에
  vertex/edge object metadata가 없으면 materializer가 graphid label id로 label cache를 찾고, label relation의
  id btree index 또는 table scan으로 해당 row를 읽어 vertex/edge object를 만든다. 이 lookup은
  `VLEMaterializerHandoff` boundary 뒤에 숨겨져 traversal source policy가 object metadata load 여부와 분리된다.
- terminal vertex SQL helper도 `build_vle_vertex_value()`의 label-row fallback을 공유한다. local-source
  terminal-vertex output은 global vertex metadata가 없어도 terminal graphid의 label relation row에서 vertex object를
  만들 수 있으므로, `RETURN n` 같은 terminal-object consumer가 source policy 때문에 global metadata load로 되돌아가지
  않는다.
- `AGE VLE Stream`의 `VLE Materialization` explain은 object/properties materialization source를 출력한다.
  `object-source=global-metadata`는 global graph metadata hit를, `object-source=label-row-fallback`과
  `vertex-source=label-row-fallback`은 local edge-state traversal 뒤 label relation row에서 vertex/object를
  materialize하는 경로를 뜻한다. 따라서 local-source terminal/path output이 hidden assertion이 아니라
  regression-visible plan surface로 검증된다.
- source policy의 endpoint-btree budget은 consumer class별로 다르다. terminal-only consumer는 기존 fanout 2
  budget을 유지하지만, path-materialized consumer는 final object materialization과 label-row fallback 비용을
  고려해 fanout 1 budget을 쓴다. `AGE VLE Stream` explain은 `budget=fanout:N`을 출력하므로 같은 fanout에서도
  terminal-only와 path output의 `age_adjacency` 전환 차이를 regression에서 확인할 수 있다.
- terminal consumer는 다시 `terminal-scalar`와 `terminal-object`로 나뉜다. `n.i` 같은 direct scalar
  terminal-property output은 fanout 2 budget을 유지하고, `RETURN n` 또는 `properties(n)`처럼 vertex/properties
  materialization이 필요한 output은 fanout 1 budget을 사용한다.
- planned empty lifecycle은 planner-only 문자열이 아니라 `AgeVLEInput`, context apply, cached refresh,
  local context까지 전달되는 runtime context contract다. `VLE Source Runtime`의
  `empty-context=eligible|ineligible/depth:N/runs:N match=true|false`는 cached context reuse를 포함한 실제
  context run이 plan descriptor와 같은 empty lifecycle policy를 받았는지 보여준다.
- benchmark summary는 `empty_plan`과 별도로 `empty_context`, `empty_context_depth`,
  `runtime_empty_context_match`를 출력한다. `empty_plan_match=true`이지만 `empty_context_match=false`이면
  source costing 문제가 아니라 input/apply/refresh handoff가 lifecycle descriptor를 잃은 것으로 본다.
- `empty-batch=eligible|ineligible/size:N`은 planned empty lifecycle에서 반복 empty source completion을
  batchable lifecycle property로 올린 것이다. runtime line은
  `empty-batch=.../size:N/capacity:N match=true|false`를 출력하고, frontier empty queue는 이 capacity를
  첫 allocation에 사용한다. 큰 fan-out에서는 empty source key가 여러 번 모이므로 이 capacity가 반복
  `repalloc`을 줄이는 root/source lifecycle batching의 첫 실행 지점이다.
- strong `empty-batch` 후보는 endpoint headroom을 0.35까지 낮춘다. 이는 현재 작은 fixture의 row count를
  최적화하기 위한 값이 아니라, 큰 dataset에서 repeated empty completion이 endpoint-btree 반복 probe보다
  가치 있을 때 `age_adjacency` lifecycle을 더 공격적으로 고르는 threshold다.
- `empty-summary=completion:N/batch:N/saturated:true|false`는 frontier/run/cache/complete evidence 합계가
  planned batch capacity를 채웠는지 보여준다. saturation이 true인 benchmark shape는 다음 threshold/root summary
  보정에서 더 강한 `age_adjacency` lifecycle 근거로 본다.
- root/source completion summary는 `VLETraversalEmptyCompletionSummary`로 별도 descriptor가 되었다.
  `root-empty=completion:N/out:N/in:N/batch:N/saturated-roots:N`은 source skip/cache/frontier/run evidence를
  planned batch capacity와 방향성까지 묶어 보여준다. stream executor는 iterator별 summary를 누적하므로
  benchmark의 shape summary는 root lifecycle saturation 후보를 직접 비교할 수 있다.
- `threshold-feedback=eligible|ineligible/headroom:N/source:S/reason:R`은 `root-empty` summary를 다음 source
  policy input 후보로 정규화한 값이다. `reason=root-empty-saturated`이면 runtime은 0.35 endpoint headroom 후보를
  제안한다. `reason=root-empty-observed`는 현재 planned headroom을 유지하지만 persistent feedback cache의 입력
  후보로 남는다.
- payload cache replay feedback도 regression-visible planner input이다. `vle_payload_replay_policy`는
  `s -> a,b -> x -> y0,y1`처럼 converging source를 만든 뒤 `EXPLAIN ANALYZE`와 후속 `EXPLAIN`을 연속 실행한다.
  첫 실행은 같은 source vertex `x`의 second expansion에서 `payload-cache=runs:.../replay:1/...`을 만들고,
  후속 planning은 `VLE Source Payload Input`의
  `source=runtime-cache ... replay-runs=1 ... replay-percent=25 seed-percent=50 ... reason=payload-replay-ratio-observed`
  를 읽는다.
  이 fixture는 payload seed가 있었다는 사실보다 source-key reuse가 실제로 replay됐다는 증거를 더 강한
  `age_adjacency` lifecycle 근거로 다루기 위한 기준이다.
- payload replay ratio가 strong threshold 이상이면 planner input headroom은 0.25로 낮아진다. 이는 작은 fixture
  row count를 맞추기 위한 값이 아니라, 큰 dataset에서 repeated source reuse가 endpoint-btree 반복 probe보다
  가치 있다는 physical lifecycle signal을 source policy에 직접 반영하는 기준이다.
- strong payload replay threshold는 materialization weight와 consumer class별로 다르다. path-materialized는
  weight 3이며 25% replay ratio부터 headroom 0.18을 사용하고, terminal-object/properties는 weight 2이며
  headroom 0.20을 사용한다. terminal-scalar는 weight 1이며 같은 ratio에서 headroom 0.35와
  `payload-replay-observed` reason을 유지한다. terminal-scalar는 40% replay ratio부터 headroom 0.25와
  `payload-replay-ratio-observed` reason으로 승격한다. scalar-only output은 materialization 절감 여지가 작기
  때문에 25% seed-heavy profile은 보수적으로 두되, branch fan-in이 40%까지 커진 repeated source lifecycle은
  path/object 수준으로 공격적으로 전환한다.
- payload replay feedback은 exact consumer class에만 갇히지 않는다. `path-materialized`와 `terminal-object`는
  같은 materialized payload family cache를 공유하고, planner는 공유 replay ratio를 현재 consumer의
  materialization weight에 맞게 headroom/batch로 다시 해석한다. 따라서 `RETURN p`에서 관측한 replay가 같은
  edge label/direction의 `RETURN n` 또는 `properties(n)` 계획에도 바로 source lifecycle 근거로 들어간다.
- materialized payload family cache도 directional threshold cache와 같은 방향 투영 규칙을 쓴다. `active=both`
  실행에서 얻은 replay evidence는 family `both` entry뿐 아니라 `out`/`in` entry에도 기록되므로, 후속 directed
  terminal-object/properties plan이 undirected path 실행의 replay evidence를 놓치지 않는다.
- benchmark summary도 현재 split EXPLAIN line 구조를 기준으로 읽는다. source runtime summary, source plan,
  source counters, payload runtime, empty evidence, empty lifecycle, runtime feedback을 각각 별도 CTE로 파싱한 뒤
  shape별로 join하므로, `payload-cache`나 `threshold-feedback` 같은 예전 단일-line vocabulary에 묶여
  NULL summary를 만들지 않는다.
- `tools/vle_benchmark.sql`의 `payload-replay-path`, `payload-replay-terminal`, `payload-replay-vertex`,
  `payload-replay-properties`는 같은 source vertex가 여러 branch에서 재사용되는 converging workload다. planner
  summary와 runtime summary를 같은 `query_name`으로 join해 replay ratio, seed ratio, `class_match`를 직접 본다.
  replay input을 읽은 후속 plan은 `adjacency-replay` class와 `keep-age-adjacency` recommendation을 출력하므로
  source selection뿐 아니라 physical lifecycle vocabulary도 regression/benchmark surface에서 맞춰진다.
- replay workload scale은 `replay_branches`와 `replay_leaves`로 조절한다. 값이 0이면 기존 derived profile을
  쓰고, 큰 dataset 측정에서는 branch fan-in과 hub leaf fan-out을 독립적으로 키워 terminal-scalar와
  terminal-object/path-materialized threshold를 비교한다.
- benchmark final join summary는 replay input 값과 resolved replay scale을 함께 출력한다. 이 metadata가
  `payload_input_replay_percent`, `payload_input_reason`, `planner_class`, `runtime_class`와 같은 row에 있으므로
  낮은/높은 replay ratio profile을 별도 post-processing 없이 비교할 수 있다.
- 같은 final join row에는 `rows_returned`, `elapsed_ms`도 포함된다. threshold 조정은 replay ratio만이 아니라
  output cardinality와 materialization elapsed time을 같이 보고 결정한다.
- terminal-scalar threshold 측정에서는 `replay_branches=2, replay_leaves=16`의 25% replay가 headroom 35를
  유지하고, `replay_branches=3, replay_leaves=16`의 40% replay가 headroom 25로 승격되는지를 benchmark smoke로
  확인한다. 이 구간이 seed-heavy observed replay와 repeated source lifecycle replay의 경계다.
- materialized replay 측정에서는 같은 25% replay profile에서 path/terminal-object/properties가
  `payload-replay-ratio-observed`로 승격된다. 이 class는 path container, terminal vertex/properties object,
  label-row fallback 비용을 동반하므로 `AGE VLE Stream` profile에 `weight:N`을 출력한다. 이 weight는 empty
  lifecycle batch sizing에도 들어가 path output의 repeated completion capacity가 terminal-object보다 더 크게
  잡힌다.
- generic `AGE Property Projection`도 final materialization weight를 출력한다. typed scalar property output은
  final `agtype` wrapper를 만들지 않으므로 weight 1이고, `RETURN n.i`처럼 agtype output이 필요한 슬롯은
  wrapper materialization 때문에 weight 2다. 이는 VLE source policy의 weight와 같은 final-output vocabulary를
  property projection boundary로 확장한 것이다.
- property aggregate narrow path도 같은 vocabulary를 쓴다. `array_agg` map/list/single property rewrite와 typed
  collect rewrite는 cached-property slot descriptor의 final materialization weight를 cost credit으로 소비한다.
  계산은 `cypher_property_paths`가 소유하므로 VLE/property projection/aggregate가 slot weight 의미를 공유하고,
  path hook은 child path와 projection boundary 조립에 머문다.
- 같은 map/list aggregate 안에서 동일 property slot이 반복되면 lower target은 기존 slot expression의
  `sortgroupref`를 공유한다. aggregate argument vector는 중복을 그대로 보존하므로 출력 순서와 list/map semantic은
  바뀌지 않지만, child scan/projection의 repeated property lookup 폭은 줄어든다. 이 단계는 slot-vector aggregate
  state 자체를 바꾸기 전 lower/final materialization boundary를 먼저 조이는 변경이다.
- threshold input도 physical lifecycle vocabulary다. `root-empty-saturated` feedback을 소비한 plan은
  `adjacency-empty-batch` / `keep-empty-batch`를 출력하고, replay run이 없는 runtime empty evidence는 planned
  empty lifecycle class와 맞춘다. payload replay run이 있으면 replay가 더 강한 source reuse signal이므로
  `adjacency-replay`가 우선한다.
- `adjacency-replay` runtime class는 planned `adjacency-cache-seeded` lifecycle의 실패가 아니다. cache seed
  가능한 `age_adjacency` source에서 replay가 발생하면 planned empty/cache lifecycle이 더 강한 runtime
  property로 만족된 것이므로 `VLE Source Runtime`은 `class-match=true`를 출력한다. pressure도
  `class-mismatch/tune-source-policy`가 아니라
  `adjacency-payload-replay/keep-payload-replay`로 남겨 다음 조정이 source rollback이 아니라 replay lifecycle
  유지와 threshold 보정임을 드러낸다.
- `VLE Source Runtime`은 더 이상 모든 source/index counter를 한 줄에 싣지 않는다. summary line은 observed
  runtime의 dominant source, class, pressure/action만 보여준다. planned source와 class match는
  `VLE Source Plan`으로 분리하고, source scan density와 index counter는 `VLE Source Counters`, payload replay는
  `VLE Payload Runtime`, empty source evidence와 lifecycle contract는 `VLE Empty Evidence`/`VLE Empty Lifecycle`,
  threshold와 recommendation은 `VLE Runtime Feedback`으로 나눈다. 이는 EXPLAIN 폭을 줄이면서도 hidden assertion
  없이 raw plan surface에 근거를 남기기 위한 출력 구조다.
- planner source도 같은 split surface를 따른다. `VLE Edge Source`는 fixed source, candidate index, state만
  요약하고, cost/statistics는 `VLE Source Cost`, consumer/headroom/lifecycle profile은 `VLE Source Profile`,
  backend-local threshold feedback input은 `VLE Source Threshold Input`, payload replay feedback input은
  `VLE Source Payload Input`, final source decision은 `VLE Source Policy`로 분리한다.
- `age_adjacency` directory entry는 endpoint run의 terminal `next_label_id` min/max summary를 가진다. terminal
  label constraint가 이 range 밖이면 visible payload scan은 main run block을 열지 않고 endpoint run 전체를
  label-filtered/cache-filtered로 처리한다. `AGE Adjacency Match` EXPLAIN은 이를
  `Adjacency Payload Runtime: ... directory-label=N ...`으로 출력한다. 이는 block-level compact label skip보다
  한 단계 위의 index data pruning이며, `(:N {i:0})-[:R]->(:Z)` regression에서 `directory-label=3`,
  `visible=0`으로 고정한다.
- 이 directory summary는 `age_adjacency` on-disk layout v5로 취급한다. `age_adjacency_debug_stats()`는
  `index_version`을 반환하고, smoke regression은 fresh-built index가 `5:2`처럼 version과 directory run count를
  함께 출력하게 해 layout identity가 숨지 않게 한다.
- 같은 summary는 `age_adjacency_visible_payload_scan_key_known_empty()`도 소비한다. delta page가 없는 fresh
  index에서 endpoint run은 있지만 terminal label range 밖이면 key는 label-aware known-empty로 취급된다.
  `age_adjacency_debug_key_known_empty()` regression은 label 1 range hit와 label 3 range miss를
  `false:true`로 고정한다. VLE frontier empty queue는 known-empty mark를 queue 시점에 payload cache에도 즉시
  반영하고, batch flush는 counter와 중복 정리만 담당한다.

### Raw materialization

- VLE path/node/edge output은 raw builder를 사용한다.
- Fixed path materialization은 arity-specific helper 대신 raw descriptor 기반으로 정리했다.
- 단일 edge path와 counted property access는 불필요한 materialization을 피한다.
- fixed path/VLE materialization regression은 hidden assertion 대신 normalized
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력으로 유지한다. graph OID literal만 정규화하고,
  raw path builder/direct property helper/VLE edge materializer surface는 expected에 직접 남긴다.

## 일반 property projection과 VLE의 접점

- VLE boundary property direct helper는 일반 property projection 최적화와 같은 목표를
  공유한다: 최종 출력 직전까지 `agtype` wrapper를 만들지 않는다.
- `last(relationships(p)).payload.a`, `last(nodes(p)).payload.a` 같은 VLE boundary
  property GROUP/DISTINCT key는 `age_vle_*_properties_at(...)` 결과에 variadic
  `agtype_access_operator()`를 씌우지 않고 `agtype_object_field_agtype()` chain으로 낮춘다.
  이 경계는 VLE binary path container가 아니라 이미 추출한 properties object를 다루므로
  direct helper surface가 맞다.
- 일반 MATCH property predicate는 index-aware rewrite를 사용한다.
- expression index가 있으면 기존 `agtype_access_operator(properties, key)` surface를
  보존하고, 없으면 direct helper로 낮춘다.
- 이 rewrite/canonicalization 판단은 VLE module이 아니라 `cypher_property_paths`의 property
  descriptor API가 담당한다. VLE boundary는 terminal/output property access를 같은 final scalar
  descriptor에 맞추는 쪽으로 확장한다.
- simple/ordered MATCH property projection의 lower/final target planning도 같은 module로 이동했다.
  VLE boundary property projection은 별도 parser를 늘리지 말고 이 cached-property descriptor와
  final materialization contract를 재사용한다.
- 다음 구조 후보는 VLE/property/aggregate 사이의 final output scalar descriptor를 공유하는
  것이다.

## CustomScan iterator boundary

- `AGE VLE Stream` CustomScan은 더 이상 `ExecMakeFunctionResultSet()`으로 `age_vle` SRF를 감싸지
  않는다.
- SQL-visible `age_vle(...)` overload는 제거했다. Cypher lowering은 `vle_internal` pseudo call의
  argument를 stable marker `VALUES` RTE로 낮추고, optimizer는 marker row를
  `AGE VLE Stream` CustomScan으로 바꾼다. 직접 `age_vle_internal(...)` SQL regression은 제거했고,
  optimizer의 Function RTE fallback과 `age_vle_internal(...)` SQL surface/C SRF wrapper도 제거했다.
- marker row는 `edges`, `__age_vle_marker`, graph, start/end/edge/range/direction, optional
  grammar-node/terminal-property slot 순서다. `edges`를 첫 column으로 둬 CustomScan executor의
  result slot과 parent `Var(edges)` attno를 맞춘다.
- terminal-property output은 parser가 marker row에 terminal-property key slot을 추가해 9-slot
  descriptor로 retarget한다. 더 이상 `age_vle_internal()` 9-arg SQL function OID를 lookup하지 않는다.
- marker row의 `edges` placeholder는 `agtype_volatile_wrapper(NULL::agtype)`다. plain NULL const를
  쓰면 compact `length(p)`/`relationships(p)` consumer에서 PostgreSQL이 CustomScan replacement 전에
  single-row values를 NULL로 접어 버릴 수 있기 때문이다.
- CustomScan 실행 루프는 `cypher_vle_stream.c`, descriptor parsing과 explain formatting은
  `cypher_vle_stream_descriptor.c`로 나눴다. VLE input semantic accessor는 `age_vle_input.c`로
  분리해 `age_vle.c`의 traversal/cache/materializer 책임과 argument slot parsing 책임을 나눴다.
  다음 typed descriptor 확장은 실행 루프 파일에 helper를 계속 쌓지 않고 descriptor/input 모듈에
  추가한다.
- `age_adjacency` payload cache key/entry allocation, payload array append/discard/free lifecycle은
  `age_vle_adjacency_cache.c`로 분리했다. `age_vle.c`에는 payload를 traversal frame으로 변환하는
  DFS callback만 남겨, 다음 traversal state/materializer 분리에서 cache contract를 별도 내부 API로
  재사용할 수 있게 했다.
- VLE materializer object cache의 graph별 function-context cache, vertex/edge object cache, typed
  vertex/edge cache는 `age_vle_materializer_cache.c`로 분리했다. `age_vle.c`는 vertex/edge semantic
  object를 만드는 builder callback만 제공하므로, materializer cache layout은 traversal/path building
  코드와 분리된 내부 API가 되었다.
- materializer cache lookup은 `VLEMaterializerHandoff`를 받는다. path/list materialization과 indexed
  typed vertex/edge materialization은 graph context, relation cache, build callback을 낱개 인자로
  흩어 보내지 않고 같은 handoff descriptor를 넘긴다. 이 경계는 다음 단계에서 traversal root/source,
  output requirement, terminal hydrate 후보를 materializer cache contract에 붙일 수 있는 기준점이다.
- materializer object builder는 global metadata hit를 우선 사용하고, miss가 나면 label relation row fallback을
  사용한다. `find_edge_entry()`는 optional edge metadata lookup contract를 제공하고, assert가 필요한 기존
  caller는 `get_edge_entry()`를 유지한다. index scan slot은 PostgreSQL heapam contract에 맞춰
  `table_slot_create()`로 만든다.
- `VLE_path_container`는 graphid array 외에 traversal root id, root validity, materializer output
  requirement를 보관한다. path output, terminal-only output, zero-bound output이 container 생성 시점에
  이 metadata를 채우므로 indexed materializer helper는 호출된 SQL wrapper 종류와 별개로 같은
  root/source descriptor를 복원할 수 있다.
- container는 materializer candidate vertex도 보관한다. normal path는 Cypher 출력 terminal vertex,
  reversed path는 traversal root이자 Cypher 출력 terminal vertex, terminal-only/zero-bound output은
  단일 출력 vertex를 candidate로 둔다.
- DFS candidate frame stack과 path/edge-index/vertex stack push/pop/top helpers는
  `age_vle_traversal.c`로 분리했다. `age_vle.c`의 DFS 함수들은 아직 traversal policy와 edge-state
  flag를 직접 다루지만, stack mutation layout은 별도 내부 API가 되어 다음 edge-state/traversal
  state 분리의 기준점이 생겼다.
- traversal frame은 더 이상 `source_vertex_id`를 들고 다니지 않는다. DFS 확장 source는
  root/consume state에서 이미 결정되므로 candidate frame에는 다음 edge, dense edge index, next vertex
  id, terminal output handoff에 필요한 `vertex_entry`만 남긴다.
- local edge-state traversal에서는 frame의 `vertex_entry` carry 여부를 output policy로 결정한다.
  terminal vertex/property/properties output은 indexed property helper와 terminal emit이 entry를 다시
  사용하므로 frame에 보존하고, path/container output처럼 terminal entity를 직접 emit하지 않는 경로는
  이 payload를 생략할 수 있는 contract로 둔다.
- dense edge-state flags와 local edge id -> dense index hash도 `VLELocalEdgeState`로 묶어
  `age_vle_traversal.c`가 init/free/get-or-create/flag lookup을 맡는다. `age_vle.c`는 edge-state flag
  bit 의미와 DFS policy만 다루며, local edge-state storage layout은 traversal 모듈로 이동했다.
- DFS 함수들의 공통 backtrack, used-edge marking, path stack push는
  `age_vle_consume_next_frame()`으로 `age_vle_traversal.c`에 낮췄다. `age_vle.c` wrapper는 cached
  terminal vertex handoff만 처리하고, `paths-between`, `paths-from`, terminal-property 전용 DFS는
  path acceptance 조건과 terminal output 처리만 별도로 남긴다.
- DFS next-vertex expansion 조건은 `extend_dfs_from_vertex_if_needed()`로 통합했고, found terminal
  result cache handoff는 `VLETerminalOutputPolicy`와 `cache_terminal_output_result()`로 묶었다.
  일반 DFS 함수는 acceptance 조건만 판단하고, terminal-property 전용 DFS는 같은 output policy로 direct
  scalar cache를 선택한다.
- length/end/end-vertex acceptance 조건은 `VLETraversalAcceptance` descriptor와
  `age_vle_accepts_path()`로 묶어 `age_vle_traversal.c`에 낮췄다. `age_vle.c`는
  `VLE_local_context`에서 lower/upper/terminal endpoint descriptor를 구성하고, DFS는 기존처럼 결과를
  반환하기 전에 다음 depth candidate를 먼저 push해 successive SRF call에서 deeper path 후보를 잃지
  않는다.
- terminal-property output은 `VLETerminalOutputPolicy`로 묶었다. DFS는 accepted terminal을 찾은 뒤
  direct-property 가능 여부와 1-byte key fast path를 같은 policy로 판단하고, iterator result handoff도
  같은 policy를 사용한다. 이 단계에서는 batch materialization array와 label scan은 유지하되,
  direct DFS path와 final emit 분기를 같은 output descriptor에 맞췄다.
- iterator materialization 선택은 `age_vle_iterator_materialization.c`로 분리했다. `age_vle.c`의
  iterator loop는 `AgeVLEOutputRequirement`, terminal property emit 여부, zero-bound 여부를
  `VLEIteratorMaterialization` descriptor로 낮춘 뒤 그 descriptor만 소비한다. terminal-only requirement
  판정도 같은 모듈을 사용하므로 traversal frame carry policy와 final output materialization이 같은
  output semantic을 공유한다.
- `VLEIteratorMaterialization`은 container output kind도 보관한다. path, reversed path, zero-bound path,
  terminal vertex, zero-bound terminal vertex 선택은 descriptor 모듈이 결정하고, `age_vle.c`는 이미
  정해진 kind에 맞는 builder만 실행한다. 따라서 reverse/zero/terminal 조합 판단이 iterator result
  emission 함수에 다시 흩어지지 않는다.
- container builder 실행 입력은 `VLEContainerBuildInput`으로 좁혔다. path/terminal/zero container
  builders는 `VLE_local_context` 전체 대신 graph oid, start vertex id, reverse-output flag,
  path stack, path vertex stack만 받는다. materializer metadata setter도 graph oid를 직접 받아
  builder 실행부가 traversal/cache/context 내부 필드에 덜 묶이게 했다.
- traversal setup의 endpoint validity, initial root id, range bound, direction은 `VLETraversalShape`가
  보관한다. metadata load policy, root descriptor, local context apply는 이 shape를 공유하므로
  root selection과 range/direction apply가 setup raw field 나열에 다시 의존하지 않는다.
- `age_vle_setup.c`는 `VLETraversalSetup` 생성, graph/label lookup cache, edge source index discovery,
  global graph load helper를 맡는다. `age_vle.c`는 setup descriptor를 받아 `VLE_local_context`에 적용하고,
  cached refresh/root mutation을 처리한다. 남은 큰 경계는 root/apply context handoff를 더 좁혀 context
  내부 필드 mutation을 별도 모듈로 옮기는 것이다.
- `VLETraversalContextApply`는 새 context base apply 입력을 graph identity, graph context, cache flag,
  local edge-state policy, edge constraint, source index, range, terminal-property prefetch budget으로 묶는다.
  root descriptor는 이 base field 적용 뒤 fan-out comparison/source layout을 계산하므로 같은 apply sequence의
  다음 단계로 유지한다.
- cached context refresh는 `VLETraversalRefreshApply`를 만든 뒤 root descriptor apply와 terminal-property
  direct-result scratch reset을 실행한다. refresh 실패 여부는 next-vertex exhaustion으로만 결정되고,
  성공한 refresh는 descriptor가 가진 root/source layout과 prefetch budget을 context에 적용한다.
- `VLETraversalSourceLayoutInput`은 source indexes, local edge-state 여부, label constraint 여부, property
  constraint 여부를 담는다. setup root, cached refresh, cached index refresh는 이 descriptor를 통해
  outgoing/incoming source kind를 계산한다.
- `VLETraversalRootSelectionInput`은 graph context, edge label oid, empty range, zero-only range를 담는다.
  setup root 생성은 이 descriptor로 start/end fan-out을 비교해 cheaper traversal root를 선택한다.
- `age_vle_root.c`는 setup root 생성, cached refresh root 갱신, source layout 계산, initial fan-out count를
  담당한다. `age_vle.c`는 source/root selection input을 만들고 root descriptor를 context에 apply한다.
- `VLETraversalRootApplyInput`은 root selection, source layout input, current root descriptor를 묶는다.
  setup root 생성과 cached refresh는 이 handoff에서 필요한 부분을 root module에 전달한다.
- `VLETraversalSetupApply`는 새 context base apply descriptor, setup root descriptor, terminal output policy,
  edge-state init descriptor를 함께 들고, context mutation 전 `VLETraversalContextApply`와 root descriptor에서
  root selection/source layout/edge-state capacity를 계산한다.
- `VLETraversalActivationApply`는 source index refresh, traversal state init, initial stack load, dirty mark를
  하나의 reload descriptor로 묶는다. 새 context activation, cached context reuse, next start-vertex reload가
  같은 helper를 통해 `load_initial_dfs_stacks()`를 호출하므로 stack reload lifecycle이 한 경계로 모였다.
- `VLETraversalCachedReuseApply`는 cached refresh root apply와 activation descriptor를 함께 들고, SRF memory
  context 전환 안에서 source index refresh와 initial stack reload를 실행한다.
- `age_vle_context.h`는 `VLE_local_context`와 setup/apply/refresh/activation descriptor type을 보관한다.
  context layout과 mutation descriptor ownership이 한 internal header로 이동했으므로, 다음 setup/apply
  module 분리는 `age_vle.c` local typedef를 다시 노출하지 않고 진행할 수 있다.
- `age_vle_apply.c`는 context/setup/refresh/activation apply와 source layout refresh를 담당한다.
  `VLETraversalApplyOps`는 source index refresh와 initial stack load callback만 묶는다. edge property
  constraint cache는 context base apply 내부 lifecycle로 이동해 apply module이 별도 callback을 요구하지 않는다.
- `age_vle_context.c`는 `VLE_local_context` setter와 field-derived descriptor builder를 담당한다. base/root/output
  apply, edge-state init, terminal direct-result reset, source/root input builder가 이 module로 이동해
  `age_vle_apply.c`는 context raw field에 직접 접근하지 않는다.
- `VLEContextOutputState`는 output requirement, terminal emit flags, frame vertex carry flag, terminal property
  key descriptor, direct-result scratch, block prefetch state, terminal-property batch state를 묶는다. terminal
  output lifecycle이 flat `VLE_local_context` field 목록에서 독립된 substate로 이동했다.
- terminal-property lookup/policy descriptor도 context API가 초기화한다. DFS/output path는
  `VLETerminalOutputPolicy`와 `VLETerminalPropertyLookup`을 context에서 받아 사용하고, direct terminal result
  scratch도 context get/set/clear API를 통해 갱신한다. key descriptor, char-fast metadata, relation cache,
  prefetched block hash, prefetch budget이 output substate에 묶이므로 caller가 `VLEContextOutputState` layout을
  직접 조립하지 않는다.
- terminal-property batch materialization state는 `VLETerminalPropertyBatchState`로 묶었다. terminal
  id list, fetched property Datum array, NULL bitmap, emit cursor, materialized flag가 한 lifecycle
  helper로 reset되므로 cached VLE context 재사용과 early cleanup에서 batch state contract가 명확해졌다.
  label별 batch scan과 tuple property extraction helper도 `VLE_local_context` 전체가 아니라
  `VLETerminalPropertyBatchFetch` descriptor와 batch state를 받도록 좁혔다. 다음 파일 분리 시 넘겨야 할
  graph/key/cache contract가 단일 descriptor surface로 정리됐다. batch fetch, label scan, tuple property
  extraction 구현은 `age_vle_terminal_property_batch.c`로 분리했고, `age_vle.c`는 batch descriptor 초기화와
  traversal-driven materialization orchestration만 맡는다.
- terminal-property batch state의 id append, result/null allocation, fetch, emit cursor, materialized flag도
  context API로 낮췄다. batch materialization caller는 현재 terminal vertex id를 context에서 받고,
  `age_vle_context_next_terminal_property_batch_result()`로 결과를 꺼내므로 `VLEContextOutputState`의 array
  layout과 traversal path stack을 직접 열지 않는다.
- terminal-property batch fetch descriptor와 `VLEContainerBuildInput`도 context API에서 조립한다. output
  materialization caller는 graph oid, start vertex id, reverse-output flag, path stack, path vertex stack,
  terminal property key를 직접 읽지 않고 context-provided descriptor를 container/batch module에 넘긴다.
- executor는 `FuncExpr` 인자를 직접 평가해 `FunctionCallInfo`를 채우고, `AgeVLEIterator`를 만들어
  traversal result를 한 row씩 꺼낸다.
- planner는 `FuncExpr` 전체를 executor로 넘기지 않고 argument expression list와
  `nargs/const/range/direction` descriptor를 CustomScan plan에 담는다. 이로써 executor는 SQL function
  identity가 아니라 CustomScan descriptor에서 VLE call surface를 복원한다.
- executor state는 argument list를 다시 semantic slot array로 풀어 graph/start/end/edge/range/direction,
  grammar-node, optional terminal-property key를 구분한다. Cypher marker stream과 terminal property
  shape만 descriptor contract로 받는다.
- CustomScan executor는 slot 평가 결과를 `AgeVLEInput`에 채우고
  `age_vle_iterator_create_from_input()`으로 iterator를 만든다. iterator creation은 더 이상
  `FunctionCallInfo`를 재구성하지 않고 `AgeVLEInput` slot을 traversal context builder에 직접 넘긴다.
- `Const` argument slot은 `ExecInitExpr`/`ExecEvalExpr`를 거치지 않고 plan-time Datum/null descriptor로
  `AgeVLEInput`에 복사한다. 동적 start/end vertex slot만 per-rescan expression evaluation을 유지한다.
- const slot 판정도 executor가 expression tree를 직접 분류하지 않고 planner가 만든 argument별 flag
  descriptor를 따른다. 이 단계부터 `AGE VLE Stream`은 함수 호출 expression을 감싼 executor가 아니라
  planner-provided scan descriptor를 실행하는 쪽으로 이동한다.
- graph name은 planner가 agtype string const에서 graph descriptor로 추출해 `custom_private`에 싣는다.
  executor는 이 descriptor를 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 known graph slot이면
  graph lookup/load 시작점에서 agtype argument를 다시 열지 않는다.
- edge prototype은 planner가 agtype edge const에서 label name, properties object, property count를
  descriptor로 추출해 `custom_private`에 싣는다. executor는 이 descriptor를 `AgeVLEInput`에 복사하고,
  `build_local_vle_context()`는 known edge slot이면 edge label lookup과 property constraint setup 전에
  edge agtype 전체를 다시 열지 않는다. dynamic edge slot fallback만 기존 parser를 공유한다.
- start/end endpoint는 executor가 expression evaluation 직후 vertex/id agtype을 graphid로 정규화해
  `AgeVLEInput`에 보관한다. `build_local_vle_context()`는 cached context refresh와 새 context creation
  모두에서 typed endpoint만 읽고 start/end agtype slot을 다시 열지 않는다.
- cached VLE context refresh의 endpoint handoff는 `VLEContextRefreshInput`으로 묶었다. refresh helper는
  start/end validity와 graphid를 받아 cached context의 traversal root, reverse paths-to root,
  reverse-output swap, terminal-property scratch state를 한 경계에서 갱신한다.
- 새 context와 cached refresh의 traversal root 선택은 `VLETraversalRootDescriptor`를 거친다.
  descriptor는 path function, start/end graphid, direction, next vertex cursor, reverse paths-to,
  reverse-output flag를 보관한다. setup path와 cached refresh path가 같은 root apply helper를 사용하므로
  root swap/reverse semantics가 한 surface로 모였다.
- traversal candidate source layout은 `VLETraversalSourceLayout`으로 묶었다. layout은
  outgoing/incoming 방향별 `VLETraversalDirectedSource`를 담고, root apply/index refresh 시점에
  age_adjacency 또는 endpoint-btree source kind와 index OID를 고정한다. vertex metadata가 없는 skeletal
  endpoint expansion과 normal vertex expansion이 같은 fixed source contract를 쓴다.
- candidate match result도 context/traversal handoff vocabulary에 들어갔다. source별
  `age_adjacency`, packed adjacency, endpoint-btree path는 candidate와 `VLECandidateMatchResult`를 만들고,
  `age_vle_context_apply_candidate_match_result()`가 traversal needs-check, property semantic match, match bit
  mark를 처리한다. source module은 더 이상 match bit policy wrapper를 호출하지 않고, match용 edge fetch
  필요 여부와 candidate construction에 집중한다.
- `age_adjacency` payload source와 packed adjacency source는 opaque context-owned lifecycle을 사용한다.
  candidate source는 payload cache entry, replay cursor, pending payload, packed out/in/self list, suppression
  flag, iterator index를 보지 않고 begin/next/end API를 통해 payload 또는 packed adjacency entry만 받는다.
  따라서 cache seed/discard, packed suppression/empty counter, list iteration policy는 context boundary 안에
  남고 candidate source는 validation과 traversal handoff에 집중한다.
- DFS frame consume 결과는 `VLETraversalStep`으로 묶었다. traversal module은 vertex id, optional
  `vertex_entry`, path length를 하나의 step으로 반환하고, context API가 consumed vertex entry cache를 갱신한다.
  DFS search loop는 terminal acceptance, upper-bound skip, edge expansion, terminal output cache를 같은
  step descriptor로 수행하므로 frame payload와 terminal output handoff가 같은 vocabulary를 쓴다.
- iterator output target은 `VLEIteratorOutputTarget`으로 묶었다. terminal scalar, terminal full properties,
  materialized terminal-property batch, path/container output은 raw `Datum *result`/`bool *is_null` pair를 직접
  쓰지 않고 같은 target setter를 사용한다. 이 경계는 다음 단계에서 terminal property builder와
  path/container final dispatch를 별도 output module로 옮길 때 shared output handoff가 된다.
- materialization kind dispatch는 `age_vle_iterator_emit_result()`로 낮췄다. iterator materialization module이
  terminal scalar, terminal full properties, path/container kind를 선택하고, `age_vle.c`는 terminal builder와
  container builder callback만 제공한다. 따라서 final output dispatch는 `age_vle.c`의 DFS/search loop에서
  분리되고, 남은 static dependency는 terminal property builder 구현으로 좁혀졌다.
- source layout은 expansion마다 다시 만들지 않는다. `VLETraversalRootDescriptor`가 runtime source layout을
  소유하고 root apply가 `VLE_local_context`의 current layout을 갱신한다. cached index refresh는 같은 source
  layout builder를 써서 current layout만 갱신하므로 expansion 시점에는 이미 확정된
  direction/source-kind/index contract를 읽고 raw OID fallback 조합을 다시 수행하지 않는다.
- `select_vle_traversal_source_layout()`은 planner의 `AGE VLE Stream` edge-source descriptor와 runtime
  root layout이 공유하는 source selector다. `age_adjacency`/endpoint-btree candidate availability,
  local edge-state 여부, upper bound, property constraint 여부를 받아 outgoing/incoming source kind를
  정하고, runtime root builder만 index OID를 붙인다.
- `age_vle_source_cost`는 reltuples/statistics 기반 endpoint fanout estimate를 공통화한다. optimizer의
  adjacency path costing과 runtime local edge-state capacity estimate가 같은 source cost evidence를 읽고,
  runtime counter feedback을 붙일 다음 boundary를 제공한다.
- `VLE Source Runtime`은 raw counter 뒤에 `empty=age_adjacency:N/endpoint-btree:N`,
  `empty-suppressed=age_adjacency:N/out:N/in:N`,
  `feedback=dominant=... yield=... replay=... push=...`를 출력한다. 이 feedback은
  `age_vle_source_cost`에서 포맷하며, endpoint-btree와 `age_adjacency` 선택의 runtime evidence를 expected
  output에 직접 남긴다. empty counter는 productive scan density와 exhausted/empty root probe를 분리하므로
  `pressure=adjacency-empty-probe action=suppress-empty-source` 또는
  `pressure=adjacency-empty-suppressed action=observe-suppression:out|in|both` 같은 source lifecycle 상태를 raw
  plan에서 바로 확인할 수 있다.
- adjacency/endpoint index OID는 `VLETraversalSourceIndexes`로 묶었다. setup은 label lookup 뒤 이 descriptor를
  채우고, cached index refresh도 같은 helper로 descriptor를 갱신한다. load policy와 source layout builder는
  네 개의 raw OID 필드를 직접 조합하지 않고 같은 index handoff를 읽는다.
- `VLETraversalSourcePolicy`는 `VLETraversalSourceIndexes`와 `VLETraversalLoadPolicy`를 함께 보관한다.
  local-index candidate 여부와 metadata load decision이 같은 source availability descriptor를 보므로,
  setup/apply 경계가 source index와 load boolean을 별도 field 묶음으로 재조합하지 않는다.
- `VLETraversalGraphLoad`는 graph name/oid, edge label oid, source policy를 한 descriptor로 묶는다.
  global graph context load는 이 descriptor를 소비하므로, setup이 만든 graph/source/load contract가
  `manage_GRAPH_global_contexts_len_for_vle()` argument list로 다시 흩어지지 않는다.
- `VLETraversalApplyInput`은 setup, loaded graph context, cache flag, grammar node id를 묶는다. 새 context
  apply는 이 descriptor를 소비해 graph/load result와 cache identity를 하나의 runtime apply handoff로 받는다.
- `AGE VLE Stream` verbose explain은 `VLE Edge Source`에 fixed source와 candidate index를 분리해 출력한다.
  예를 들어 endpoint btree index가 있어도 global metadata traversal이면 `fixed-source=out=none/in=none`으로
  보이고, local dense edge-state path에서만 `endpoint-btree`가 fixed source로 표시된다. 이 expected output은
  hidden assertion 대신 source-dispatch contract를 직접 보여주는 regression evidence다.
- graph, endpoint, range/direction, output key dynamic fallback과 typed accessor는
  `age_vle_input.c`에 모았다. edge prototype dynamic fallback도 같은 모듈로 옮겨
  `age_vle.c`는 edge label/property constraint descriptor 결과만 받아 traversal state를 채운다.
- 새 VLE context의 graph, edge label/properties constraint, adjacency index OID, vertex metadata load
  policy, initial endpoints, range, direction은 `VLETraversalSetup` descriptor로 묶었다. builder는 이
  setup을 context에 적용하고, 새 context에서는 setup이 이미 찾은 adjacency index OID를 사용해 같은 edge
  label의 index list를 다시 열지 않는다. cached context refresh는 기존 refresh helper를 유지한다.
- `VLETraversalSetup` 안의 metadata load decision은 `VLETraversalLoadPolicy`로 분리했다. graph/edge
  descriptor, endpoint, range/direction, index OID가 먼저 채워진 뒤 policy helper가 edge property
  metadata, edge metadata, vertex metadata load 여부를 계산한다. context builder는 policy boolean을
  조합하지 않고 `manage_GRAPH_global_contexts_len_for_vle()`에 전달한다.
- 새 context에 `VLETraversalSetup`을 적용하는 부분은 `apply_vle_traversal_setup()`으로 묶었다.
  graph/global context, traversal storage mode, endpoint/path function, edge constraint cache, edge label
  and adjacency index OID, range/direction, reverse traversal root 선택을 한 경계에서 처리한다.
  terminal output descriptor 초기화는 그 다음 단계에 남겨 setup 적용과 output materialization 정책을
  분리했다.
- targeted edge-label load decision은 start/end typed endpoint를 직접 사용한다. bound-end-only
  paths-to shape는 vertex list에서 임시 start를 꺼내지 않고 bound end를 reverse traversal root로 삼는다.
- typed endpoint가 정한 traversal root는 `age_adjacency` payload cache key의
  `source_vertex_id`로 직접 내려간다. payload replay 여부도 이 source entry에서 판단해,
  여러 root를 도는 cached VLE에서 낮은 fan-out root 하나가 방향 전체의 cache policy를
  결정하지 않는다.
- source runtime counter는 `age_adjacency_empty_scans`와 `endpoint_btree_empty_scans`를 별도로 누적한다.
  `push_candidates_from_source()`가 source begin/next/end lifecycle에서 실제 yielded candidate 수를 반환하고,
  adjacency/endpoint source wrapper가 0이면 empty scan으로 기록한다. executor는 iterator별 counter를
  CustomScan 실행 전체 누적값에 합산하므로 여러 runtime root를 도는 plan에서도 empty probe 압력이 유지된다.
  2026-06-05 fan-out smoke에서는 `800-label-fanout-*` shape가 `age_adjacency=9/8/...`,
  `empty=age_adjacency:8/endpoint-btree:0`, `pressure=adjacency-empty-probe`로 출력되어 low density가 아니라
  반복 empty source setup이 다음 병목임을 분리했다.
- `age_adjacency` payload source는 `age_adjacency_visible_payload_scan_begin_key()`가 false를 반환하면 scan source를
  열지 않고 empty completion으로 처리한다. 이 경우 source는 선택된 것으로 간주되어 missing-vertex fallback과
  packed fallback이 잘못 활성화되지 않으며, `age_adjacency_empty_source_skips`만 증가한다. 2026-06-05 fan-out
  smoke 재측정에서는 `age_adjacency=1/8/...`, `empty=age_adjacency:0/endpoint-btree:0`,
  `empty-suppressed=age_adjacency:8`, `pressure=adjacency-empty-suppressed`로 바뀌어 root별 empty scan 8회를
  cursor 생성 전에 제거했다.
- suppressed empty source feedback은 outgoing/incoming 방향별 counter도 보관한다. right fan-out은
  `empty-suppressed=age_adjacency:8/out:8/in:0`, left fan-out은
  `empty-suppressed=age_adjacency:8/out:0/in:8`로 출력된다. benchmark summary도 이 값을 별도 column으로
  추출하므로 planner policy가 active direction별 source pressure를 비교할 수 있다.
- adjacency prefetch/cache handoff는 `VLEAdjacencyRootDescriptor`를 사용한다. source vertex, outgoing/
  incoming index oid, direction, self-loop skip policy가 하나의 root descriptor로 내려가므로
  `age_adjacency` scan source와 payload cache key가 같은 root contract를 본다.
- materializer cache handoff도 같은 descriptor 방향으로 맞췄다. path output, `nodes(p)`,
  `relationships(p)`, indexed typed entity materialization이 `VLEMaterializerHandoff`를 공유하므로
  graph context/relation cache/build callback 조합이 caller별 helper signature에 갇히지 않는다.
  handoff에는 output requirement와 traversal root도 포함되어, 다음 terminal hydrate/cache seed 작업이
  `age_adjacency` root descriptor와 같은 source contract를 볼 수 있다.
- path/container builder 실행부는 `age_vle_container.c`로 분리했다. `VLEContainerBuildInput`은 graph oid,
  start id, reverse-output flag, path stack, path vertex stack만 넘기고, container kind dispatch와
  path/reversed/zero/terminal container 조립은 새 모듈에서 처리한다. `age_vle.c`는 iterator output emission에서
  build input을 준비하는 경계만 유지한다.
- materializer build callback signature도 handoff 기반으로 바뀌었다. cache dispatch가 handoff를
  해석한 뒤 다시 `ggctx/id/relation_cache`로 풀어 보내지 않으므로, vertex/edge object builder와 typed
  builder 내부까지 output requirement와 traversal root metadata가 유지된다.
- vertex object cache와 typed vertex cache는 handoff의 candidate vertex를 먼저 seed한다. object cache가
  없는 direct materialization에는 추가 work를 만들지 않고, path/node materialization처럼 같은 vertex를
  결국 materialize하는 경로에서 candidate를 cache hit 대상으로 만든다.
- candidate seed는 global graph의 full-property cache가 이미 채워진 vertex만 object cache로 승격한다.
  terminal-property batch hydrate나 lazy hydrate가 채운 vertex는 재사용하지만, seed 자체가 새 heap fetch를
  만들지는 않는다.
- indexed `age_vle_node_property_at()`은 full properties object를 materialize한 뒤 key lookup하지 않고
  `get_vertex_entry_property_with_cache()`를 사용한다. terminal-property direct output과 indexed node
  property helper가 같은 vertex-entry scalar cache를 우선 확인한다.
- indexed property helper는 node 전용 descriptor에서 node/edge 공통 `VLEIndexedPropertyLookup`으로
  넓혔다. `age_vle_node_property_at()`, `age_vle_edge_property_at()`,
  `age_vle_terminal_vertex_property_from_path()`가 같은 key descriptor와 `fn_extra` relation cache
  handoff를 사용하고, edge entry도 scalar property cache를 보관해 반복 indexed edge property lookup에서
  full properties object materialization을 피한다.
- fixed path indexed node/edge property plan 검증은 DO block hidden assertion에서 visible
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력으로 옮겼다. expected output이 `age_vle_node_property_at`/
  `age_vle_edge_property_at` surface와 `AGE VLE Stream` descriptor를 직접 보여준다.
- `VLEIndexedPropertyLookup`은 `VLE_path_container` candidate vertex metadata, entity kind, runtime
  property key를 묶는다. indexed node/edge property helper와
  `age_vle_terminal_vertex_property_from_path()`는 같은 lookup helper를 사용하므로 candidate scalar-cache
  hit와 relation-cache fallback contract를 공유한다.
- terminal-property stream도 `VLEPropertyKeyDescriptor`를 공유한다. `VLETerminalPropertyLookup`은 key,
  1-byte char fast metadata, relation cache, block prefetch set, prefetch budget을 한 descriptor로 묶고,
  `VLETerminalOutputPolicy`가 이 lookup을 소유한다. direct DFS output, final property emit, batch fetch가
  같은 key/cache contract를 보므로 다음 CustomScan key slot handoff를 runtime helper별로 다시 해석하지
  않아도 된다.
- CustomScan output descriptor는 `AgeVLEOutputRequirement`, terminal key length, char-fast metadata를
  보관한다. executor는 이 값을 `AgeVLEInput`에 복사하고, `build_local_vle_context()`는 `nargs`와 grammar
  id를 다시 조합하지 않고 planner-derived output requirement로 path/terminal-vertex/terminal-property
  emit mode를 정한다. verbose `EXPLAIN`도 path, terminal-vertex, terminal-property surface를 구분하고
  `VLE Shape`도 output requirement 기준으로 출력한다. `requirement`, `len`, `char-fast`를 함께 남겨
  key/cache/output contract를 expected에 드러낸다.
- terminal vertex full properties output은 `terminal-properties` requirement로 같은 output descriptor에
  들어간다. parser는 `properties(n)`이 VLE terminal vertex raw target만 요구하는 shape이면 marker row에
  NULL terminal-key slot을 붙이고, executor는 accepted terminal에서 바로 vertex properties Datum을
  반환한다. 이 경로는 terminal vertex entity나 `VLE_path_container`를 만들지 않으며, mixed consumer가
  scalar key slot과 full-properties slot을 공유하지 않도록 marker row requirement를 구분한다.
- iterator output emission은 `VLEIteratorOutputState` helper 경계로 묶었다. `age_vle_iterator_next()`는
  terminal scalar batch emit, terminal full-properties emit, terminal vertex/path container build,
  iterator finish/dirty cleanup contract를 output helper에 맡긴다. DFS path-function dispatch와
  next-start advance도 `VLEIteratorSearchState` helper 경계로 묶어 traversal search와 output
  materialization이 서로 직접 커지지 않게 했다. materializer 선택은
  `VLEIteratorMaterialization` descriptor로 낮춰 path, terminal vertex, terminal scalar property,
  terminal full properties, zero-bound 여부를 한 handoff로 표현한다. 아직 같은 파일 안에 있지만 다음
  단계의 traversal/output/materializer 모듈 분리 기준점이다.
- `cypher_vle` regression의 VLE index probe, compact count/length/list helper, tail-last helper,
  slice-boundary mode, single edge/node materializer plan 검증은 hidden `DO` assertion에서 visible
  `EXPLAIN (VERBOSE, COSTS OFF)` 출력으로 바꿨다. expected 파일이 `AGE VLE Stream`, direct helper,
  endpoint join presence/absence, single-object materializer 선택을 직접 보여준다.
- `VLE_path_container` count/index contract는 `get_vle_container_edge_count()`,
  `get_vle_container_node_count()`, `normalize_vle_container_index()`로 모았다. single edge/node
  materializer, node/edge id/label/properties direct helper, edge endpoint/property helper가 같은
  negative-index normalization과 bounds contract를 사용한다.
- path/node-list materializer는 path container의 vertex id stream을 먼저 모아
  `prefetch_vertex_entry_properties_by_ids()`로 넘긴다. 이 global graph API는 uncached vertex를 중복
  제거하고 label relation별 scan으로 `vertex_entry`의 full properties cache와 TID를 채운다. 따라서 긴
  path materialization에서 vertex object builder가 같은 relation을 반복해서 열거나 id lazy fetch를
  반복하는 비용을 줄인다. relation cache 수명은 materializer handoff가 이미 가진 cache에 묶고, 같은
  label relation에 충분한 uncached 후보가 모인 경우에만 scan을 선택한다. single typed vertex helper처럼
  cache 수명 관리가 없는 경로에서는 prefetch를 실행하지 않는다.
- lower/upper range와 direction은 planner가 agtype integer/null const에서 typed descriptor로 추출해
  `custom_private`에 싣는다. executor는 이 descriptor를 `AgeVLEInput`에 복사하고,
  `build_local_vle_context()`는 known range/direction slot이면 agtype argument를 다시 열지 않고
  typed int/null metadata를 직접 사용한다. dynamic slot fallback만 기존 agtype parser를 공유한다.
- grammar-node와 terminal-property key도 output descriptor로 `custom_private`에 싣는다.
  executor는 이 descriptor를 `AgeVLEInput`에 복사하고, known terminal-property key slot이면
  `build_local_vle_context()`가 agtype argument를 다시 열지 않는다. `EXPLAIN (VERBOSE)`는 실행마다
  바뀔 수 있는 grammar node numeric id 대신 `cached`/`terminal-only` semantic marker를 출력한다.
- `EXPLAIN (VERBOSE)`의 `AGE VLE Stream` node는 VLE shape, argument count, const/dynamic slot layout,
  graph, edge, endpoint source contract, range, direction, output requirement, materialization class를
  출력한다. plan regression은 CustomScan 존재만 확인하지 않고 어떤 descriptor로 실행되며 path container,
  terminal vertex container, terminal scalar direct, terminal full-properties direct 중 어떤 output handoff를
  쓰는지 expected output에 남긴다. path-container materialization은 label-batch vertex prefetch policy와
  min relation candidate threshold도 출력한다.
- 다음 구조 변경은 `AgeVLEInput` slot parsing 자체를 agtype argument layout에서 typed traversal
  descriptor/custom scan construction으로 줄이는 방향에서 진행한다.
- rescan/end에서는 multi-call context cleanup callback을 유지하되, 조기 종료된 cached local context를
  clean으로 되돌리지 않는다. `EXISTS` subplan처럼 consumer가 VLE stream을 끝까지 읽지 않는 shape에서
  dirty DFS stack을 재사용하지 않기 위한 contract다.
- 남은 한계: traversal state 자체는 아직 `VLE_local_context`와 `age_vle.c`에 남아 있다. 다음 단계는
  start/end vertex, edge prototype, range, direction, terminal-only/property output 요구를 argument
  expression이 아닌 typed VLE descriptor로 정규화하고, consumer projection이 materialization 요구를
  executor에 직접 넘기게 하는 것이다.

## 남은 병목 후보

1. Cold global graph load
   - vertex/edge table full scan
   - label-unrelated load 중 targeted adjacency VLE의 vertex label list scan은 생략했다.
     targeted edge label의 adjacency-index existence scan, index OID lookup, graph/label lookup
     반복도 cache한다. fallback edge metadata load에서는 constraint 없는 query의 property
     metadata read를 생략했고, targeted edge label의 endpoint vertex table scan은 skeletal
     vertex + lazy hydrate 경로로 낮췄다. lazy hydrate 결과도 vertex entry에 cache한다.
     terminal property batch fetch가 읽는 tuple은 skeletal vertex entry cache도 채우도록 연결했다.
     terminal-property가 아닌 path/node-list materialization은 vertex id를 label relation별로 묶어
     full properties cache를 선행 hydrate하고, `AGE VLE Stream` EXPLAIN은 이 policy와 threshold를
     출력한다. terminal-only, label-constrained, property-constraint-free VLE는 endpoint btree index가
     있으면 global edge metadata load를 생략하고 DFS expansion 시점에 `start_id`/`end_id` index를
     endpoint id로 probe한다. `AGE VLE Stream` EXPLAIN은 source contract를 `VLE Edge Source`로 출력하며,
     label/index가 확인된 terminal-only shape에서는 `local-index-candidate`와 `state=dense-local`을
     expected output에 남긴다. dense-local edge state는 endpoint `stadistinct`, edge label `reltuples`,
     bounded upper depth를 이용해 local edge hash와 flag array 초기 capacity를 잡는다. candidate provider는
     더 이상 short path stack을 선형 scan하지 않고 dense `VLE_EDGE_STATE_USED` flag로 cycle-check를
     통일한다. `VLETraversalState`는 frame stack, path stacks, edge state, cached path depth를 한 boundary로
     묶고, consume/reset/free가 이 state 단위로 움직인다. `AGE VLE Stream` edge-source descriptor는 planner가
     확인한 label relation `reltuples`와 endpoint fanout evidence도 담고, verbose explain은
     `cost=reltuples=... fanout=start:.../end:... stats=rel:.../start:.../end:...` 형태로 출력한다.
     fanout 값과 통계 신뢰도를 분리하므로, fanout 0과 endpoint statistics 부재를 같은 상태로 취급하지 않는다.
     남은 후보는 이 planner evidence를 source cost adjustment와 runtime feedback cache가 실제로 읽게 하는 구조다. 현재 fanout evidence는
     `VLESourceFanoutEvidence`로 공통화되어 stream descriptor, local edge-state capacity, `age_adjacency`
     custom path costing이 같은 relation/fanout vocabulary를 읽는다. local-index-candidate plan은 planner-only
     cost policy도 함께 출력해 endpoint-btree/`age_adjacency` recommendation을 visible regression에 남긴다.
     descriptor에는 text와 directed enum recommendation을 같이 보관한다.
     cached context refresh도 같은 policy descriptor를 유지한다. refresh input은 start/end vertex뿐 아니라
     CustomScan descriptor의 directed source policy도 갱신한다. multi-loop fixture는 5개 start vertex에 대해
     같은 `AGE VLE Stream`을 반복 실행하고, `VLE Source Runtime`은 마지막 loop가 아니라 전체 누적
     `endpoint-btree=5/6`을 출력한다.
     source policy explain은 `endpoint-work=sum(current/limit)`과 `reason=out:.../in:...`을 출력한다.
     같은 line에 `consumer=... consumer-class=...`를 함께 출력해 path materialization 없이 terminal-only
     output을 소비하는 source decision인지 regression-visible하게 구분한다.
     undirected source policy는 direction별 endpoint work와 별도로 `combined-work=all:.../...`도 출력한다.
     이는 out/in endpoint fanout을 따로 통과시키더라도 실제 undirected frontier가 합산 fanout으로 커지는
     경우를 threshold 보정 대상으로 드러내기 위한 값이다.
     low fanout/depth 1 tie는 endpoint-btree를 유지하고, low fanout/depth 2 tie는
     `out:6/6 reason=out:work-tie`로 `age_adjacency`를 선택한다. fanout 3/depth 2 fixture는
     `out:12/6 reason=out:work-exceeds-limit`으로 `age_adjacency`를
     선택한다. 이 값은 leaf depth 하나가 아니라 bounded traversal 전체의 누적 branch work다. depth 2 cap은
     제거했고, bounded/property-constraint-free VLE는 finite upper 전체에 costed policy를 적용한다. fanout 3,
     depth 3 fixture는 `out:39/14`를 출력해 큰 bounded fan-out에서도 `age_adjacency` source가 plan descriptor로
     설명되는지 확인한다.
     `vle_adjacency_only_policy` fixture는 edge relation fanout statistics가 없는 cold policy에서
     `reason=out:unknown-fanout/in:unknown-fanout`을 출력한다.

2. Dense traversal state
   - `age_adjacency` payload와 VLE frame/edge state 사이의 중복 lookup
   - edge-state hash sizing과 path stack update 비용
   - terminal property/output cache와 traversal frame의 연결 비용

3. Generic property materialization
   - property fetch -> scalar conversion -> `agtype` materialization -> aggregate/sort/hash
     경로
   - HashAggregate input width
   - final projection output descriptor 부재

## Benchmark 기준 workload

- 800-label fan-out synthetic workload.
  - `tools/vle_benchmark.sql` 기본 profile은 `label_fanout_labels=800`, `label_fanout_edges=64`,
    `run_standard_cases=0`이다.
  - benchmark는 unrelated `NoiseEdge_*` labels와 constrained
    `(:FanoutStart {i: 0})-[:FanoutEdge*1..2]->(n) RETURN n.i` terminal-property VLE를 만든다.
  - output은 timing/result count와 함께 PostgreSQL `EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF,
    SUMMARY OFF)`에서 `AGE VLE Stream`, `VLE Edge Source`, `VLE Source Runtime` lines를 저장해 source policy와
    runtime counter를 같이 비교한다.
  - 800-label smoke 결과는 `rows_returned=64`, `fixed-source=out=age-adjacency/in=endpoint-btree`,
    `endpoint-work=sum(out:4160/6,in:2/6)`, runtime dominant `age-adjacency`다.
  - `VLE Source Runtime` feedback은 source별 `density=...`를 출력한다. 이는 candidates/scans 비율이며,
    다음 source threshold 보정에서 age_adjacency, endpoint-btree, packed source의 실제 효율을 비교하는 기준이다.
  - `VLE Source Runtime`은 `planned=out:.../in:... source-match=...`도 출력한다. planner fixed-source descriptor와
    runtime dominant source가 같은 family인지 EXPLAIN line 자체에서 확인한다.
  - feedback은 `class=... recommendation=...`도 함께 출력한다. `endpoint-direct`는 endpoint-btree source를
    유지할 근거이고, `adjacency-cache-seeded`는 multi-step에서 `age_adjacency` depth policy를 유지하거나 더
    강화할 근거다.
  - runtime feedback line은 planner policy text의 `class`/`recommendation` token도 `planned-class`,
    `planned-recommendation`으로 다시 싣고, runtime feedback class와 맞는지 `class-match`로 출력한다. 따라서
    benchmark summary를 보지 않아도 raw `EXPLAIN ANALYZE`에서 source family mismatch와 feedback class mismatch를
    같이 확인할 수 있다.
  - `VLE Edge Source` planner policy도 `class=... recommendation=...`을 출력한다. benchmark 끝의 summary
    query는 planner policy/reason/class/recommendation과 runtime dominant/class/recommendation을 별도 row로
    추출한다.
  - planner source policy 입력은 `VLESourcePolicyProfile`로 묶는다. 이 profile은 output requirement,
    consumer class, endpoint fanout budget, finite depth, cost eligibility, `age_adjacency` cache seed eligibility를
    함께 담는다. `cache-seed=eligible`은 finite multi-step이고 `age_adjacency` 후보가 있는 source policy만 표시한다.
  - generic cache seed 가능한 source policy는 endpoint-btree가 기존 fanout budget 안에 있더라도 work limit의
    75% 안에 들어오는 경우에만 endpoint-btree를 유지한다. planned empty lifecycle이 있는 source는 repeated
    empty completion까지 기대하므로 endpoint 유지 headroom을 50%로 낮춘다. 이 headroom을 넘으면
    `reason=...empty-lifecycle-headroom...`으로 `age_adjacency`를 선택한다.
  - 기본 benchmark는 terminal-scalar, terminal-object, path-materialized shape를 모두 실행한다. planner/runtime join
    summary는 consumer, fanout budget, planner policy, runtime dominant source, `source_match`, planner/runtime
    join 기준 `class_match`, runtime line 기준 `runtime_class_match`, `cache_seed`, `endpoint_headroom`, source별
    density를 한 row로 보여줘 source threshold mismatch를 바로 확인하게 한다.
- constrained start + terminal anonymous 단일 VLE path.
- nested agtype containment 100k/200k row workload.
- scalar property GROUP BY/DISTINCT/sort/collect synthetic workload.

측정 시 최소 2개 이상을 같이 본다.

- `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)`
- lldb breakpoint/call count
- focused synthetic runtime 반복
- normalized `EXPLAIN` 출력 regression

## 최근 검증 기록

- 상세 검증 이력은 `HISTORY.md`에 둔다. `VLE.md`에는 현재 VLE 구조와 regression coverage만 유지한다.
- 최근 큰 VLE source descriptor 묶음은 `make clean`, `COPT=-Werror` build, install,
  `installcheck REGRESS='cypher_vle age_global_graph age_adjacency'`, `git diff --check`를 통과했다.
- `AGE VLE Stream` regression은 plan evidence를 hidden assertion으로 숨기지 않고
  `EXPLAIN (VERBOSE, COSTS OFF)` 또는 `EXPLAIN (ANALYZE, VERBOSE, TIMING OFF, SUMMARY OFF)` 출력에
  source layout, planner cost evidence, runtime counter를 드러내는 방향을 유지한다.
- `age_upgrade`는 VLE correctness gate가 아니라 fresh-install-only 정책 smoke test다.

## Regression coverage

주요 관련 파일:

- `regress/sql/cypher_vle.sql`
- `regress/expected/cypher_vle.out`
- `regress/sql/cypher_match.sql`
- `regress/expected/cypher_match.out`
- `regress/sql/age_global_graph.sql`
- `regress/expected/age_global_graph.out`
- `regress/sql/age_adjacency.sql`
- `regress/expected/age_adjacency.out`
- `regress/sql/age_upgrade.sql`
- `regress/expected/age_upgrade.out`

자주 쓰는 focused set:

```sh
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='cypher_vle age_global_graph cypher_match security'
```

```sh
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='expr cypher_match cypher_create cypher_set cypher_delete cypher_merge cypher_vle age_global_graph age_adjacency security age_upgrade'
```

## 다음 설계 판단

- VLE full path/list output이 필요하지 않은 consumer는 materialization fallback으로 보내지 않는다.
- `.id`/property semantic이 바뀔 수 있는 arbitrary list/entity projection은 fallback을 유지한다.
- 새로운 fixed path arity helper를 추가하지 않는다.
- plan shape 검증은 assertion block보다 출력 regression을 우선한다.
- runtime source 선택 검증은 hidden assertion보다 `EXPLAIN (ANALYZE, VERBOSE, TIMING OFF, SUMMARY OFF)`
  출력에 `VLE Source Runtime`을 남기는 방식으로 유지한다.
- planner source-cost 검증은 `VLE Source Cost`의 `reltuples=... fanout=... stats=...` 출력으로 유지한다.
- source cost policy 검증은 `VLE Source Policy` line의
  `out=.../in=... endpoint-work=sum(...) reason=...` 출력으로 유지한다. fanout statistics 신뢰도는
  `VLE Source Cost`의 `stats=rel:.../start:.../end:...` 출력으로 확인한다.
- planner source policy의 threshold 보정 방향은 `VLE Source Policy`의 `class=... recommendation=...` 출력으로 확인한다.
  benchmark planner summary는 `consumer`, `consumer_class`, `planner_policy`, `policy_reason`,
  `policy_class`, `recommendation`을 함께 뽑아 runtime feedback과 같은 workload shape에서 비교한다.
- source cost policy handoff는 `AGE VLE Stream` edge-source descriptor의 directed policy enum을 기준으로
  진행한다. runtime source layout이 fanout/statistics를 다시 계산하지 않고 descriptor recommendation을 읽는
  구조를 유지한다.
- source cost policy는 depth 2 이하로 제한하지 않는다. finite upper 전체에 누적 work를 적용하되, infinite upper와
  property constraint가 있는 shape는 layout decision으로 둔다.
- source policy handoff가 local-index source를 고르면 runtime local edge-state도 graph metadata load 여부보다
  planner descriptor를 우선한다. 다만 path materialization은 edge object lookup을 요구하므로 terminal/local-safe
  shape와 분리해 유지한다.
- runtime source feedback은 CustomScan execution 전체 누적값으로 본다. nested-loop 반복 실행에서 마지막 iterator
  snapshot만 남기는 형태는 cost 보정 근거로 사용하지 않는다.
- runtime source feedback은 dominant source뿐 아니라 source별 scan density도 같이 본다. scan density가 낮으면
  source setup/empty scan 비용을 줄이는 방향으로 threshold나 fallback suppression을 조정한다.
- runtime source feedback의 `class`와 `recommendation`을 threshold 보정의 1차 분류로 사용한다. 같은 숫자라도
  `endpoint-direct`와 `adjacency-cache-seeded`는 서로 다른 source policy 조정을 의미한다.
- runtime pressure는 class/recommendation보다 한 단계 더 직접적인 다음 action hint다. `VLE Source Runtime`의
  `pressure=... action=...`은 source handoff mismatch, class mismatch, materialized tie, cache seed miss,
  low adjacency density, endpoint fanout을 분리한다. benchmark summary도 `runtime_pressure`, `runtime_action`을
  출력하므로, 다음 threshold/fallback 조정 후보를 raw EXPLAIN과 SQL summary에서 같은 vocabulary로 볼 수 있다.
- 2026-06-05 fanout small smoke에서는 모든 800-label fanout shape가 `adjacency-density-low`와
  `check-fallback-suppression`을 출력했다. source family/class match는 이미 true이므로 다음 큰 후보는 source
  전환 자체보다 `age_adjacency` scan density와 local fallback suppression lifecycle이다.
- planner policy와 runtime feedback의 class vocabulary는 맞춰 둔다. benchmark에서 둘이 다르면 source threshold나
  cached refresh handoff가 어긋난 신호로 본다.
- benchmark의 `source_match`가 true라도 `class_match` 또는 `runtime_class_match`가 false이면 planner policy는 같은
  source family를 골랐지만 runtime feedback이 더 강한 depth/cache 근거를 발견했다는 뜻이다. 이 경우 source enum을
  바꾸는 guard보다 planner budget, work-tie policy, payload replay evidence를 descriptor에 더 반영하는 방향으로
  조정한다.
- `cache_seed=eligible`인데 runtime class가 `endpoint-direct`에 머무르면 depth/cache seed 후보와 실제 source
  효율이 어긋난 것이다. 반대로 `cache_seed=ineligible`이면 단순 depth guard가 아니라 adjacency availability와 finite
  upper bound를 먼저 확인한다.
- `vle_headroom_policy` regression fixture는 old endpoint work limit 안에 있지만 empty lifecycle headroom을 넘는
  fanout을 만들어 `reason=out:empty-lifecycle-headroom`으로 `age_adjacency`가 선택되는 plan evidence를 유지한다.
- `VLE Source Runtime`의 `source-match=false`는 SQL benchmark join을 보지 않아도 fixed-source handoff나 cached
  refresh가 runtime source 선택과 어긋났다는 직접 신호다. 이 경우 fallback guard보다 descriptor handoff와 refresh
  contract를 먼저 점검한다.
- multi-step `age_adjacency` planner policy는 class를 `adjacency-cache-seeded`로 출력한다. 실제 runtime cache seed
  발생 전이라도 bounded depth에서 payload cache seed/replay가 source 선택 근거이므로, planner와 runtime summary의
  `class_match`가 threshold mismatch 신호로 쓸 수 있는 vocabulary를 공유한다.
- source fanout 산정은 `VLESourceFanoutEvidence`를 기준으로 유지하고 caller-local statistics 조합을 다시
  늘리지 않는다.
- planner source policy feedback은 `cost_policy` 문자열만이 아니라 `AgeVLEStreamEdgeSource` descriptor의
  `policy_class`, `policy_recommendation`, `cache_seed_eligible`, `endpoint_headroom_percent` field로 보관한다.
  `VLE Source Runtime`의 `planned-class`와 `planned-recommendation`은 이 typed descriptor를 읽으므로, runtime
  formatter가 `VLE Edge Source` explain text를 다시 파싱하지 않는다.
- planner source policy profile도 `cost_policy` 문자열에서 분리한다. `policy_consumer`,
  `policy_consumer_class`, `policy_active_direction`, `policy_fanout_budget`은 `AgeVLEStreamEdgeSource`
  descriptor field로 보관하고, `VLE Edge Source`는 이를 `profile=consumer:.../class:.../active:.../budget:...`
  surface로 출력한다. benchmark summary는 이 profile surface에서 consumer/active/budget을 읽고, `policy=`는
  directed source, depth/work, reason, class/recommendation 확인용으로만 사용한다.
- source policy profile은 traversal direction을 active direction으로 보관한다. `right` VLE는 outgoing policy만,
  `left` VLE는 incoming policy만, undirected VLE는 양쪽 policy와 combined work를 평가한다. inactive side는
  `in:inactive-direction` 또는 `out:inactive-direction`으로 출력해 benchmark가 실제 쓰지 않는 방향의 fanout/work를
  threshold mismatch로 오해하지 않게 한다.
- `tools/vle_benchmark.sql`은 right fan-out과 left fan-out workload를 모두 만들고, planner/runtime join summary에서
  `active_planner_source`와 `active_planned_source`를 추출한다. `source_match`는 active direction source와 runtime
  dominant source를 직접 비교하므로 inactive side가 문자열에 남아 있어도 false positive를 만들지 않는다.
- `VLE Source Runtime`은 suppressed empty source도 active direction evidence로 올린다.
  `suppressed-source=out:age-adjacency/in:none` 또는 `out:none/in:age-adjacency`는 실제로 cursor 생성을
  건너뛴 directed source를 뜻하고, `suppression-match=true`는 그 source가 planner/runtime descriptor의
  planned directed source와 일치했다는 뜻이다. benchmark summary는 `suppressed_source`,
  `suppression_match`를 추출하므로 `adjacency-empty-suppressed` pressure를 source mismatch와 lifecycle
  optimization으로 분리해 볼 수 있다.
- 2026-06-05 small fan-out smoke에서는 right/left fan-out 모두 `age_adjacency` empty source 8개가
  suppressed 되고, right는 `out:age-adjacency/in:none`, left는 `out:none/in:age-adjacency`로 출력되며
  `suppression-match=true`였다. 따라서 현재 병목 후보는 source handoff mismatch가 아니라 반복 source completion을
  더 상위 root/source descriptor와 batching contract로 올리는 쪽이다.
- `age_adjacency` payload cache는 payload array뿐 아니라 `known_empty` state도 보관한다. cache replay와
  known-empty hit은 payload scan cursor를 만들기 전에 처리하므로, 같은 activation 안에서 converging paths가 같은
  empty terminal source를 다시 확장하면 AM `begin_key` probe 없이 source completion으로 끝난다.
- payload cache storage는 activation `multi_call_context` reset에 묶이지 않는다. activation cleanup은 open
  adjacency scan cursor만 닫고, payload cache는 cached VLE context 전체 cleanup에서 명시적으로 해제한다.
- `VLE Source Runtime`의 `empty-cache=age_adjacency:N/out:N/in:N`은 payload source begin 단계에서 소비한
  known-empty cache hit 수다. source object 생성 전에 missing-vertex source-run 전체가 접히면
  `empty-run=age_adjacency:N/out:N/in:N`으로 기록한다.
- frontier-level existence check는 `age_adjacency` visible payload scan의 directory cache를 hint로 사용한다.
  delta block이 있으면 보수적으로 unknown으로 두고, directory cache range 안에서 key가 없을 때만 known-empty로
  본다. VLE source는 scan 중 payload cache hash를 바로 mutate하지 않고 source-local `frontier_empty_keys` batch에
  모은 뒤 source 종료 시점에 `known_empty` cache entry로 반영한다. self-loop처럼 현재 active source key는 batch
  대상에서 제외해 active source cache entry와 empty marker가 충돌하지 않게 한다.
- `VLE Source Runtime`의 `empty-frontier=age_adjacency:N/out:N/in:N`은 frontier directory-cache hint로 새로
  기록한 known-empty source 수다. 이후 같은 activation에서 그 source가 payload source begin까지 내려가면
  `empty-cache` hit이 되고, missing-vertex source-run precheck에서 먼저 처리되면 `empty-run` skip이 된다.
  `vle_frontier_empty_policy` regression은 `empty-frontier=age_adjacency:1/out:1/in:0`과
  `empty-run=age_adjacency:1/out:1/in:0`을 함께 고정한다.
- 2026-06-05 800-label fan-out smoke에서는 right fan-out path/terminal/property/vertex shape가
  `empty-frontier=age_adjacency:64/out:64/in:0`과 `empty-cache=age_adjacency:64/out:64/in:0`을 출력했다.
  left fan-out shape는 reverse direction에서 기존 empty source suppression이 먼저 적용되어
  `empty-suppressed=age_adjacency:64/out:0/in:64`가 유지된다.
- `vle_empty_cache_policy` regression은 converging path의 terminal empty source를 source-run precheck 단계에서
  `empty-run=age_adjacency:1/out:1/in:0`으로 접는 것을 고정한다. 이 값은 source family를 바꾼 것이 아니라
  known-empty source-run lifecycle을 payload source begin보다 위로 올린 evidence다.
- `empty-suppressed`, `empty-frontier`, `empty-run`이 planned `age_adjacency` source와 일치하고 planned class가
  `adjacency-cache-seeded`이면 runtime feedback class도 `adjacency-cache-seeded`로 정규화한다. 이 경우
  `missing-vertex` counter가 남아도 class mismatch가 아니라 planned adjacency lifecycle 안에서 empty source를
  상위 단계에서 접은 것으로 해석한다.
- `AgeVLEStreamEdgeSource` descriptor는 planner가 empty source lifecycle을 기대하는지
  `empty_lifecycle_eligible`, `empty_lifecycle_depth` typed field로 보관한다. `VLE Edge Source`는
  `empty-lifecycle=eligible|ineligible/depth:N`, `VLE Source Runtime`은 `empty-plan=...`을 출력한다.
  runtime feedback은 이 field를 읽어 planned cache-seeded lifecycle과 empty suppression/frontier/run evidence를
  비교하므로, EXPLAIN 문자열 token 변경이 source policy 판단을 깨지 않는다.
- `VLE Source Runtime`은 `empty-evidence=...`도 출력한다. `empty-run`이 있으면 `empty-frontier`보다 상위
  lifecycle evidence로 보고, planned empty lifecycle과 일치할 때 pressure는 `adjacency-empty-run` /
  `action=keep-empty-run:out|in|both`가 된다. frontier hint만 있으면 `adjacency-empty-frontier` /
  `keep-empty-frontier:...`로 출력한다. 이 vocabulary는 source mismatch가 아니라 planned negative-property
  lifecycle을 어디에서 소비했는지를 나타낸다.
- frontier known-empty queue는 flush 단위도 누적한다. `VLE Source Runtime`은
  `empty-frontier-batch=flushes:N/out:N/in:N/keys:N/max:N`을 출력하므로, `empty-frontier` mark 수와 별개로
  source completion queue가 몇 번, 어느 방향, 어느 폭으로 batch 처리됐는지 확인할 수 있다.
- `age_adjacency` payload cache evidence는 source-run 수와 tuple/event 수를 분리한다. `VLE Source Runtime`은
  `payload-cache=runs:scan:N/replay:N/seed:N/tuples:scan:N/replay:N/seeds:N`을 출력한다. `runs`는 source key를
  fresh scan으로 열었는지, cached payload를 replay했는지, payload cache를 seed한 source-run이 있었는지를
  보여주고, `tuples`/`seeds`는 실제 payload scan/replay tuple 수와 cache seed event 수를 보여준다.
- payload run evidence는 backend-local source feedback cache에도 들어간다. `VLE Source Payload Input`은 다음
  planning에서 읽은 payload feedback을
  `source=none|runtime-cache headroom=N scan-runs=N replay-runs=N seed-runs=N observed=N reason=... class=...`
  로 출력한다. 이 값은 raw runtime counter가 아니라 planner profile이 소비한 input property다.
- `tools/vle_benchmark.sql`은 planner summary에 `empty_lifecycle`, `empty_lifecycle_depth`, runtime summary에
  `empty_plan`, `empty_plan_depth`, `runtime_empty_plan_match`, join summary에 `empty_plan_match`,
  `empty_plan_depth_match`, `empty_evidence`, `empty_frontier_batch_*`, `payload_*_runs`,
  `payload_scan_tuples`, `payload_replay_tuples`, `payload_seed_events`, `payload_input_*`를 출력한다.
  `source_match`, `class_match`, `suppression_match`가 true이면서 `empty_plan_match`도 true이면 source handoff가
  아니라 root/source lifecycle threshold 보정 문제로 본다.
- `VLE Source Runtime`의 `threshold-feedback=...`은 executor final source stats에서 backend-local threshold
  feedback cache로 기록된다. key는 `graph,label,edge-label-oid,consumer-class,active-direction`이며,
  다음 planning의 `VLESourcePolicyProfile`이 이를 읽어 endpoint headroom과 empty-batch 후보로 사용한다.
  runtime line은 `threshold-feedback=.../headroom:N/batch:N/source:.../reason:...`을 출력한다.
- `active-direction=both`에서 runtime source가 `source:both`로 집계되면 같은 feedback을 `out`/`in`
  directional key에도 투영한다. 이렇게 해야 undirected VLE가 먼저 만든 repeated empty completion과 payload
  cache evidence를 이후 directed VLE가 같은 backend에서 `VLE Source Threshold Input: source=runtime-cache ... direction=out|in`으로
  소비할 수 있다.
- `AgeVLEStreamEdgeSource` descriptor는 planner가 읽은 feedback input을 `threshold_input_known`,
  `threshold_input_headroom_percent`, `threshold_input_batch_size`, `threshold_input_observed_count`,
  `threshold_input_saturated_count`, `threshold_input_relaxed_count`, `threshold_input_source`,
  `threshold_input_reason`으로 보관한다.
  `VLE Source Threshold Input`은 `source=none|runtime-cache headroom=N batch=N direction=... reason=... class=...`와
  `observed/saturated/relaxed` cache state를 함께 출력한다. `tools/vle_benchmark.sql`은 planner summary와
  planner/runtime join summary에서 이 값을 추출한다.
- `vle_empty_cache_policy` regression은 `EXPLAIN ANALYZE`로 saturated
  `threshold-feedback=eligible/headroom:35/batch:16/source:out/reason:root-empty-saturated`를 만든 뒤 같은
  backend에서 후속 `EXPLAIN`을 실행해 `VLE Source Profile`의 `empty-batch=eligible/size:16`과
  `VLE Source Threshold Input`의
  `source=runtime-cache headroom=35 batch=16 direction=out reason=root-empty-saturated observed=1 saturated=1 relaxed=0`
  를 expected에 남긴다. 이 regression은 plan assertion이 아니라 raw CustomScan evidence로 planner feedback
  handoff와 batch feedback 적용을 고정한다.
- `vle_directional_feedback_policy` regression은 undirected `EXPLAIN ANALYZE`가 만든
  `threshold-feedback=.../source:both`를 directed `EXPLAIN`이 `source:out` runtime cache input으로 읽는 것을
  expected에 남긴다.
- 반대 방향도 같은 lifecycle property로 본다. directed 실행에서 먼저 얻은 `out` 또는 `in` threshold feedback은
  후속 `active=both` request가 exact `both` cache를 찾지 못할 때 direction family fallback으로 합쳐 소비한다.
  이때 headroom은 더 공격적인 낮은 값, empty batch는 더 큰 값, threshold class는 더 강한 lifecycle class를
  사용한다. `vle_empty_cache_policy`의 후속 undirected EXPLAIN은 `active=both` 계획이
  `direction=out reason=root-empty-repeat-observed class=adjacency-empty-lifecycle` input을 읽고 policy class를
  유지하는지 보여준다.
- `tools/vle_benchmark.sql`도 이 handoff를 별도 surface로 출력한다. `800-label-fanout-family-path`는 directed
  right/left fan-out feedback 뒤에 undirected path explain을 실행하고,
  `threshold_directional_family=true`, `threshold_input_source=out|in|mixed`로 exact `both` cache가 아니라 방향
  family input을 소비했는지 보여준다. 다음 threshold 보정은 이 column을 기준으로 mixed-direction feedback을
  일반 exact feedback과 분리해서 해석한다.
- mixed direction family feedback은 exact `both` feedback보다 완만하게 적용한다. `active=both` request가
  `out`/`in` family entry를 합쳐 읽으면 repeated empty lifecycle class와 batch size는 유지하지만 endpoint
  headroom은 최소 0.40으로 올린다. directed exact plan은 0.30/0.25 같은 더 공격적인 repeat headroom을 계속
  사용할 수 있고, undirected plan만 한쪽 방향의 empty completion이 다른 방향을 과도하게 누르지 않도록 한다.
- benchmark join summary는 mixed family decision을 해석하기 위한 비율도 계산한다.
  `directional_family_productive_density`는 같은 plan의 productive `age_adjacency` density이고,
  `directional_family_empty_completion_ratio`는 returned row 대비 root empty completion 비율,
  `directional_family_empty_out_ratio`/`directional_family_empty_in_ratio`는 empty completion 방향 split이다.
  이 값들은 threshold를 더 낮출지보다, mixed family를 방향별 partial policy로 나눌 근거를 보기 위한 surface다.
- `VLE Source Runtime`의 `empty-plan=eligible|ineligible/depth:N match=true|false`는 raw EXPLAIN에서 같은 판단을
  드러낸다. empty evidence가 없으면 match는 true이고, `age_adjacency` empty probe/suppression/cache/frontier/run
  evidence가 있으면 planned empty lifecycle eligibility가 있어야 true다.
- general expansion source-run도 `known_empty` payload cache entry를 source object 생성 전에 소비한다. 기존
  missing-vertex precheck와 같은 source-run boundary를 사용하며, cursor가 known-empty이면
  `empty-run=age_adjacency:N/out:N/in:N`으로 기록하고 해당 direction을 used source로 남긴다. 80-label/8-edge
  fanout smoke에서 right fanout의 `empty-cache=8`은 `empty-run=8`로 올라갔다.
- `age_adjacency` AM의 known-empty probe는 delta가 없을 때 directory cache range 안의 missing key뿐 아니라,
  directory lookup으로 찾을 수 없는 cache range 밖 key도 negative property로 본다. 이 broader directory probe로
  reverse fanout도 `empty-suppressed=8`이 아니라 `empty-frontier=8`과 `empty-run=8`로 처리된다.
- runtime feedback은 descriptor의 `cache_seed_eligible`을 같이 본다. depth 1 terminal-object처럼 cache seed policy가
  ineligible인 shape에서 payload seed/replay counter가 생겨도 `adjacency-cache-seeded`로 과대 분류하지 않고 planner
  class/recommendation과 같은 `adjacency-work` evidence로 남긴다.
- endpoint-btree/`age_adjacency` tie는 단일 step과 multi-step을 분리한다. multi-step tie에서 endpoint-btree로
  되돌리는 guard를 추가하지 말고, runtime counter와 payload replay evidence를 기준으로 policy threshold를 더
  보정한다.
- 단일 step tie도 consumer class를 본다. `terminal-scalar`는 endpoint-btree direct scalar lookup 비용이 낮으므로
  endpoint tie를 유지하지만, `path-materialized`와 `terminal-object`는 final object/path materialization 비용과
  label-row fallback을 함께 고려해 `age_adjacency`를 선호한다. 이 경우 policy class는
  `adjacency-materialized-tie`, recommendation은 `prefer-age-adjacency-materialization`으로 출력한다.
- `vle_tie_policy` regression fixture는 같은 fanout 1 incoming traversal에서 `terminal-property`와
  `path`/`terminal-vertex` consumer가 서로 다른 tie policy를 쓰는 것을 `EXPLAIN (VERBOSE, COSTS OFF)` expected에
  직접 남긴다.
- 일반 fixed-length MATCH의 `AGE Adjacency Match`도 이제 실험용 GUC가 아니라 기본 index path 후보로 동작한다.
  이 변경은 VLE의 `age_adjacency` source policy와 같은 방향이다. endpoint-bound one-hop MATCH는 edge variable,
  edge property predicate, right node property predicate가 있어도 `age_adjacency` payload를 먼저 stream하고 필요한
  residual predicate/projection을 위에서 처리한다.
- `AGE Adjacency Match` executor는 한 endpoint payload를 `List`로 전부 모으지 않고
  `AgeAdjacencyVisiblePayloadScan` cursor에서 한 tuple씩 반환한다. 이는 VLE payload replay/cache가 추구하는
  source-run lifecycle과 같은 원칙으로, 큰 fan-out에서 memory materialization boundary를 줄이는 구조다.
- `AGE Adjacency Match` EXPLAIN은 `Adjacency Index`와 `Adjacency Payload Columns`를 출력한다. direction,
  endpoint key, fanout, edge/right residual requirement, edge variable 여부, required payload columns가 raw plan
  surface에 남으므로 VLE source descriptor와 같은 방식으로 index path의 required/provided property를 비교할 수 있다.
- required payload columns는 executor에서만 사후 계산하지 않고 CustomPath 생성 전 `AdjacencyMatchPayloadRequest`로
  산정한다. 따라서 `start_id,end_id`만 필요한 id-only source와 `properties`를 포함하는 edge-row source가 cost,
  descriptor, actual scan tlist에서 같은 column vector를 공유한다.
- terminal right label id는 `AgeAdjacencyVisiblePayloadScan`에 전달되어 `next_vertex_id`의 graphid label bits로
  cursor 내부에서 pruning된다. 이는 VLE frontier source에서도 적용 가능한 source-level label pruning contract다.
  metadata-backed terminal property index와 constant AGTYPEOID value가 있는 right property predicate는 executor가
  terminal vertex id lookup으로 tuple emission 전에 precheck한다. lookup은
  `cypher_adjacency_match_terminal.c`가 소유하고, repeated endpoint는 execution-local match cache로 재사용한다.
  AGTYPEOID property source index oid가 descriptor에 있으면 begin 단계에서 property index를 스캔해 matching
  vertex id set을 미리 채우고 `mode=property-index-prefetch`로 출력한다. `EXPLAIN ANALYZE`는 prefetch match 수,
  payload 후보 수, terminal filter 수, emitted row 수를 별도 runtime line에 남긴다. 상위 SQL residual은 semantic
  recheck로 남긴다.
- 방향별 `age_adjacency` source index는 `create_adjacency_index(es)` helper로 생성할 수 있다. VLE와 fixed
  MATCH가 같은 `(endpoint,id,next)` index layout을 공유하도록 DDL surface를 맞췄다.
- `age_adjacency` payload v4는 SQL key column contract는 유지하지만 bulk-built main posting에서 endpoint key를
  제거한다. directory entry가 run key와 count를 제공하므로 VLE/fixed MATCH source scan은
  `(heap_tid, edge_id, next_vertex_id)` main payload를 active key로 재구성한다. delta page는 insert 후 unordered
  scan을 위해 full key를 유지한다.
- visible payload main cache도 compact main payload를 그대로 보관한다. cached entry마다 endpoint key를 다시 두지
  않고 active key를 emission 직전에 붙이므로, VLE source replay/cache path가 on-disk v4 compaction의 row-width 이득을
  runtime cache에서도 유지한다.
- main cache load는 page 전체가 아니라 active key의 run window만 읽는다. directory `first_offnum/posting_count`가
  VLE payload cursor의 cache allocation 범위를 정하므로, 같은 page의 다른 endpoint run은 현재 source scan에 실리지
  않는다.
- main run은 posting마다 line pointer를 두지 않고 run-local packed block item 안에
  `(heap_tid, edge_id, next_vertex_id)` payload를 보관한다. block의 edge/next label id가 같으면 label id는
  header에 한 번만 저장하고 각 posting은 48-bit entry id만 저장한다. directory entry가 첫 block과 logical posting
  count를 가리키므로 VLE/fixed MATCH source cursor는 CSR의 offset/list boundary처럼 block 내부 payload를 순서대로
  stream한다. bulk delete는 block 내부 posting을 compact하고, cost hook은 compact block page density를 main page
  estimate에 사용한다.
- bulk build는 같은 endpoint run 안에서 `next_label_id`, `edge_label_id` 순서로 payload를 정렬한다. terminal label
  constraint가 있는 VLE/fixed MATCH source는 compact block의 `next_label_id`가 맞지 않으면 block 전체를
  `label-filtered`로 건너뛴다. full block은 label-homogeneous descriptor가 아니므로 block skip을 쓰지 않고
  per-posting filter로 내려간다.
- terminal property prefetch set도 main cache load 단계에서 소비한다. `property-index-prefetch`가 active이면
  cache entry를 만들기 전에 terminal vertex id가 prefetch set에 있는지 확인하고, 맞지 않는 posting은 cache에 넣지
  않는다. `Adjacency Payload Runtime`의 `cache-filtered=N`은 label block skip과 cache-load property pruning을 합친
  source/cache 폭 감소 evidence다.
- Generic property aggregate 쪽에서는 `array_agg` slot-vector handoff가 typed/agtype payload domain count와 payload
  materialization weight, final materialization weight, slot별 payload value type vector를 보관한다. VLE 직접 변경은
  아니지만, path/list consumer가 property aggregate와 결합될 때 final path materialization 직전까지 typed payload를
  유지하는 비용 모델 근거가 된다.
- aggregate handoff descriptor도 repeated key-source slot을 `heap-lookups`와 `reused`로 분리한다. 같은 raw property
  lookup을 여러 final type으로 쓰는 경우 final output slot은 유지하되 heap lookup weight는 unique key-source 기준으로
  읽을 수 있다. row-scaled aggregate credit은 total materialization weight에 `reuse-weight`를 더해 repeated source
  lookup 제거 효과를 비용 입력으로 소비한다. slot-vector partial state도 `AggGetAggref()`로 aggregate value argument
  expression을 비교해 같은 expression을 같은 source group으로 기록하고, serialize/combine boundary에서
  `source-groups/reused-slots`를 보존한다.
- aggregate benchmark는 wide text payload도 생성한다. 이는 VLE path/list consumer가 property aggregate와 결합될 때
  path materialization만이 아니라 varlena property width가 cache/source policy와 cost credit을 흔들 수 있음을 보는
  shared evidence로 유지한다.
- `cache-filtered` 합계는 `cache-label`과 `cache-property`로도 분리한다. label block skip, typed property source
  prefetch, 향후 composite seek가 같은 endpoint run에서 서로 다른 방식으로 cache 폭을 줄이기 때문에 VLE/fixed MATCH
  source policy는 이 두 counter를 따로 읽어야 한다.
- fixed MATCH의 `Adjacency Pruning`은 planner descriptor에서 온 `terminal-source=...`도 출력한다. VLE source
  descriptor와 맞추기 위해 terminal pruning을 `label-block`, `property-source`, `label-block+property-source`,
  `property-recheck`처럼 source strategy로 다룬다.
- VLE source threshold feedback은 반복 empty completion을 별도 lifecycle property로 승격한다. 첫 saturated root empty
  batch는 `adjacency-empty-batch`/headroom 35%로 남기고, 이후 같은 key에서 completion이 반복되면
  `root-empty-repeat-observed`/`adjacency-empty-lifecycle`/headroom 30%로 낮춘다. 반복 saturation은
  `root-empty-repeat-saturated`/`adjacency-empty-batch`/headroom 25%와 더 큰 batch로 반영한다. `cypher_vle`
  regression은 후속 `EXPLAIN`에서 `observed=2 saturated=1 relaxed=1`과 repeat reason을 출력하고,
  `tools/vle_benchmark.sql` smoke는 fan-out properties/vertex shape에서 repeat observed/saturated class가
  `class-match=true`로 join되는 것을 확인한다.
- terminal property source prefetch는 endpoint key binding 뒤로 이동했다. `EXPLAIN`은 실행 전
  `mode=deferred-prefetch`, 실행 후 `mode=property-index-prefetch`를 구분한다. VLE source policy도 같은 방식으로
  source descriptor와 actual frontier/run size를 나눠 보고 prefetch/cache lifecycle을 결정해야 한다.
- `Adjacency Terminal Prefetch`는 actual candidate count와 threshold를 출력한다. VLE frontier source도 같은
  run-size-aware gate를 적용하려면 단순 eligible flag가 아니라 actual frontier/run cardinality와 skipped-small
  evidence를 함께 보존해야 한다.
- `age_adjacency` v6 directory entry는 endpoint run의 terminal label distinct count를 보관한다. terminal label
  constraint가 있으면 active posting count는 전체 run fanout이 아니라 label-aware candidate estimate로 낮아지고,
  이 값이 `Adjacency Terminal Prefetch` threshold gate에 들어간다. 작은 mixed-label run은
  `property-index-prefetch` 대신 `id-btree-cache`로 남아 불필요한 global property source scan을 피한다.
- fixed MATCH의 terminal prefetch surface는 `run-count`와 label-aware `candidate-count`를 분리한다. 이는 VLE source
  policy도 전체 frontier fanout과 terminal-label slice fanout을 구분해야 한다는 evidence다. debug main probe의
  `main_label_groups`는 directory summary가 실제 endpoint run에서 몇 개 label slice를 제공하는지 확인하는
  regression surface다.
- fixed MATCH planner descriptor도 planned endpoint `fanout`과 `terminal-fanout`을 분리한다. terminal label constraint가
  있는 property prefetch threshold는 전체 endpoint fanout이 아니라 terminal fanout을 기준으로 낮춰야 하며, 이 surface는
  VLE source policy가 frontier 전체 fanout과 terminal slice fanout을 따로 비용화해야 한다는 기준이 된다.
- terminal prefetch threshold는 숫자뿐 아니라 `reason`도 출력한다. `small-terminal-fanout`은 더 넓은 property source
  prefetch가 아니라 local id/cache recheck 또는 향후 label+typed property composite seek 후보이고,
  `large-terminal-fanout`/`edge-payload-required`는 source prefetch 비용을 더 적극적으로 쓸 수 있는 class다.
- fixed MATCH의 `Adjacency Composite Source`는 terminal label slice와 property source lifecycle을 별도 line으로
  출력한다. `candidate-fanout`은 label slice 이후 fanout이고, `composite-fanout`은 property source prefetch selectivity를
  반영한 cost 후보 수다. `planned=id-cache-small|source-prefetch|none`은 runtime `Adjacency Terminal Runtime`의
  `outcome`과 비교할 수 있다.
- terminal runtime은 `outcome`과 `action`도 출력한다. `reason=small-terminal-fanout`이
  `outcome=id-cache-small action=keep-id-cache`로 이어지면 source mismatch가 아니라 의도된 small-slice lifecycle이다.
  VLE feedback도 같은 방식으로 reason과 action을 나눠야 source enum rollback을 피할 수 있다.
- terminal prefetch line은 `planned` lifecycle도 출력한다. cost는 이 planned lifecycle을 소비하므로
  `planned=id-cache-small`에서는 source prefetch selectivity credit을 주지 않는다. 이는 fixed MATCH와 VLE source
  policy 모두에서 planned lifecycle과 runtime outcome을 분리해 비교하는 기준이다.
- terminal runtime은 `lifecycle-match=true|false`도 출력한다. fixed MATCH terminal property source도 VLE source
  policy의 `source-match`/`class-match`와 같은 방식으로 planned lifecycle과 actual outcome mismatch를 드러낸다.
- fixed MATCH `Adjacency Pruning`은 `index-solved=N residual-count=N`도 출력한다. ORCA식 residual predicate count와
  Neo4j식 index-compatible predicate 분리를 AGE EXPLAIN에 맞춘 surface로 올린 것이며, 다음 cost/feedback 작업은
  boolean flag가 아니라 이 count를 직접 소비한다. 현재 adjacency match cost도 residual count로 CPU verification weight를
  올리고 source-solved count로 verification credit을 적용한다. `Adjacency Cost Input`은 이 weight/credit을 percent
  surface로 출력해 cost input과 pruning descriptor를 같은 regression에서 비교하게 한다. planned prefetch threshold와
  reason도 non-ANALYZE plan surface에 함께 출력한다.
- `AGE Adjacency Match` terminal property descriptor는 `value=const|runtime-slot|none`과
  `prefetch=eligible|ineligible`을 출력한다. runtime expression이라도 현재 CustomScan `required_outer` 안에서
  평가할 수 있으면 property index prefetch source로 유지하고, 바깥 MATCH 변수에 의존하면 prefetch source를 제거해
  residual join verification으로 남긴다.
- property source index metadata의 `options.property_type`은 terminal descriptor의 `domain=...`으로 출력된다.
  fixed MATCH terminal property prefetch는 이제 실제 property source index key type을 기준으로 `agtype`,
  `int8`, `float8`, `numeric`, `text` scan key를 만든다. VLE terminal property batching도 같은 원칙으로 확장할
  수 있도록 domain 문자열이 아니라 index/key descriptor를 source contract의 기준으로 둔다.
- `age_adjacency_debug_main_probe()`는 VLE/fixed MATCH가 공유하는 main payload cursor의 run-window cache evidence를
  출력한다. `main_block_items < main_page_offsets`이면 logical postings보다 page item 수가 줄어든 packed run block
  layout이 적용된 것이고, `main_compact_block_items > 0`이면 block-local graphid compression이 실제 index payload에
  들어간 것이다.
- VLE visible payload cursor와 fixed payload scan은 delta page opaque의 `min_key/max_key/posting_count`를
  page-level negative filter로 공유한다. insert delta가 여러 page로 커질 때 active key가 들어갈 수 없는 page를
  tuple loop 전에 건너뛴다.
- delta page도 full key triple을 item마다 저장하지 않는다. page opaque가 key/edge/next label id triple을 보관하고,
  delta item은 key/edge/next 48-bit entry id와 TID만 저장한다. 새 insert의 label triple이 현재 delta page와 다르면
  새 page를 시작해 page-local compact invariant를 유지한다. VLE/fixed MATCH cursor는 page header와 item payload로
  full graphid를 재구성한다.
- `age_adjacency_debug_delta_probe()`는 같은 range filter를 기준으로 delta page visited/skipped와 tuple scan 수를
  출력한다. VLE regression은 raw plan assertion 대신 이 lifecycle evidence를 통해 delta skip이 실제로 scan 폭을
  줄였는지 확인할 수 있다.
- `age_adjacency` AM cost hook도 graphid constant endpoint qual이면 delta probe의 tuple scan 수를 delta CPU cost에
  사용한다. page chain 확인 비용은 유지되지만, VLE/fixed MATCH planner는 clustered delta에서 range skip이 줄이는
  tuple loop 비용을 source/index decision에 반영할 수 있다.
- `age_adjacency_debug_delta_maintenance()`는 delta page 상태를 `none`, `observe-delta`, `range-skip-delta`,
  `reindex-delta` action으로 정규화한다. VLE benchmark가 threshold boolean 대신 action/reason을 보면, source
  policy 문제와 index maintenance 문제를 분리해서 해석할 수 있다.
- `age_adjacency_reindex_if_needed()`는 같은 action을 소비하는 명시적 maintenance entry point다. threshold를 넘은
  delta는 PostgreSQL reindex로 main run에 다시 병합되고, benchmark harness는 수동 `REINDEX` 대신 이 function을
  호출한다.
- graph index DDL은 helper-only가 아니라 Cypher parser surface를 우선한다.
  `CREATE INDEX name FOR ()-[r:TYPE]->() ON (ADJACENCY)`는 outgoing `age_adjacency(endpoint,id,next)`
  source index를 만들고 metadata에 direction/source provider를 기록한다. `DROP INDEX name`은 실제 graph
  schema index와 metadata를 함께 제거하고, `SHOW INDEXES`는 graph-local source index 목록을 raw table
  surface로 보여준다.
- fixed MATCH는 const-bound endpoint에서 `age_adjacency` directory summary를 planner cost input으로도 소비한다.
  `Adjacency Index`와 `Adjacency Cost Input`은 `label-groups`와 `fanout-source=directory|statistics`를 출력한다.
  VLE source policy도 같은 방향으로 frontier key가 안정적으로 좁혀진 경우 directory-backed terminal label 후보 수를
  먼저 보고, global property source scan이나 cache materialization으로 넘어가야 한다.
- VLE stream source descriptor는 이제 start/end endpoint arg가 const graphid이거나 해당 vertex relation에
  `id = const` restriction이 있으면 `age_adjacency` directory entry의 endpoint run posting 수를 fanout evidence로
  사용한다. `VLE Source Cost`는 `source=start:directory/end:statistics`를 출력해 relation statistics와 directory
  evidence를 구분한다. 이는 fixed MATCH directory fanout과 VLE frontier source policy를 같은 vocabulary로 묶는
  단계이며, 다음 단계는 terminal label/property composite seek까지 같은 descriptor에 올리는 것이다.
- terminal label constraint가 VLE output descriptor에 있으면 source cost도 endpoint run 전체가 아니라 해당 label
  posting 수를 사용한다. `source=start:directory-label`은 directory entry에서 terminal label slice가 fanout을 줄인
  경우이며, mixed terminal label regression은 `reltuples=2 fanout=start:1`로 이 contract를 고정한다.
- VLE와 fixed `AGE Adjacency Match`는 property source selectivity helper를 공유한다. helper는 property source
  btree index expression stats의 MCV와 `stadistinct`를 읽고, const predicate가 MCV에 있으면 MCV frequency를 먼저
  본다. `VLE Composite Fanout`은 property-prefilter가 planned일 때
  `selectivity=... selectivity-source=fallback|typed-mcv|typed-distinct|fallback-mcv-ceiling|fallback-ceiling`을
  출력한다. `vle_typed_selectivity` regression은 `ANALYZE`된 skewed `n.i` property source에서
  `selectivity-source=typed-mcv`를 고정하고, 기존 `n.rare` fixture는 common MCV가 fallback보다 넓은 경우
  `fallback-mcv-ceiling`으로 fanout 확대를 막는 경로를 고정한다.
- property source prefetch 결과는 `AgeAdjacencyVertexSetFilter`로 `age_adjacency` payload scan에 전달된다. 이
  descriptor는 hash set, min/max range, sorted vertex id vector를 함께 들고, payload scan은 range reject 뒤
  sorted binary search로 in-range non-match를 제거한다. `VLE Payload Runtime`은 실제 reject가 발생하면
  `set-range-filter=N`과 `set-sorted-filter=N`을 출력한다. 현재 단계는 hash callback 제거와 membership 비용 절감이며,
  sorted vector는 compact main block precheck에도 들어간다. compact block의 terminal label이 고정되어 있고 packed
  next entry id를 읽을 수 있으면 property matched set과 교차하지 않는 block을 cache fill 전에 건너뛴다. 이 경우
  runtime은 `set-block-filter=N`을 출력한다. range/exact/bloom summary를 모두 통과한 뒤 payload-order compact posting
  intersection이 block을 접은 경우는 같은 총량 뒤에 `/posting:N`을 붙여, summary skip과 residual exact posting skip을
  구분한다. v15 compact block compressed entry vector가 graphid exact overflow 뒤에 negative를 증명하면
  `/compressed:N`을 붙인다. directory entry도 v7부터 endpoint run의
  `min_next_vertex_id/max_next_vertex_id`를 보관하므로, prefetch range와 run range가 겹치지 않으면 main run을 열지
  않고 `set-directory-filter=N`으로 접는다. v14 directory wide bloom이 64-bit directory bloom false positive를 다시
  접으면 runtime은 같은 총량 뒤에 `/wide-bloom:N`을 붙인다. VLE는 run count를 얻은 뒤 prefilter set을 붙일 수 있으므로
  `age_adjacency_visible_payload_scan_set_terminal_vertex_set_filter()`가 active directory entry를 다시 확인한다.
  `age_adjacency_visible_payload_scan_key_known_empty()`도 같은 range descriptor를 소비하므로 no-delta frontier key가
  property prefilter range 밖이면 source-run precheck에서 known-empty로 올릴 수 있다. 다음 단계는 같은 descriptor를
  label+property composite seek request나 directory-level value summary로 더 올리는 것이다. directory range miss로
  candidate가 0이 된 VLE source는 더 이상 일반 `empty-scan`으로 feedback되지 않고 `evidence=directory-filter`로
  정규화된다. 이는 `set-directory-filter=N` payload runtime과 같은 사건을 source feedback vocabulary에도 올려,
  composite property prefilter 성공을 source policy 실패로 오해하지 않게 한다. 같은 backend 안에서 후속
  composite-planned VLE plan은 이 evidence를 `VLE Source Payload Input`의 `source=runtime-cache`와
  `class=adjacency-composite-prefilter`로 읽는다. directory-filtered 0-candidate scan은
  `reason=payload-directory-filter-observed`, 일반 property prefilter scan은
  `reason=payload-composite-prefilter-observed`로 분리한다. VLE와 fixed `AGE Adjacency Match`는 이 label+property
  handoff를 `AgeAdjacencyCompositeTerminalFilter`로 넘긴다. 이 request는 terminal label id, property-source vertex
  set, callback fallback을 함께 담으므로 다음 directory-level value summary도 setter 순서가 아니라 request field로
  추가할 수 있다. property prefilter handoff는 property source index OID, VLE property filter id, prefetched match
  count도 request summary로 싣고, VLE runtime은 summary가 실린 handoff를 `composite-request=N`으로 출력한다.
- `AgeAdjacencyVertexSetFilter`는 composite request의 `property_filter_id`가 있으면 matched vertex id set을
  request-side value summary bloom으로도 준비한다. 이 summary는 on-disk value posting layout은 아니지만, terminal
  property value identity가 있는 request만 directory entry의 64-bit/wide next-vertex bloom과 먼저 교차한다.
  negative이면 main block을 열기 전에 `set-directory-filter=.../value-summary:N`으로 접고, fixed
  `AGE Adjacency Match`와 `AGE VLE Stream` runtime formatter가 같은 suffix를 출력한다. 따라서 wide bloom skip과
  value-identity request skip을 분리해, 다음 on-disk terminal value posting summary가 실제로 어느 false positive를
  더 줄였는지 비교할 수 있다.
- 같은 request-side value identity는 block summary skip에도 겹쳐 기록된다. range/exact/compressed/bloom/posting 중
  어느 block summary가 실제 skip을 만들었는지는 기존 suffix가 보존하고, 그 skip이 property value identity request에서
  발생했는지는 `set-block-filter=.../value-summary:N`으로 따로 본다. `age_adjacency_debug_composite_probe()`는
  `set_block_value_filter`를 반환해 directory-only skip과 block-level value request skip을 한 row에서 비교한다.
- `age_adjacency` v16 directory entry는 homogeneous terminal label run에 대해 48-bit compressed `next_entry_id`
  vector를 저장한다. exact graphid slot이 overflow된 run도 compressed vector가 property matched set과 교차하지 않으면
  main block을 열지 않고 `set-directory-filter=.../compressed:N`으로 접는다. wide bloom fixture는 same 19 posting
  skip을 `compressed`, `wide-bloom`, `value-summary` 축으로 모두 드러내므로, 이후 value identity별 posting summary가
  단순한 next-entry exact summary인지 실제 value-keyed summary인지 비교할 수 있다.
- property value identity는 `age_adjacency_property_filter_id()`에서 생성한다. VLE payload cache와 fixed
  `AGE Adjacency Match` terminal property lookup은 모두 property source index OID와 predicate value image hash를
  같은 방식으로 섞고, prefetch matched vertex set을 `AgeAdjacencyCompositeTerminalFilter.property_filter_id`와 함께
  내려보낸다. 따라서 source가 VLE인지 fixed MATCH인지와 무관하게 같은 value predicate는 같은 request identity를 가진다.
- fixed `AGE Adjacency Match`의 `Adjacency Terminal Runtime`은 이 handoff를 값 자체나 hash 없이
  `value-identity=present|none`으로 출력한다. 이는 not-indexable recheck path와 value-keyed composite request를
  EXPLAIN surface에서 분리해 다음 directory/block value posting summary가 실제 request identity를 소비하는지 확인하기
  위한 evidence다.
- directory-level payload pruning counter는 이제 `set-directory-filter=N` 총량 뒤에
  `/range:N`, `/exact:N`, `/label-bloom:N`, `/compressed:N`, `/value-summary:N`, `/wide-bloom:N` suffix를 붙인다.
  VLE composite prefilter regression의 0-candidate case는 `set-directory-filter=7/range:7`로 고정되어,
  해당 skip이 value identity posting summary가 아니라 prefetch vertex range와 directory run range의 불일치였음을
  raw `EXPLAIN`에서 바로 확인할 수 있다. fixed `AGE Adjacency Match` debug SRF도 같은 column set을 반환하므로
  VLE와 one-hop MATCH가 동일한 adjacency pruning vocabulary를 공유한다.
- storage v17은 request-gated value-posting bloom을 directory entry와 main run block에 저장한다. 이 summary는
  property value 자체를 저장하지 않고, `property_filter_id`가 있는 prefetch request의 matched vertex set과
  endpoint posting set을 별도 512-bit bloom에서 교차 확인한다. runtime은 기존 `/value-summary` 뒤에
  `/value-posting:N`을 붙인다. `age_adjacency_value_posting` regression은 기존 64/256-bit value summary가 false
  positive로 통과하지만 새 directory value-posting bloom이 34 postings를 접는 case를 고정한다.
- storage v18은 directory의 작은 terminal-label bloom slot마다 value-posting bloom도 같이 저장한다. 따라서
  mixed-label endpoint run에서도 request terminal label이 slot에 있으면 전역 run bloom 대신 label-slice posting
  bloom을 소비해 directory 단계에서 false positive를 줄인다. `age_adjacency_label_value_posting` regression은
  label 1/2가 섞인 run에서 label 1 request가 `/value-posting:38` directory skip을 만드는지 고정한다. fixed
  `AGE Adjacency Match` planner descriptor도 `value-posting=label-slice|run|none`을 cost input과 composite source에
  싣는다.
- `AGE VLE Stream` source descriptor도 같은 value-posting source evidence를 싣는다. `VLE Source Cost`는
  `value-posting=start:label-slice/end:none`처럼 directory fanout evidence와 terminal label slice value-posting
  source를 함께 출력하고, composite property prefilter가 planned된 경우 `VLE Source Policy`도
  `value-posting=out:label-slice/in:none`을 출력한다. planner class는 여전히
  `adjacency-composite-prefilter`로 유지한다. 이는 summary availability와 actual negative evidence를 분리하기
  위한 contract이며, runtime에서 실제 directory/block value-posting skip이 발생하면 `VLE Payload Runtime`의
  `/value-posting:N` counter와 `VLE Source Runtime` pressure `adjacency-composite-value-posting` /
  action `keep-value-posting`으로 드러난다. benchmark summary는 `value_posting_source` column을 추가해 planned
  source와 runtime counter를 같은 row에서 비교할 수 있다.
- value-posting runtime evidence도 backend-local planner feedback cache에 연결했다. 첫 `EXPLAIN ANALYZE`에서
  `VLE Payload Runtime`이 `/value-posting:N` 또는 request-side value identity 기반
  `/value-summary:N` block/directory pruning을 출력하면 후속 plan은 `VLE Source Payload Input`에
  `value-posting=label-slice/observed:N reason=payload-value-posting-observed
  class=adjacency-composite-value-posting`을 싣고, `VLE Source Policy`는
  `recommendation=keep-value-posting`으로 승격한다. `vle_value_posting_feedback` regression은 실제 endpoint가
  아니지만 label/value summary false positive를 통과하는 terminal property 후보를 만들어 directory
  value-posting bloom이 38 postings를 접는 경로를 고정한다.
- 이 승격은 source availability가 아니라 observed negative evidence에만 반응한다. 아직 cache key는 value identity를
  raw EXPLAIN에 노출하지 않는 family 단위이므로, 다음 단계는 terminal label, property source index, value identity,
  value-posting source class를 cache discriminator로 올려 다른 predicate의 false promotion을 막는 것이다.
- value-posting feedback cache key는 terminal label id, property source index OID, `property_filter_id`, active
  direction, value-posting source class를 discriminator로 갖는다. `property_filter_id`는 raw value hash를
  EXPLAIN에 출력하지 않고 backend-local key에만 쓰므로 expected는 value 자체나 hash encoding에 묶이지 않는다.
  `vle_value_posting_feedback` regression은 `n.i = 59` 실행이 만든
  `class=adjacency-composite-value-posting` feedback을 같은 predicate 후속 plan만 소비하고, `n.i = 1` 후속 plan은
  `VLE Source Payload Input: source=none ... value-posting=none/observed:0`으로 남는지 고정한다.
- `tools/vle_benchmark.sql` decision table은 value-posting source availability, identity-safe payload input,
  runtime `/value-posting:N` counter를 분리해 출력한다. `value_posting_decision`은 planner-side input을
  `identity-cache-hit|source-available|none`으로 분류하고, final summary의
  `value_posting_runtime_decision`은 `identity-cache-hit|runtime-hit|source-available|none`으로 실제 실행 결과까지
  합친다. 후속 source policy reason도 final summary의 `policy_reason`으로 전달하고,
  `composite-value-posting` reason은 `value_posting_policy_decision=policy-value-posting`으로 분리한다. 따라서
  benchmark에서 source availability, planner policy adoption, observed value-posting success, predicate-safe cache
  reuse를 같은 row에서 비교할 수 있다.
- `tools/vle_benchmark.sql`은 `value_posting_edges` fixture도 만든다. `ValuePostingNode`는 직접 graphid local id를
  지정해 start vertex, 실제 endpoint, non-endpoint property 후보를 분리하고,
  `ValuePostingEdge`는 per-root endpoint run을 만든다. 각 root의 posting run은 256개 이하로 제한해
  `age_adjacency` per-run append 한계를 피하고, final summary는 `value_posting_root_count`와
  `value_posting_edges_per_root`를 출력한다. `ValuePostingNode.i` property source index와
  `ValuePostingEdge` `age_adjacency` index를 같이 생성하므로 VLE terminal-property plan이
  property-prefilter와 payload directory/block pruning을 모두 소비한다.
- benchmark value-posting shape는 `value-posting-reject-seed`, `value-posting-reject`,
  `value-posting-endpoint-control`로 나뉜다. reject shape는 `n.i = 59` 후보가 endpoint run에 없어서
  `set-directory-filter=N/value-summary:N/wide-bloom:N` runtime evidence를 만들고, summary는 기존
  `/value-posting:N`과 이 value-summary/wide-bloom suffix를 모두 `payload_value_posting_filtered`로 읽는다.
  endpoint-control은 `n.i = 1` 실제 endpoint를 조회해 `value_posting_runtime_hit=false` control을 제공한다.
- 이 shape는 Cypher result를 다시 SQL `count(*)`로 감싼 run wrapper에서는 현재 planner/executor target-list issue를
  밟으므로 EXPLAIN ANALYZE capture-only로 둔다. benchmark final join은 plan/runtime evidence 중심이므로
  `rows_returned`가 없는 row를 허용하고, value-posting policy 보정은 `payload_value_posting_filtered`,
  `value_posting_runtime_hit`, `value_posting_runtime_decision`을 기준으로 본다.
- 1024-edge benchmark는 4 roots x 256 postings로 실행된다. `value-posting-reject`는
  `payload_input_value_posting_observed=1024`, `class=adjacency-composite-value-posting`,
  `recommendation=keep-value-posting`을 출력하고, endpoint-control은 같은 source/index에서
  `value_posting_runtime_hit=false`로 남는다. 이 결과는 value identity pruning을 endpoint-btree rollback 신호가
  아니라 `age_adjacency` composite payload source 유지 신호로 소비해야 한다는 근거다.
- payload feedback cache entry는 replay/cache seed/value identity pruning을 마지막 관측값 하나로 덮어쓰지 않는다.
  scan/replay/seed/value-pruning count는 누적하고, `payload_class`/`payload_reason`은
  `adjacency-composite-value-posting > adjacency-composite-prefilter > adjacency-replay > ...` rank에서 더 강한
  property를 보존한다. 이는 ORCA `CReqdColsRequest`처럼 request boundary를 key로 삼고, provided lifecycle property는
  merge하는 구조에 맞춘 것이다.
- VLE runtime value-posting feedback은 source-aware counter를 사용한다. vertex-set value counter가 있으면 그 값을
  그대로 쓰고, 없더라도 active source가 `value-posting=label-slice|run`이면 terminal-property cache/property/composite
  filter count를 observed value-posting pruning으로 승격한다. source가 `value-posting=none`이면 같은 filter counter가
  있어도 value-posting feedback으로 보지 않는다. `vle_value_posting_feedback`의 all-depth terminal-property shape는
  후속 plan에서 `scan-runs=1 seed-runs=1 value-posting=label-slice/observed:8
  class=adjacency-composite-value-posting`을 함께 출력해 cache seed lifecycle과 value-posting evidence가 같은 profile
  key에 쌓였음을 고정한다.
- `vle_fixed_chain_value_replay`는 fixed label-chain collapse가 만든 `terminal-label=3/all-depth` VLE에서 source
  replay까지 같은 key에 누적되는지 고정한다. 두 경로가 같은 source vertex로 합류하고 terminal fanout에 다른 label
  decoy를 둔 fixture라서 첫 실행은 `VLE Payload Runtime`에 `runs=scan:4/replay:1/seed:2`와
  `cache-filter=16/16/0`을 남긴다. 후속 plan은 `VLE Source Payload Input`에
  `replay-runs=1 seed-runs=2 value-posting=label-slice/observed:16
  class=adjacency-composite-value-posting`을 함께 출력한다.
- observed value-posting은 planned `adjacency-composite-prefilter`보다 강한 runtime-provided property다. 따라서 첫
  실행에서도 `VLE Source Runtime`은 `class=adjacency-composite-value-posting`을 출력하고,
  `VLE Source Plan`은 planned class가 `adjacency-composite-prefilter`여도 `class-match=true`로 정규화한다. 이는
  value-posting success를 planner/source mismatch가 아니라 같은 composite source lifecycle의 강화 evidence로 보기
  위한 contract다.
- 후속 planning은 같은 feedback을 `VLE Source Payload Input`에서만 보여주지 않고 directed source policy reason에도
  반영한다. `reason=out:composite-value-posting`은 `age_adjacency` source 선택이 단순 property prefilter availability가
  아니라 observed value-posting lifecycle을 소비해 유지됐다는 뜻이다.
- value-posting observed class는 replay/cache seed가 같이 있을 때만 headroom을 낮추는 보조 signal이 아니다.
  backend-local payload feedback이 `adjacency-composite-value-posting`이면 materialization weight별 value-posting
  headroom을 직접 적용한다. terminal-scalar는 `headroom=25`, terminal-object는 `20`, path는 `18`을 쓰며, replay나
  seed evidence가 같은 entry에 있으면 더 낮은 쪽을 유지한다. 따라서 후속 `VLE Source Profile`의
  `endpoint-headroom`만 봐도 value-posting lifecycle이 dense-local/`age_adjacency` 전환 기준에 들어갔는지 확인할 수
  있다. benchmark summary도 `value_posting_headroom_expected`와 `value_posting_headroom_applied`를 출력해,
  `value_posting_policy_decision=policy-value-posting`이 실제 threshold/headroom decision으로 이어졌는지 threshold
  table에서 바로 확인한다.
- benchmark final summary는 `source_policy_outcome`과 `source_policy_next_action`도 출력한다. 이 값은
  source handoff mismatch, runtime class mismatch, value-posting headroom 적용/승격 후보, directional family split 후보,
  empty lifecycle 유지, replay headroom 유지를 한 vocabulary로 접는다. threshold 숫자를 바꾸기 전에는 이 outcome이
  `source-mismatch`나 `class-mismatch`인지 먼저 보고, mismatch가 없을 때만 headroom/batch/source family를 조정한다.
- composite prefilter plan에서 property candidate가 실제 reachable endpoint가 아니어서 missing-vertex source evidence만
  남아도, dominant source가 planned `age_adjacency`이고 source suppression이 match되면 runtime class는
  `adjacency-composite-prefilter`로 유지한다. 이는 property prefilter가 실패했다는 뜻이 아니라 composite source가
  negative endpoint probe를 제공했다는 evidence다.
- anonymous edge label source planning은 `_ag_label_edge`를 default edge relation으로 본다. `_ag_label_edge`에
  direction별 `age_adjacency` index가 있으면 `VLE Edge Source`는 `local-index-candidate`와
  `fixed-source=out:age-adjacency/...`로 출력되고, runtime `VLE Source Plan`은 `source-match=true`가 된다.
  `_ag_label_edge`에 endpoint btree만 있으면 anonymous VLE는 dense-local source로 승격하지 않고 global metadata path를
  유지한다.
- `VLE Source Policy`의 `reason=...unknown-fanout...`은 cost evidence 부족을 뜻한다. 이 경우 planned class는
  `unknown-fanout`이 아니라 실제 chosen family인 `adjacency-stream`이며, recommendation만
  `collect-endpoint-stats`로 둔다.
- `active=both` request가 directional family threshold input을 소비하고 empty completion이 한쪽 방향으로 치우치면
  source policy는 partial split을 적용한다. empty 쪽은 `age_adjacency`를 유지하고 productive 반대 방향은
  `endpoint-btree`로 바꾸며, policy reason은 `directional-family-productive`로 남긴다. 이 split은 source family를
  새 class로 만들지 않고 `adjacency-empty-batch` 같은 lifecycle class를 유지해 runtime `class-match=true`를 보존한다.
  benchmark summary는 이를 `directional-family-split-applied`로 분류한다.
- 단기 micro fast path보다 typed/scalar descriptor handoff와 dense traversal state contract를
  우선한다.
- `VLE Source Threshold Input`과 `VLE Source Payload Input`은 runtime-cache 입력을 `reason`과 `class`로 분리한다.
  `reason=root-empty-saturated class=adjacency-empty-batch`,
  `reason=payload-replay-ratio-observed class=adjacency-replay`처럼 출력되며,
  planner feedback은 reason 문자열 비교보다 class를 우선 사용한다. 이는 ORCA식 required/provided property
  handoff에 맞춘 구조로, repeated empty completion과 payload replay를 source enum rollback이 아니라
  lifecycle/threshold property로 소비하게 한다.
