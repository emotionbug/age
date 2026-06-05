# VLE Follow-up TODO

## 진행 원칙

- TODO는 남은 작업, 바로 다음 실행 계획, 검증 기준만 유지한다.
- 완료한 작업은 `HISTORY.md`, 설계 판단과 조사 근거는 `RESEARCH.md`, VLE 실행
  구조/cache layout/traversal state/benchmark/regression coverage는 `VLE.md`에 남긴다.
- workspace note 문서 언어, 문서별 역할, commit message, push, macOS/WSL test 절차는
  `AGENTS.md`를 기준으로 적용한다.
- TODO 항목에서 설계, 조사, 검토, 측정이 필요하면 준비 단계에서 멈추지 않는다. 같은
  작업 단위 안에서 근거 기록, 코드/SQL/test 변경, focused 검증까지 이어서 진행한다.
- 실패를 이유로 되돌리는 것을 기본 행동으로 삼지 않는다. 실패 shape와 깨진 contract를
  분리하고 descriptor boundary, source/index/cache layout, workload shape를 조정해 돌파구를
  만든다.
- guard/assertion 추가만으로 문제를 덮지 않는다. source kind, resolved signature,
  descriptor, planner/executor contract가 더 넓은 semantic을 올바르게 처리하도록 바꾼다.
- regression에서 plan shape를 검증할 때는 hidden assertion보다 `EXPLAIN (VERBOSE, COSTS OFF)`
  또는 실제 query result를 expected에 드러내는 방식을 우선한다.
- 작은 synthetic 데이터셋에서 효용이 작아 보여도 row width 감소, materialization 지연,
  required column 축소, cache/load contract 개선처럼 큰 데이터셋에서 효용이 커질 구조 변경을
  우선한다.
- 구현 전 최우선 참고 소스는 `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca`다. ORCA의
  required/computed column, physical property, pruning 구조에서 먼저 insight를 얻는다.
- graph planning/materialization 구조는 `/Users/emotionbug/IdeaProjects/neo4j`도 함께 참고한다.
  Cypher planner, physical planning, slotted runtime, var-expand pruning 구조를 우선 본다.
- 한 파일에 여러 기능을 계속 쌓지 않는다. 책임이 섞이면 같은 작업 단위에서 파일 또는
  모듈 분리까지 진행한다.
- 기본 태도는 보수적 회피가 아니라 공격적이고 진취적인 구조 개선이다. 필요한 경우
  자료구조, planner/executor boundary, SQL object, regression expected 변경까지 포함한다.
- commit 단위는 구조적 의미가 있는 전진 단위로 잡는다. 단일 helper/API/guard 변경만으로
  commit하지 않고, 같은 descriptor 전환, caller 정리, 문서 근거, focused 검증을 하나로 묶는다.
- 코드 정리는 같은 분류를 최대한 묶은 대분류 단위로 진행한다. 기존처럼 작은 helper/caller
  단위로 끊지 않고, 대략 10배 큰 범위의 module boundary, descriptor family,
  source/cache lifecycle, projection/index handoff를 한 작업 단위로 정리한다. 작은 정리
  10개 정도가 모여야 하나의 의미 있는 대분류 정리로 본다.
- workspace note 문서(`AGENTS.md`, `FEATURES.md`, `HISTORY.md`, `RESEARCH.md`, `TODO.md`,
  `VLE.md`)는 사용자가 명시적으로 요청한 경우에만 commit에 포함한다.

## 현재 우선순위

