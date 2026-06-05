# VLE Follow-up TODO

## 진행 원칙

- TODO는 남은 작업, 바로 다음 실행 계획, 검증 기준만 유지한다.
- 완료한 작업은 `HISTORY.md`, 설계 판단과 조사 근거는 `RESEARCH.md`, VLE 구조와
  benchmark/regression 기록은 `VLE.md`로 옮긴다.
- workspace note 문서 언어, 문서별 역할, commit message, push, macOS/WSL test
  절차는 `AGENTS.md`를 기준으로 적용한다. TODO에는 그중 현재 작업 진행에 필요한
  실행 원칙만 짧게 남긴다.
- 사용자가 `AGENTS.md` 지침을 다시 제시하면 현재 turn의 작업 기준으로 즉시
  동기화한다. 상세 절차는 `AGENTS.md`에 두고, `TODO.md`에는 최적화 진행 순서와
  검증/commit 판단에 직접 필요한 항목만 반영한다.
- `AGENTS.md`와 충돌하는 세부 절차가 생기면 `AGENTS.md`를 우선 기준으로 삼고,
  `TODO.md`에는 현재 최적화 작업의 우선순위와 다음 실행 계획만 남긴다.
- workspace note 문서(`AGENTS.md`, `FEATURES.md`, `HISTORY.md`, `RESEARCH.md`,
  `TODO.md`, `VLE.md`)는 기본 한글로 작성한다. code identifier, file path, command,
  SQL/C function name, benchmark label, 논문 제목은 원 표기를 유지한다.
- TODO 항목을 진행할 때 새로 확인한 연구 근거와 설계 판단을 코드 변경과 함께
  남긴다. PostgreSQL source, ORCA, Neo4j, 논문, graph DB/project 조사로 방향을
  바꾸면 근거는 `RESEARCH.md`에 적고, VLE 실행 구조/cache layout/traversal
  state/regression coverage와 직접 관련되면 `VLE.md`에도 반영한다.
- 완료 기록은 `TODO.md`에 길게 쌓지 않는다. 완료 변경과 검증 결과는
  `HISTORY.md`, 설계 판단은 `RESEARCH.md`, VLE 실행 구조와 benchmark/regression
  coverage는 `VLE.md`에 남기고, `TODO.md`는 남은 작업과 바로 다음 실행 계획으로
  되돌린다.
- argument 없는 전체 `make installcheck`는 실행하지 않는다. 변경 범위에 맞는
  focused `installcheck REGRESS='...'`만 사용한다.
- 큰 실험 주제가 끝나면 macOS 기준 build, install, focused installcheck,
  `git diff --check`를 한 번 실행한다. 같은 실험 주제 안의 작은 commit마다 full
  검증을 반복하지 않는다.
- macOS 기본 검증은 `/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config`
  기준 build/install/focused installcheck/`git diff --check`다. WSL 검증이 필요하면
  regression temp instance와 output은 Windows mount가 아니라 `/tmp` 아래에 둔다.
- 경고를 오류로 확인해야 하는 요청이 있으면 `make clean` 뒤 `COPT=-Werror` build를
  사용한다. 평소 진행 중에는 변경 범위에 맞는 focused regression을 우선한다.
- regression 실패 시 먼저 `regress/regression.diffs` 또는 `/tmp/.../regression.diffs`와
  `log/`를 확인한다. 실패를 이유로 변경을 되돌리는 대신 plan/result shape와 깨진
  contract를 분리해 focused workload에서 돌파구를 만든다.
- 중간중간 commit은 하되, 단일 guard/assertion/helper 변경처럼 독립 의미가 약한
  조각을 여러 작은 commit으로 나누지 않는다. 같은 최적화 주제, 같은
  materialization boundary, 같은 descriptor 전환, 같은 focused regression 범위에 속한
  코드/SQL/test/문서 변경은 하나의 의미 있는 commit으로 묶는다.
- 같은 최적화 주제에서 코드, regression, workspace note 문서가 함께 바뀌면 focused
  검증 뒤 한 commit으로 묶는다. 단, 사용자가 문서 제외를 명시하면 해당 workspace note는
  commit 대상에서 뺀다.
