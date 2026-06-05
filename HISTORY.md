# VLE Follow-up History

## 기록 원칙

- 완료된 변경과 검증 결과를 날짜별로 짧게 남긴다.
- 상세 설계 판단은 `RESEARCH.md`, VLE 구조와 benchmark는 `VLE.md`에 둔다.
- 되돌린 시도는 이유와 다음 방향만 남긴다.

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
- partial index predicate와 `check_index_predicates()` 이후 `indrestrictinfo` clause를
  cached-property slot expression 기준으로 정규화한다. 실제 index surface가 raw
  `agtype_access_operator`인 경우는 보존하고, scalar/typed canonical surface와 이미 일치하는
  index/predicate expression만 정규화 대상으로 삼는다.
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
