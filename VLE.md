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
- `VLE Source Runtime`은 raw counter 뒤에 `feedback=dominant=... yield=... replay=... push=...`를 출력한다.
  이 feedback은 `age_vle_source_cost`에서 포맷하며, endpoint-btree와 `age_adjacency` 선택의 runtime
  evidence를 expected output에 직접 남긴다.
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
     `cost=reltuples=... fanout=start:.../end:...` 형태로 출력한다. 남은 후보는 이 planner evidence를 source
     cost adjustment와 runtime feedback cache가 실제로 읽게 하는 구조다. 현재 fanout evidence는
     `VLESourceFanoutEvidence`로 공통화되어 stream descriptor, local edge-state capacity, `age_adjacency`
     custom path costing이 같은 relation/fanout vocabulary를 읽는다. local-index-candidate plan은 planner-only
     cost policy도 함께 출력해 endpoint-btree/`age_adjacency` recommendation을 visible regression에 남긴다.
     descriptor에는 text와 directed enum recommendation을 같이 보관한다.
     cached context refresh도 같은 policy descriptor를 유지한다. multi-loop fixture는 5개 start vertex에 대해
     같은 `AGE VLE Stream`을 반복 실행하고, `VLE Source Runtime`은 마지막 loop가 아니라 전체 누적
     `endpoint-btree=5/6`을 출력한다.
     source policy explain은 `endpoint-work=sum(current/limit)`과 `reason=out:.../in:...`을 출력한다.
     low fanout/depth 2 fixture는 `out:6/6 reason=out:endpoint-work`으로 endpoint-btree를 유지하고,
     fanout 3/depth 2 fixture는 `out:12/6 reason=out:work-exceeds-limit`으로 `age_adjacency`를
     선택한다. 이 값은 leaf depth 하나가 아니라 bounded traversal 전체의 누적 branch work다. depth 2 cap은
     제거했고, bounded/property-constraint-free VLE는 finite upper 전체에 costed policy를 적용한다. fanout 3,
     depth 3 fixture는 `out:39/14`를 출력해 큰 bounded fan-out에서도 `age_adjacency` source가 plan descriptor로
     설명되는지 확인한다.

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
  `policy=... endpoint-work=sum(...) reason=...` 출력으로 유지한다.
- source cost policy handoff는 `AGE VLE Stream` edge-source descriptor의 directed policy enum을 기준으로
  진행한다. runtime source layout이 fanout/statistics를 다시 계산하지 않고 descriptor recommendation을 읽는
  구조를 유지한다.
- source cost policy는 depth 2 이하로 제한하지 않는다. finite upper 전체에 누적 work를 적용하되, infinite upper와
  property constraint가 있는 shape는 layout decision으로 둔다.
- runtime source feedback은 CustomScan execution 전체 누적값으로 본다. nested-loop 반복 실행에서 마지막 iterator
  snapshot만 남기는 형태는 cost 보정 근거로 사용하지 않는다.
- runtime source feedback은 dominant source뿐 아니라 source별 scan density도 같이 본다. scan density가 낮으면
  source setup/empty scan 비용을 줄이는 방향으로 threshold나 fallback suppression을 조정한다.
- source fanout 산정은 `VLESourceFanoutEvidence`를 기준으로 유지하고 caller-local statistics 조합을 다시
  늘리지 않는다.
- 단기 micro fast path보다 typed/scalar descriptor handoff와 dense traversal state contract를
  우선한다.