- 이 최적화 브랜치에서는 extension upgrade 하위 호환성을 보장하지 않는다.
  `age_upgrade`는 regression 목록에 남기되 예전 install과 fresh install의
  catalog/data structure/layout parity를 비교하지 않는 정책 smoke로만 둔다.
- 예전 catalog/data structure/layout을 유지하기 위해 최적화 자료구조나 SQL object
  변경을 제한하지 않는다. fresh install 기준 동작과 focused regression을 우선한다.
- `age--1.7.0.sql` 같은 built SQL 산출물은 source SQL 변경으로 재생성되는 파일이면
  강제로 commit하지 않는다. 커밋 대상은 `sql/*.sql`, C source, regression, 문서처럼
  실제 source of truth인 파일을 우선한다.
- workspace note 문서(`AGENTS.md`, `FEATURES.md`, `HISTORY.md`, `RESEARCH.md`,
  `TODO.md`, `VLE.md`)는 진행 요청으로 직접 바뀐 경우에만 같은 작업 commit에
  포함한다. 단순 완료 로그를 `TODO.md`에 길게 남기지 않는다.
- 문서 역할은 `AGENTS.md`의 정의를 따른다. `TODO.md`는 남은 작업과 다음 실행 계획,
  `HISTORY.md`는 완료 변경과 검증, `RESEARCH.md`는 조사 근거와 설계 판단, `VLE.md`는
  VLE 실행 구조/cache layout/traversal state/benchmark/regression coverage를 맡는다.
- 사용자가 push를 요청하면 `wsl.exe`를 거치지 않고 현재 shell에서 직접 `git push`를
  실행한다. 히스토리 재작성 뒤 push는 기본적으로 `--force-with-lease`를 사용한다.
- commit message는 짧은 명령형 subject와 필요한 body로 작성하고, history rewrite 뒤
  push가 필요하면 현재 shell에서 `git push --force-with-lease`를 기본으로 쓴다.
- guard를 계속 추가하는 방식은 피한다. 의미가 실제로 갈리는 경우가 아니라면
  source kind, resolved signature, compact container contract를 더 명확히 만들어
  dispatch가 직접 경로를 선택하게 한다.
- 실패를 막기 위해 planner guard를 덧붙이는 것보다, helper/descriptor/executor contract가
  더 넓은 semantic을 올바르게 처리하도록 개선하는 선택을 우선한다. 특히 property
  descriptor가 properties object와 vertex/edge entity 양쪽을 만나는 경우에는 호출자를
  제한하기보다 공통 helper의 입력 contract를 명확히 확장한다.
- 새 regression은 가능하면 `DO` block 안에서 plan 문자열을 assertion으로 숨기지 않는다.
  최적화 plan shape를 고정해야 할 때도 `EXPLAIN (VERBOSE, COSTS OFF)` 또는 실제 query
  result를 그대로 출력해 expected 파일이 plan/result evidence가 되도록 한다. assertion형
  regression은 오류 메시지 자체가 중요한 경우나 출력 노이즈가 과도한 경우에만 제한적으로
  사용한다.
- OID처럼 실행마다 달라지는 값만 `pg_temp` helper 등으로 정규화하고, plan 자체는 expected에
  드러낸다. hidden assertion을 추가해 plan evidence를 감추는 방향으로 회귀하지 않는다.
- 병목이 반복되면 자료구조 변경, metadata 추가, auxiliary index/cache, load contract
  변경, traversal state layout 변경, planner/executor boundary 변경을 후보로 올린다.
- 현재 AGE 코드 구조, catalog/data layout, helper 함수 경계, planner hook 경계에 갇히지
  않는다. 큰 데이터셋에서 필요한 구조라면 기존 모듈 경계나 자료구조를 바꾸는 실험을
  우선 검토한다.
- 한 파일에 여러 기능을 계속 쌓지 않는다. 새 최적화가 기존 파일의 책임을 흐리거나
  helper/projection/aggregate/VLE 경계가 섞이면 같은 작업 단위에서 파일 또는 모듈
  분리까지 진행한다.