1. `age_vle` FunctionScan/SRF 의존을 CustomScan 실행 구조로 더 밀어낸다.
   - 목표는 `age_vle` 함수를 더 잘 감싸는 것이 아니라, planner가 `AGE VLE Stream`
     CustomScan을 VLE 전용 scan node처럼 구성하고 executor가 typed descriptor/traversal contract를
     직접 실행하게 만드는 것이다.
   - SQL-visible `age_vle()` wrapper와 `age_vle_internal()` SQL/C SRF surface는 제거했다. direct/internal
     SQL regression은 VLE stream/marker 기반 regression으로 대체한다.
   - `AGE VLE Stream` verbose explain에는 shape/range/direction/slot layout, output requirement,
     source layout/runtime counter 같은 metadata를 계속 드러낸다.
   - `VLECandidateMatchResult`와 match bit policy는 context/traversal handoff API로 낮췄다. DFS consume
     결과도 `VLETraversalStep`으로 묶어 terminal acceptance/output/extension이 같은 step descriptor를
     읽는다. `age_adjacency` payload cache/replay와 packed adjacency source도 opaque begin/next/end
     lifecycle로 묶었다. iterator output result/null write는 `VLEIteratorOutputTarget`으로 묶었고,
     materialization kind dispatch는 `age_vle_iterator_emit_result()`로 낮췄다. terminal scalar/full
     properties, prefetch, batch materialization은 `age_vle_terminal_output` module boundary로 분리했다.
     planner/runtime source 선택도 `select_vle_traversal_source_layout()` descriptor API를 공유한다. endpoint
     fanout/reltuples estimate, planner edge-source evidence, local edge-state capacity evidence,
     `age_adjacency` path costing evidence, runtime source feedback formatting은 `age_vle_source_cost`로
     묶었다. `AGE VLE Stream` descriptor에는 planner-only source cost policy text와 directed enum recommendation도
     싣는다. 이 directed enum은 `AgeVLEInput`과 setup/apply/root source layout handoff를 통해 executor source
     선택에 들어간다. cached context refresh도 같은 source policy를 유지하며, runtime counter는 CustomScan 실행
     전체 누적값과 source별 scan density로 출력한다. source policy explain은 bounded traversal의 누적 branch work를
     `endpoint-work=sum(current/limit)`과 `reason=out:.../in:...`으로 출력하고, finite upper 전체에 costed
     policy를 적용한다. 800-label fan-out benchmark harness는 `tools/vle_benchmark.sql` 기본 profile로 추가했다.
     fixed-source가 local edge-state
     label/no-property VLE의 방향 전체를 덮는 경우 packed fallback setup을 policy skip으로 생략하고, runtime
     counter는 `packed=scans/candidates/empty-skips/policy-skips`를 출력한다. source policy는 fanout 값과
     statistics 신뢰도를 분리하고, multi-step endpoint/`age_adjacency` work tie는 `age_adjacency`로 전환한다.
     planner policy와 runtime feedback은 모두 `class=... recommendation=...`으로 threshold 보정 방향을 출력한다.
     planner policy는 consumer requirement와 combined undirected work도
     `consumer=... consumer-class=... combined-work=...`로 출력한다. path/list materialization consumer는
     이제 local edge-state source를 쓰더라도 필요한 vertex/edge object를 label relation row에서 직접
     materialize할 수 있다. source policy는 terminal-only와 path-materialized consumer의 endpoint fanout
     budget을 분리해 path output이 `age_adjacency` 후보를 더 빨리 선택하게 한다. terminal consumer도
     `terminal-scalar`와 `terminal-object`로 나눠 vertex/properties materialization이 필요한 output은 path와
     같은 낮은 endpoint fanout budget을 쓴다. 다음 큰 단위는 benchmark
     결과에서 planner/runtime class와 consumer class를 함께 비교해 dense-local/`age_adjacency` 전환 기준과
     runtime feedback 반영 방식을 더 보정하는 것이다. benchmark summary는 terminal/path fan-out shape를 모두
     실행하고 planner policy와 runtime feedback을 shape별로 join해 `source_match`, `class_match`, source
     density를 출력한다. multi-step `age_adjacency` planner policy는 payload cache seed 가능성을 class로 반영해
     benchmark의 planner/runtime class vocabulary를 맞춘다. `AGE VLE Stream` descriptor는 policy class,
     recommendation, cache seed eligibility, endpoint headroom, consumer profile, active direction, fanout budget을
     typed field로 싣고 runtime feedback/benchmark summary는 더 이상 policy text에서 profile을 다시 파싱하지 않는다.
     `VLE Source Runtime` line도 planned fixed-source,
     `source-match`, `planned-class`, `class-match`, `pressure`, `action`을 직접 출력해 raw EXPLAIN만으로
     descriptor/runtime source, feedback class mismatch, 다음 fallback/cache 조정 후보를 확인할 수 있다. source policy 입력은 `VLESourcePolicyProfile`로 묶어 consumer
     class, active direction, fanout budget, depth, cache seed eligibility를 하나의 request로 다룬다. `VLE Edge Source`와
     benchmark summary는 `active=out|in|both`, `cache-seed=eligible|ineligible`을 출력한다. cache seed 가능한 multi-step `age_adjacency` 후보는
     planned empty lifecycle이 있으면 endpoint-btree 유지 headroom을 50%로 낮추고, headroom을 넘으면
     `empty-lifecycle-headroom` reason으로 `age_adjacency`를 고른다. terminal vertex materialization도 global vertex metadata가 없는 local-source
     output에서 label relation row fallback을 사용한다. 단일 step work tie도 consumer class를 반영해
     terminal-scalar는 endpoint-btree를 유지하고 path/terminal-object는 `adjacency-materialized-tie`로
     `age_adjacency`를 선호한다. runtime pressure는 이제 productive low-density scan과 empty/exhausted source
     probe를 분리한다. `VLE Source Runtime`은 `empty=age_adjacency:N/endpoint-btree:N`을 출력하고,
     800-label fan-out smoke에서는 `age_adjacency` scan 9회 중 empty 8회가 `adjacency-empty-probe` /
     `suppress-empty-source`로 드러났다. `age_adjacency` payload source는 이제 `begin_key=false`를 scan으로
     열지 않고 selected empty source completion으로 처리하며, fan-out smoke는 scan 1회, empty scan 0회,
     `empty-suppressed=8`을 출력한다. runtime feedback은
     `empty-suppressed=age_adjacency:N/out:N/in:N`과 `action=observe-suppression:out|in|both`로 active
     direction을 직접 드러낸다. frontier-level known-empty source도 `age_adjacency` directory cache hint로
     payload cache에 반영하며, runtime evidence는 `empty-frontier=age_adjacency:N/out:N/in:N`으로 출력한다.
     missing-vertex source-run은 payload source object 생성 전에 known-empty cache를 확인해
     `empty-run=age_adjacency:N/out:N/in:N`으로 접는다. 다음 큰 단위는 이 evidence를 planner feedback과
     dense-local/`age_adjacency` work-tie 보정에 반영하는 것이다. empty suppression/run evidence가 planned
     `adjacency-cache-seeded` lifecycle과 일치하면 runtime feedback도 같은 class로 정규화해 class mismatch를
     source handoff 문제로 오해하지 않게 한다. planner descriptor는 `empty_lifecycle_eligible`와
     `empty_lifecycle_depth` typed field도 싣고, EXPLAIN은 `empty-lifecycle`/`empty-plan`으로 이를 드러낸다.
     runtime feedback은 `empty-evidence`와 `adjacency-empty-frontier`/`adjacency-empty-run` pressure로
     planned empty lifecycle이 어느 단계에서 소비됐는지도 드러낸다. empty lifecycle descriptor는
     `AgeVLEInput`, context apply, cached refresh, local context source stats까지 전달되고,
     `VLE Source Runtime`의 `empty-context=... match=...`와 benchmark join summary로 context handoff까지
     확인한다. 다음 큰 단위는 root/source descriptor가 이 empty lifecycle eligibility와 runtime empty evidence를
     threshold 보정 또는 repeated source completion batching 입력으로 소비하게 만드는 것이다. 현재 planner는
     `empty-batch=.../size:N`을 계산하고 executor frontier empty queue가 이 capacity를 사용한다. strong
     empty-batch 후보는 endpoint headroom을 0.35로 낮추고, runtime은
     `empty-summary=completion:N/batch:N/saturated:true|false`를 출력한다. root/source completion은
     `root-empty=completion:N/out:N/in:N/batch:N/saturated-roots:N` descriptor로 올라갔다. runtime
     `threshold-feedback`은 backend-local planner feedback cache로 연결됐고, 후속 `VLE Edge Source`는
     `threshold-input=runtime-cache/headroom:N/batch:N/...`로 이를 드러낸다. saturated feedback은 다음
     planning의 endpoint headroom과 empty-batch capacity에 함께 들어간다. feedback cache는
     `observed/saturated/relaxed` state를 가지며 non-saturated completion evidence로 batch를 다시 낮출 수 있다.
     frontier known-empty queue flush는 `empty-frontier-batch=flushes:N/out:N/in:N/keys:N/max:N`으로
     계측한다. 다음 큰 단위는 큰 fan-out benchmark를 기준으로 dense-local/`age_adjacency` 전환 threshold와
     cache/payload replay contract를 더 공격적으로 조정하는 것이다.

