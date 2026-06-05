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
  단위로 끊지 말고, 대략 10배 큰 범위의 module boundary, descriptor family, source/cache lifecycle,
  projection/index handoff를 한 작업 단위로 정리한다. 작은 정리 10개 정도를 모아 하나의
  대분류 정리로 만드는 것을 기본 기준으로 삼는다.
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
     counter는 `packed=scans/candidates/empty-skips/policy-skips`를 출력한다. 다음 큰 단위는 이 benchmark 결과와
     runtime counter를 기준으로 source threshold를 더 보정하는 것이다.

2. ORCA식 `PathTarget` lower/final descriptor 구조를 더 넓은 AGE shape에 적용한다.
   - `RESEARCH.md`의 "ORCA required/computed column 구조"를 기준 설계로 둔다.
   - `add_deferred_projection_paths`를 공통 진입점으로 유지하고, lower required expression과 final
     output expression을 분리할 수 있는 shape를 우선한다.
   - collect/property aggregate 함수를 수동으로 계속 나열하지 않는다. aggregate rewrite가 필요하면
     descriptor, slot vector, final materialization contract를 함께 바꾼다.
   - collect/numeric/array_agg/count property aggregate rewrite entry point는 `cypher_property_paths`
     module로 모았다. 다음 aggregate 변경은 이 walker에 작은 case를 더 붙이는 방식이 아니라
     slot-vector/final materialization descriptor 확장으로 묶는다.
   - 다음 index 작업은 `(key path, semantic value type, physical result type)` property signature를
     cached-property slot/index metadata로 직접 넘기고, partial predicate와 expression index matching이
     같은 canonical descriptor를 공유하게 하는 것이다.
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
   handoff를 다시 확인한다.
2. VLE source planner policy와 runtime feedback을 큰 fan-out cost/index decision으로 보정한다.
   - 후보: endpoint-btree/`age_adjacency` source 선택 work model, accumulated runtime source counter,
     `AGE VLE Stream` policy descriptor reuse, cached-property slot/index 연결.
3. 가장 큰 구조 변경 후보 하나를 고르고, 같은 단위 안에서 코드 변경, regression expected, 문서 근거를
   함께 묶는다.
4. focused regression은 변경 범위에 맞춰 실행한다. VLE/global graph 변경의 기본 후보는
   `cypher_vle age_global_graph age_adjacency`다.
5. 변경 단위가 너무 작아지면 commit하지 말고 caller contract와 module boundary 정리까지 확장한다.

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