- 기본 태도는 보수적 회피가 아니라 공격적이고 진취적인 구조 개선이다. 큰 데이터셋
  효용이 분명한 후보는 기존 guard에 머무르지 말고 자료구조, planner/executor boundary,
  파일 분리, SQL object 변경까지 포함해 실제 patch로 밀어붙인다.
- 최우선 참고 소스는 `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca`다.
  구현 전 ORCA의 `CColRefSet`, `CScalarProjectElement`, `CPhysicalComputeScalar`,
  `CExpressionPreprocessor::PexprPruneUnusedComputedCols`처럼 required column과
  computed column을 분리하는 구조에서 insight를 먼저 얻고, AGE 변경 방향을 정한다.
- graph planning/materialization 구조는 `/Users/emotionbug/IdeaProjects/neo4j`도 함께
  참고한다. 우선 조사 대상은 `community/cypher/cypher-planner`,
  `community/cypher/physical-planning`, `community/cypher/slotted-runtime`의 projection,
  distinct, aggregation, slot allocation, pruning var-expand 구조다.
- 작은 synthetic 데이터셋에서 개선 폭이 작거나 흔들려도, row width 감소, materialization
  지연, required column 축소, cache/load contract 개선처럼 큰 데이터셋에서 효용이 커질
  수 있는 구조 변경을 우선한다. 작은 데이터 결과만으로 구조 후보를 폐기하지 말고,
  큰 cardinality/넓은 property payload/cold cache workload에서 다시 판단한다.
- 첫 구현이나 첫 regression/benchmark 시도가 실패해도 그 자체를 포기 조건으로 삼지
  않는다. 실패 원인을 `RESEARCH.md`/`VLE.md`에 남기고, 같은 목표를 더 작은 shape,
  더 명확한 descriptor, 더 좁은 workload로 나눠 여러 번에 걸쳐 성공 경로를 만든다.
- 실패가 발생했을 때 기본 행동은 되돌리기가 아니라 돌파구 탐색이다. regression,
  build, benchmark, planner assertion, crash가 실패하면 먼저 실패 shape와 깨진
  contract를 분리하고, 필요한 경우 descriptor boundary, target handoff, index layout,
  load/cache contract, workload shape를 바꿔 같은 목표를 계속 전진시킨다.
- TODO 진행 원칙 자체를 적용하다 실패해도 해당 원칙이나 변경을 되돌리는 것으로
  마무리하지 않는다. 순서, 범위, 검증, commit 묶음, 구조 변경 지침이 막히면 실패한
  지점을 기록하고 더 큰 구조 변경, 더 좁은 focused workload, 별도 descriptor/index/cache
  설계처럼 돌파구를 찾아 같은 목표를 계속 진행한다.
- TODO에서 "설계한다", "검토한다", "확인한다", "측정한다"처럼 준비 단계로 보이는
  표현이 나오면 그 단계에서 멈추지 않는다. 같은 작업 단위 안에서 설계 근거 기록,
  코드/SQL/test 변경, focused 검증까지 한꺼번에 진행한다.

## 다음 작업