2. ORCA식 `PathTarget` lower/final descriptor 구조를 더 넓은 AGE shape에 적용한다.
   - `RESEARCH.md`의 "ORCA required/computed column 구조"를 기준 설계로 둔다.
   - `add_deferred_projection_paths`를 공통 진입점으로 유지하고, lower required expression과 final
     output expression을 분리할 수 있는 shape를 우선한다.
   - collect/property aggregate 함수를 수동으로 계속 나열하지 않는다. aggregate rewrite가 필요하면
     descriptor, slot vector, final materialization contract를 함께 바꾼다.
   - collect/numeric/array_agg/count property aggregate rewrite entry point는 `cypher_property_paths`
     module로 모았다. 다음 aggregate 변경은 이 walker에 작은 case를 더 붙이는 방식이 아니라
     slot-vector/final materialization descriptor 확장으로 묶는다.
   - cached-property slot은 handoff descriptor를 함께 보관하고, partial predicate/restriction
     canonicalization은 descriptor와 catalog expression surface를 함께 담은 canonical entry를 공유한다.
     다음 index 작업은 expression index matching과 restriction rewrite가 이 canonical entry를 직접 소비하게 해
     rebuilt helper chain보다 catalog surface를 우선하는 contract를 넓히는 것이다.
   - property index restriction rewrite와 predicate/restriction canonicalization은
     `cypher_property_paths` module로 이동했다. 다음 index 작업은 catalog expression surface 보존과
     cached-property slot metadata를 더 직접 연결하는 쪽으로 묶는다.

