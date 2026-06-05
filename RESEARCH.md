# VLE/RPQ Research Notes

## 목적

이 문서는 AGE Cypher/VLE/property 최적화 중 방향을 바꾼 근거, 참고한 PostgreSQL
source/project/paper, 그리고 되돌린 접근의 결론을 기록한다. 상세 진행 로그는
`HISTORY.md`, VLE 구조와 benchmark는 `VLE.md`에 둔다.

## PostgreSQL planner/executor boundary

- PostgreSQL 18 source 기준 `CustomPath`/`CustomScan`은 path target, custom expr,
  qual, reparameterization contract를 명확히 맞춰야 planner와 executor가 안정적으로
  상호작용한다.
- AGE Cypher expression을 PostgreSQL 표준 expression으로 더 많이 낮출수록 plan cache,
  index matching, invalidation, costing을 PostgreSQL에 맡길 수 있다.
- 단순 helper rewrite가 expression index surface를 바꾸면 no-index workload는 빨라져도
  indexed workload가 seq scan으로 떨어질 수 있다. 그래서 property predicate rewrite는
  index-aware guard를 둔다.

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
  (`work-exceeds-limit`), 또는 source availability fallback인지(`endpoint-only`, `unknown-fanout`,
  `no-source`, `layout`)를 regression-visible contract로 남긴다.
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
- cached-property projection도 같은 descriptor 안에서 중복 key path를 식별해야 한다. ORCA식 computed column
  reuse 관점에서 같은 `(container, key path)`를 여러 physical type으로 최종 변환할 때 heap properties object를
  다시 탐색할 필요가 없다. executor slot descriptor가 이전 slot을 source로 참조하면 raw property lookup과 final
  type conversion boundary가 분리되고, `Cached Property Slots` explain이 reuse 관계를 직접 보여준다.

## 되돌린 접근 요약

- Generic comparator micro fast path 반복: benchmark 개선 폭이 작아 구조 후보로 전환.
- Sort key를 final output으로 직접 재사용: cast semantic이 바뀔 수 있어 되돌림.
- Lower path가 output `agtype`를 함께 carry: sort width 증가로 악화되어 되돌림.
- Broad terminal node skip: chained `*0..0` path를 깨뜨려 단일 VLE segment + constrained
  start 조건으로 제한.
- Helper-only property predicate rewrite: expression index matching을 깨뜨려 index-aware
  rewrite로 대체.