1. ORCA식 `PathTarget` lower/final descriptor 구조를 더 넓은 AGE shape에 적용한다.
   - `RESEARCH.md`의 "ORCA required/computed column 구조"를 기준 설계로 둔다.
   - `add_deferred_projection_paths`를 공통 진입점으로 유지하고, 새 최적화는 lower
     required expression과 final output expression을 분리할 수 있을 때만 추가한다.
   - collect/property aggregate 함수를 새로 더 나열하거나, 함수별 aggregate rewrite를
     먼저 늘리지 않는다.
   - 단순/nested select-target property key와 nested expression matching은 direct helper
     surface와 property signature descriptor로 낮췄다.
   - property predicate expression index는 nested key path까지 semantic property
     signature로 index surface를 선택하고, rewrite 뒤 index path를 다시 생성하도록 구조를
     보강했다. `cypher_match`의 property predicate/index surface 검증은 assertion이 아니라
     `EXPLAIN (VERBOSE, COSTS OFF)` 출력 regression으로 유지한다.
   - index 구조 조사는 1번의 추가 우선순위다. PostgreSQL `IndexOptInfo`의 expression
     key/predicate matching뿐 아니라 AGE property descriptor가 어떤 index key layout,
     operator class, partial predicate, cached-property handoff로 이어져야 하는지까지
     함께 확인하고 바로 patch 후보로 만든다.
   - `create_property_index(..., 'payload.a')`는 단일 key 이름이 아니라 property path
     descriptor로 해석해 `VARIADIC ARRAY[properties, key...]` expression index를 만들도록
     확장했다. 다음 index 작업은 이 path key layout을 typed/scalar operator class 또는
     cached-property handoff와 연결하는 방향이다.
   - `create_property_index(..., 'score', 'pg_bigint')`는 scalar helper expression index를
     만들도록 확장했다. `n.score::pg_bigint >= 10`처럼 typed property helper와 `agtype`
     literal이 비교되는 shape도 literal side를 scalar로 낮춰 같은 index layout을 쓰게 했다.
   - `create_property_index(..., 'payload.score', 'pg_bigint')`처럼 nested path typed scalar
     index는 prefix object descriptor와 terminal typed key helper로 연결했다. 다음 index
     작업은 partial predicate와 cached-property handoff가 같은 descriptor layout을 공유하게
     만드는 방향이다.
   - `create_property_index(..., 'payload.gpa', 'numeric')`도 parser의
     `n.payload.gpa::numeric` surface와 같은 `agtype_object_field_numeric_agtype(prefix, key)`
     descriptor index를 만들도록 연결했다. numeric은 PostgreSQL `numeric` Datum이 아니라
     numeric agtype descriptor로 matching한다.
   - `create_property_index(..., 'payload.gpa', 'pg_numeric')`은 PostgreSQL `numeric` Datum을
     반환하는 `agtype_object_field_numeric(prefix, key)` expression index로 분리했다.
     `n.payload.gpa::pg_numeric >= 3.8::pg_numeric`은 numeric btree index surface로
     matching한다. `ORDER BY n.payload.a::pg_numeric LIMIT 1`의 ordered projection delay와
     `collect(DISTINCT n.payload.a::pg_numeric)`의 typed collect aggregate도 같은 scalar
     descriptor를 사용하도록 확장했다.
   - typed `create_property_index`의 type-to-helper mapping과 property path의
     `prefix object + terminal key` 분해를 별도 helper로 분리했다. 다음 cached-property/index
     작업은 이 terminal descriptor를 slot/index metadata 후보로 넘기는 방향이다.
   - 새 property surface를 만들면 matching index descriptor, `create_index_paths()` 재호출
     필요성, GROUP/DISTINCT lower target expression surface가 함께 맞는지 확인한다.
   - property access comparison은 `(container, key path)` descriptor API로 분리했고,
     rewrite 뒤 partial expression index predicate 상태도 다시 계산하도록 보강했다.
   - ordered property projection의 ctid refetch helper는 relid/ctid/key implicit 구조에서
     relid/ctid/properties attno/key descriptor로 바꿨다. 현재 적용은 검증된 vertex
     properties로 제한한다.
   - ordered property projection delay는 `pg_bigint` 단일 helper에서 `pg_float8`,
     `pg_text`, `numeric` typed property sort descriptor까지 확장했다.
   - GROUP count lower/final descriptor는 `pg_bigint`뿐 아니라 `pg_float8`, `pg_text`,
     `numeric` typed property key에서도 `typed key + raw count` lower target을 유지하도록
     regression으로 고정했다.
   - edge ordered property projection의 join target handoff는 child target을 required
     expression과 join key 중심으로 재구성해 돌파했다. joined edge lower path는
     `r.properties` 원본을 carry하지 않고 `r.id`, typed sort expression, join key만
     유지하도록 `EXPLAIN` 출력 regression으로 고정했다. `count(startNode(...).name)` 같은
     VLE endpoint property count는 planner guard 없이 `agtype_object_field_exists_nonnull`
     helper contract를 vertex/edge entity까지 넓혀 semantic을 보존했다. 다음
     index/cached-property 작업은 이 computed expression handoff를 expression index path와
     연결한다.
   - property signature descriptor는 key path뿐 아니라 typed/scalar value type까지 포함한다.
     chained-prefix typed expression index와 parser가 만든 typed property surface가 같은
     descriptor로 matching되어 `Index Scan`으로 연결되는 것을 `index` regression의
     `EXPLAIN` 출력으로 고정했다.
   - property signature descriptor는 physical field result type도 포함하도록 확장했다.
     `numeric` agtype helper와 `pg_numeric` Datum helper는 semantic value type이 모두
     `NUMERICOID`라도 index matching에서 섞이지 않는다. 다음 index 작업은 이
     `(key path, semantic value type, physical result type)` descriptor를 cached-property
     slot/index metadata로 직접 넘기는 것이다.
   - partial typed expression index도 같은 descriptor matching과 `check_index_predicates()`
     재계산 뒤 `Index Scan`으로 연결되는 것을 출력 regression으로 고정했다. cached/non-null
     terminal property handoff는 `cypher_property_signature`의 terminal object/key API를
     공유하게 낮췄다.
   - nested ordered property projection도 key path descriptor를 ctid/id refetch final
     expression에 넘기도록 확장했다. `ORDER BY n.payload.a::pg_bigint LIMIT 1`의 lower target은
     prefix object를 carry하지 않고 `ctid + typed sort key`만 유지하며, final output에서
     refetched prefix 뒤 terminal key를 materialize한다. 다음 작업은 같은 key path descriptor를
     scalar final materialization slot과 typed aggregate final output에도 연결하는 것이다.
   - parser의 scalar property typecast rewrite도 `cypher_property_signature` terminal descriptor를
     공유하게 바꿨다. nested typed aggregate와 scalar final materialization에서 variadic prefix
     accessor 대신 direct object-field prefix를 쓰는 plan을 출력 regression으로 고정했다.
   - nested numeric collect는 prefix object를 aggregate input으로 받지 않고
     `age_collect_numeric_path_property(properties, key_path_array)` descriptor aggregate로 낮췄다.
     실제 결과와 plan 출력 regression으로 고정했다.
   - `array_agg` 단일 property, map2/map/list aggregate final output도 같은 key-path
     descriptor contract로 낮췄다. aggregate SQL object는 늘리지 않고 기존 aggregate 인자의
     agtype key descriptor를 scalar key 또는 key-path list로 확장했다.
   - collect/numeric/array_agg property aggregate rewrite를 `cypher_property_paths.c`의
     descriptor 모듈로 모았다. 다음 작업은 이 descriptor를 cached-property slot/index
     handoff와 연결하는 것이다.
   - expression index matching도 `cypher_property_paths.c`의 property signature descriptor
     API로 옮겼다. `cypher_paths.c`는 hook/rewrite orchestration에 집중하고, 다음 index
     작업은 property descriptor 모듈이 제공하는 canonical expression을 cached-property
     slot/index handoff와 연결하는 것이다.
   - `GROUP BY`/`DISTINCT` key의 remaining variadic shape 중 fixed path indexed property와
     VLE boundary property는 direct helper chain으로 낮췄다. 남은 조사는 index descriptor와
     scalar/cached-property descriptor 중심으로 이어간다.
   - 실패한 plan shape는 포기하지 말고, semantic guard와 descriptor boundary를 더 작게
     나눠 다시 시도한다.