3. Generic property projection/aggregate의 중간 `agtype` materialization을 더 줄인다.
   - `GROUP BY`/`DISTINCT` key와 aggregate input을 typed/scalar physical value로 유지하고 final
     projection 직전에만 `agtype`으로 materialize한다.
   - `CypherCachedPropertySlotDescriptor`, scalar final handoff, typed collect handoff, property index
     handoff가 같은 cached-property 후보 descriptor를 쓰게 한다.
   - `AGE Property Projection`은 같은 key path를 여러 physical result type으로 출력할 때 첫 slot의 raw
     property lookup을 재사용한다.
   - `array_agg` property/map/list aggregate handoff와 slots aggregate OID cache는 `cypher_property_paths`
     module로 이동했다. 다음 변경도 aggregate 함수 나열이 아니라 cached-property slot/index metadata와
     aggregate lower target descriptor를 연결하는 방향으로 묶는다.
   - typed collect lower argument plan도 `cypher_property_paths` module로 이동했다. aggregate path hook은
     group expr 보존과 path 조립만 담당하고, cached-property slot expression 선택과 aggregate target rewrite는
     property descriptor module이 소유한다.
   - typed collect와 `array_agg` property narrow path의 lower required target planning도
     `cypher_property_paths` module로 이동했다. group expr 보존과 aggregate arg sortgroupref assignment는
     descriptor module이 맡고, path hook은 path node 조립만 담당한다.
   - scalar-to-agtype final output의 lower/final `PathTarget` construction도 `cypher_property_paths`
     module로 이동했다. count/agtype deferred projection path는 path 조립만 맡고, lower computed expression과
     final materializer rewrite는 property descriptor module에서 만든다.
   - simple property projection detection과 ordered property projection lower/final target construction도
     `cypher_property_paths` module로 이동했다. path hook은 CustomPath/deferred path 삽입만 맡고,
     property source/key/type 판정은 descriptor module이 소유한다.
   - 다음 작업은 map/list aggregate final materialization의 slot-vector state를 cached-property
     slot/index metadata와 더 직접 연결하고, 큰 hash aggregate/partial aggregate shape에 필요한 executor
     contract를 확장하는 것이다.

