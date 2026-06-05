# VLE Optimization Log

## Goal

VLE(variable-length edge/path) 실행에서 불필요한 path/list/entity/property
materialization과 graph table refetch를 줄인다. 최종 출력 semantic이 필요한 경우만
`agtype` container를 만들고, count/length/endpoint/property-only consumer는 raw descriptor
또는 direct helper로 처리한다.

## 현재 VLE execution boundary

- Parser/lowering 단계에서 consumer shape를 분류한다.
- `length(p)`, `count(p)`, endpoint-only, property-only consumer는 materialized path를
  요구하지 않는 경로로 낮춘다.
- Full `nodes(p)`, `relationships(p)`, path variable projection, arbitrary list element
  projection은 semantic 보존을 위해 materialization fallback을 유지한다.
- Fixed path arity-specific helper(`_agtype_build_path_label5/7/9` 등)는 제거했고 다시
  늘리지 않는다.
- 최종 path output은 raw descriptor와 `_agtype_build_path_raw` 계열을 사용한다.

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
  후속 planning은
  `payload-input=runtime-cache/.../replay-runs:1/.../replay-percent:25/seed-percent:50/.../reason:payload-replay-ratio-observed`
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
- threshold input도 physical lifecycle vocabulary다. `root-empty-saturated` feedback을 소비한 plan은
  `adjacency-empty-batch` / `keep-empty-batch`를 출력하고, replay run이 없는 runtime empty evidence는 planned
  empty lifecycle class와 맞춘다. payload replay run이 있으면 replay가 더 강한 source reuse signal이므로
  `adjacency-replay`가 우선한다.

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
- planner source-cost 검증은 `VLE Edge Source`의 `cost=reltuples=... fanout=...` 출력으로 유지한다.
- source cost policy 검증은 같은 `VLE Edge Source` line의
  `policy=... endpoint-work=sum(...) reason=...` 출력으로 유지한다. fanout statistics 신뢰도는 같은 line의
  `stats=rel:.../start:.../end:...` 출력으로 확인한다.
- planner source policy의 threshold 보정 방향은 같은 line의 `class=... recommendation=...` 출력으로 확인한다.
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
- payload run evidence는 backend-local source feedback cache에도 들어간다. `VLE Edge Source`는 다음 planning에서
  읽은 payload feedback을
  `payload-input=none|runtime-cache/headroom:N/scan-runs:N/replay-runs:N/seed-runs:N/observed:N/reason:...`
  으로 출력한다. 이 값은 raw runtime counter가 아니라 planner profile이 소비한 input property다.
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
- `AgeVLEStreamEdgeSource` descriptor는 planner가 읽은 feedback input을 `threshold_input_known`,
  `threshold_input_headroom_percent`, `threshold_input_batch_size`, `threshold_input_observed_count`,
  `threshold_input_saturated_count`, `threshold_input_relaxed_count`, `threshold_input_source`,
  `threshold_input_reason`으로 보관한다.
  `VLE Edge Source`는 `threshold-input=none|runtime-cache/headroom:N/batch:N/source:.../reason:...`을 출력하고,
  `threshold-cache=observed:N/saturated:N/relaxed:N`으로 cache state를 함께 출력한다. `tools/vle_benchmark.sql`은
  planner summary와 planner/runtime join summary에서 이 값을 추출한다.
- `vle_empty_cache_policy` regression은 `EXPLAIN ANALYZE`로 saturated
  `threshold-feedback=eligible/headroom:35/batch:16/source:out/reason:root-empty-saturated`를 만든 뒤 같은
  backend에서 후속 `EXPLAIN`을 실행해
  `empty-batch=eligible/size:16 threshold-input=runtime-cache/headroom:35/batch:16/source:out/reason:root-empty-saturated threshold-cache=observed:1/saturated:1/relaxed:0`
  를 expected에 남긴다. 이 regression은 plan assertion이 아니라 raw CustomScan evidence로 planner feedback
  handoff와 batch feedback 적용을 고정한다.
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
- 단기 micro fast path보다 typed/scalar descriptor handoff와 dense traversal state contract를
  우선한다.