2. Generic property projection/aggregate의 중간 `agtype` materialization을 더 줄인다.
   - ORCA식으로 computed projection을 별도 descriptor/column처럼 다루고, 상위에서
     실제 필요한 expression만 lower target에 남기는 방향을 우선한다.
   - `GROUP BY`/`DISTINCT` key를 typed/scalar physical value로 유지하고 final
     projection에서만 `agtype`으로 materialize하는 handoff metadata를 구현 가능한
     단위로 나눠 코드와 regression으로 검증한다.
   - `collect(n.i::numeric)`은 numeric property descriptor aggregate로 낮춰 generic
     `age_collect(agtype)`와 중간 numeric agtype materialization을 피하도록 했다.
   - typed DISTINCT collect는 `pg_float8`, `pg_bigint`, `pg_text` 모두 typed aggregate와
     narrow lower target을 유지하도록 regression으로 고정했다.
   - `count(n.payload.a)` 같은 nested property count는 terminal value를 만들지 않고
     prefix object + final key `exists_nonnull` descriptor로 낮췄다.
   - scalar-to-agtype final materialization은 `int8_to_agtype` 전용 추출에서
     `int8/float8/text` 공통 descriptor API로 낮췄다. 다음 작업은 이 descriptor를
     cached-property slot/index handoff와 연결해 final projection 직전까지 scalar Datum을
     유지하는 범위를 넓히는 것이다.
   - duplicate scalar-to-agtype final output은 lower target에서 같은 scalar descriptor를 한 번만
     carry하도록 줄였다. lower scalar descriptor canonicalization은
     `cypher_property_paths.c`의 property signature API로 옮겼다. 다음 작업은 이 unique
     scalar descriptor를 cached-property slot/index 생성 지점까지 넘기는 것이다.
   - scalar lower descriptor dedupe는 `equal()`뿐 아니라 property signature match로 canonical
     lower expression을 재사용한다. 다음 작업은 이 canonical descriptor를 expression index와
     cached-property slot 생성 지점까지 넘기는 것이다.
   - property fetch, predicate helper, projection, aggregate 사이의
     `agtype_access_operator()` -> scalar 변환 -> 다시 `agtype` materialize 경로를
     `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)`와 lldb call stack으로 다시 확인하고,
     확인한 병목을 줄이는 patch 또는 실패 기록을 같은 턴에 남긴다.
   - HashAggregate 입력 width와 final output descriptor를 직접 줄이는 구조를 우선한다.
   - `pg_numeric` grouped count는 lower target에서 `numeric + raw count`를 유지하도록
     낮췄다. 같은 scalar numeric descriptor를 `collect(DISTINCT ...)`, final agtype
     materialization, cached-property handoff와 공유하는 구조로 이어간다.
   - `age_collect_numeric(numeric)`은 `pg_numeric` DISTINCT collect를 위한 typed aggregate로
     추가했다. 다음 작업은 typed aggregate 함수를 더 늘리는 것이 아니라
     scalar descriptor와 final materialization descriptor를 공통 metadata로 묶는 것이다.
   - typed collect aggregate 선택은 `(value_type, aggregate name, cached oid)` descriptor table로
     공통화했다. property helper와 final agtype materializer 판정도
     `(value_type, field helper, field result type, final materializer)` descriptor table로
     낮췄다. 다음 descriptor 작업은 이 metadata를 index/cached-property handoff까지 넘겨
     final projection 직전까지 scalar Datum을 유지하는 것이다.
   - parser의 property signature helper도 field descriptor table로 낮췄고, optimizer-local
     scalar descriptor는 이 shared field descriptor API를 사용하도록 바꿨다. 다음 작업은
     final materializer와 typed aggregate descriptor까지 같은 descriptor handoff object로 묶어
     cached-property slot/index 생성 지점에 `(container, key path, semantic value type,
     physical result type, final materializer)`를 넘기는 것이다.
   - scalar-to-agtype final projection은 `CypherScalarFinalHandoff`로 낮춰 scalar expr,
     semantic value type, physical result type, final materializer OID, optional property
     signature를 함께 넘기게 했다. typed collect aggregate rewrite와 DISTINCT collect narrow
     path detection도 `CypherTypedCollectHandoff`로 낮춰 aggregate input expr, value type,
     aggregate OID, optional property signature를 함께 넘긴다. 다음 작업은 이 handoff들을
     expression index와 cached-property slot 생성 지점에서 재사용하게 만드는 것이다.
   - expression index matching도 `CypherPropertyIndexHandoff`로 낮춰 query expr, matched
     index expr, optional property signature를 함께 넘긴다. 다음 작업은 scalar final/typed
     collect/index handoff를 같은 cached-property slot descriptor 생성 API에 연결하는 것이다.
   - scalar final, typed collect, property index handoff는 이제 모두
     `CypherPropertyHandoffDescriptor`를 선택적으로 들고 property signature, final materializer
     OID, typed aggregate OID, matched index expr을 같은 cached-property 후보 descriptor에
     모은다. 다음 작업은 이 descriptor에서 실제 cached-property slot metadata를 만들고
     lower target/index path 생성에 넘기는 것이다.
   - simple property projection CustomPath는 `CypherCachedPropertySlotDescriptor`를 통해 key,
     semantic value type, physical field result type을 CustomScan executor까지 넘긴다. 다음
     작업은 이 slot metadata를 typed/scalar cached value path에도 사용해 final projection 직전까지
     scalar Datum을 유지하는 범위를 넓히는 것이다.
   - AGE Property Projection CustomScan은 slot metadata의 physical field result type을 사용해
     simple `pg_bigint`/`pg_float8`/`pg_numeric`/`pg_text` property projection을 scalar Datum으로
     직접 출력할 수 있다. ordered property projection delay도 output과 sort key의
     semantic/physical descriptor가 같으면 typed computed sort key를 final output으로 재사용해
     ctid/id refetch를 피한다. 다음 작업은 aggregate lower target과 index handoff도 같은 slot
     descriptor에서 scalar Datum을 받게 확장하는 것이다.
   - `CypherCachedPropertySlotDescriptor`는 canonical property field expression을 만들 수 있고,
     scalar final handoff와 typed DISTINCT collect lower target은 이 slot expression builder를
     통과한다. expression index handoff도 matched index expr을 설정할 때 같은 slot expression
     builder를 먼저 사용한다. partial predicate surface와 `check_index_predicates()` 재계산 이후
     `indrestrictinfo`도 실제 index/predicate tree 안에 같은 canonical slot expression이 있는 경우
     이 descriptor를 기준으로 정규화한다.