4. VLE/global graph cold-load 병목을 큰 fan-out workload에서 다시 측정하고 바로 줄인다.
   - 현재 큰 후보는 cold global graph load의 vertex/edge table full scan, label-unrelated load,
     `age_adjacency` payload와 dense VLE state contract 연결 비용이다.
   - endpoint-btree/`age_adjacency` 선택은 runtime source counter와 fan-out workload evidence를 기준으로
     더 공격적으로 cost 기반 전환한다.
   - 800-label fan-out synthetic workload와 constrained start + terminal anonymous 단일 VLE path를 기준
     workload로 유지한다. 기본 실행은 `tools/vle_benchmark.sql`의 `800-label-fanout-terminal`이다.

5. `_agtype_build_path` 제거 이후 남은 fixed/arbitrary path materialization boundary를 점검한다.
   - arity-specific `_agtype_build_path_label*` 함수는 다시 늘리지 않는다.
   - path/list/entity 최종 projection이 필요한 shape만 materialization fallback을 유지한다.
   - count/length/endpoint/property-only consumer는 raw descriptor 또는 direct helper로 처리한다.

## 다음 실행 단위

1. `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca`에서 required/computed column과 physical property
   handoff를 다시 확인하되, source mismatch를 찾는 조사가 아니라 repeated source completion을 physical
   property/lifecycle contract로 올리는 관점에 집중한다.
2. VLE source planner policy와 runtime feedback을 큰 fan-out cost/index decision으로 보정한다.
   - `suppression-match=true`인 800-label fan-out shape는 source handoff mismatch가 아니다. known-empty payload
     cache는 같은 source 반복, frontier-level known-empty source, missing-vertex source-run precheck에 효과가 있고
     regression과 runtime feedback에 들어갔다. 다음 후보는 dense-local/`age_adjacency` work tie 보정과
     root descriptor의 frontier empty completion summary를 planner threshold에 반영하는 것이다.
   - 작은 데이터에서 효용이 작아도 큰 dataset에서 empty source completion이 반복될 수 있으므로,
     `adjacency-empty-suppressed` pressure를 source enum rollback이 아니라 source lifecycle/batching 구조 변경의
     우선 근거로 둔다.
   - benchmark join은 `active_planner_source`, `active_planned_source`, `suppressed_source`,
     `suppression_match`, `empty-cache`, `empty-frontier`, `empty-run`, `empty-plan`, `empty_evidence`,
     `empty_plan_match`를 함께 보고, `source_match=false`, `class_match=false`,
     `suppression-match=false`, `empty_plan_match=false`일 때만 source handoff 또는 descriptor mismatch를 먼저
     의심한다.
   - `source_match=true`, `class_match=true`, `suppression_match=true`, `empty_plan_match=true`인데
     `adjacency-empty-frontier` 또는 `adjacency-empty-run`이 반복되면 다음 작업은 source enum 전환이 아니라
     root/source lifecycle threshold와 repeated source completion batching을 조정한다.
   - raw EXPLAIN의 `empty-plan=... match=false`가 나오면 threshold 조정보다 planner descriptor eligibility와
     runtime empty evidence contract를 먼저 고친다.
   - general expansion source-run precheck는 right fanout의 frontier known-empty를 `empty-run`으로 올렸다.
     broader `age_adjacency` directory negative probe는 left/reverse fanout도 `empty-frontier`/`empty-run`으로
     올렸다. pressure/action vocabulary도 planned lifecycle match 기준으로 `empty-frontier`/`empty-run` 중심으로
     세분화했다. planned empty lifecycle은 endpoint headroom threshold와 cached refresh context handoff에도
     반영했다. `empty-batch` capacity는 frontier empty queue allocation과 endpoint headroom threshold에 적용했고,
     runtime saturation summary와 root/source completion summary도 출력한다. `threshold-feedback`은 backend-local
     feedback cache를 통해 다음 planning의 `threshold-input=runtime-cache/headroom:N/batch:N`로 들어간다. cache
     state는 `threshold-cache=observed:N/saturated:N/relaxed:N`로 출력된다. frontier empty queue flush는
     `empty-frontier-batch=flushes:N/out:N/in:N/keys:N/max:N`로 출력된다. payload cache evidence도
     `payload-cache=runs:scan:N/replay:N/seed:N/tuples:scan:N/replay:N/seeds:N`로 분리됐고,
     backend-local planner feedback 입력은 `payload-input=runtime-cache/...`로 드러난다.
     `vle_payload_replay_policy` regression은 `payload-cache` replay run과 후속
     `payload-input=.../replay-percent:25/.../reason:payload-replay-ratio-observed`를 plan surface로 고정했다.
     replay 비율이 strong threshold를 넘으면 path/object consumer의 endpoint headroom은 0.25까지 낮아지고,
     terminal-scalar consumer는 같은 25% replay ratio에서 headroom 0.35를 유지한다. benchmark harness도
     `payload-replay-path`, `payload-replay-terminal`, `payload-replay-vertex`, `payload-replay-properties`로
     seed-only와 high replay ratio를 분리한다. replay input을 소비한 planner policy class는
     `adjacency-replay`로 정규화한다. `root-empty-saturated` threshold input은
     `adjacency-empty-batch` class로 분리해 empty lifecycle/batching request가 `adjacency-cache-seeded`에 묻히지
     않게 했다. `tools/vle_benchmark.sql`은 `replay_branches`와 `replay_leaves`를 받아 replay fan-in/leaf fan-out을
     독립 측정할 수 있고, final join summary는 input/resolved replay scale, `rows_returned`, `elapsed_ms`를
     출력한다. 낮은/높은 replay ratio profile 재측정 결과 terminal-scalar strong replay threshold는 40%로
     낮췄고, 25% replay profile은 `payload-replay-observed`로 유지한다. path/terminal-object는 25% replay부터
     strong class로 두되 `materialization-weight` descriptor를 추가해 path는 weight 3/headroom 18%,
     terminal-object/properties는 weight 2/headroom 20%, terminal-scalar는 weight 1/headroom 25%를 사용한다.
     generic `AGE Property Projection` slot descriptor도 final agtype/scalar materialization weight를 출력한다.
     다음 후보는 이 weight를 partial materialization boundary와 aggregate final descriptor에 연결하는 것이다.
3. 가장 큰 구조 변경 후보 하나를 고르고, 같은 단위 안에서 코드 변경, regression expected, 문서 근거를
   함께 묶는다.
4. focused regression은 변경 범위에 맞춰 실행한다. VLE/global graph 변경의 기본 후보는
   `cypher_vle age_global_graph age_adjacency`다.
5. 변경 단위가 너무 작아지면 commit하지 말고 같은 분류의 caller contract, module boundary,
   descriptor/cache lifecycle 정리까지 확장해 대분류 단위로 묶는다.

## 검증 기준

```sh
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install
git diff --check
```

```sh
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='cypher_vle age_global_graph age_adjacency'
```

- argument 없는 전체 `make installcheck`는 기본 검증으로 사용하지 않는다.
- 변경 범위가 넓으면 `expr cypher_match cypher_create cypher_set cypher_delete cypher_merge
  cypher_vle age_global_graph age_adjacency security` 중 관련 항목을 추가한다.
- 경고를 오류로 확인해야 하는 요청이 있으면 `make clean` 뒤 `COPT=-Werror` build를 사용한다.

## 현재 기준

- 이 브랜치의 gate는 fresh install 기준 동작, focused regression, benchmark 근거다.
- extension upgrade 하위 호환성, 예전 catalog/data structure/layout 보존은 gate가 아니다.
- `age_upgrade`는 regression 목록에 남아도 upgrade parity가 아니라 정책 smoke test로만 본다.
- `age--1.7.0.sql` 같은 built SQL 산출물은 source SQL 변경으로 재생성되는 파일이면 강제로 commit하지
  않는다. source of truth인 `sql/*.sql`, C source, regression, 문서를 우선한다.