3. VLE/global graph cold-load 병목을 다시 측정하고 바로 줄인다.
   - 현재 남은 큰 후보는 cold global graph load의 vertex/edge table full scan,
     label-unrelated load, `age_adjacency` payload와 dense VLE state contract 연결 비용이다.
   - 800-label fan-out synthetic workload와 constrained start + terminal anonymous
     단일 VLE path를 기준 workload로 유지한다.
   - `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)`, lldb breakpoint, focused synthetic workload
     중 2개 이상으로 병목을 확인한 뒤 같은 작업 단위에서 patch, regression/benchmark
     기록, focused 검증까지 진행한다.

4. `_agtype_build_path` 제거 이후 남은 fixed/arbitrary path materialization boundary를
   점검하고 바로 정리한다.
   - arity-specific `_agtype_build_path_label*` 함수는 다시 늘리지 않는다.
   - path/list/entity 최종 projection이 필요한 shape만 materialization fallback을 유지한다.
   - count/length/endpoint/property-only consumer는 raw descriptor 또는 direct helper로
     처리한다.
   - fixed path와 VLE materialization plan 검증은 hidden `DO` assertion 대신 normalized
     `EXPLAIN (VERBOSE, COSTS OFF)` 출력 regression으로 유지한다.

## 검증 명령

```sh
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install
git diff --check
```

```sh
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='index cypher_match'
```

변경 범위가 VLE/global graph/security로 넓어질 때만 해당 regression을 추가한다.
argument 없는 전체 `make installcheck`는 기본 검증으로 사용하지 않는다.

## 현재 기준

- `age_upgrade`는 regression 목록에 포함하지만 upgrade parity를 검증하지 않는 정책
  smoke test다.
- 최근 clean warning fix는 `COPT=-Werror` build를 통과했다.
- 최근 `age_upgrade` focused installcheck는 `All 1 tests passed.`였다.
- Workspace note 문서는 사용자가 명시적으로 요청하지 않는 한 커밋하지 않는다. 사용자가
  TODO/문서 반영을 요청한 경우에는 해당 작업 commit에 포함한다.
