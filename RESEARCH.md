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
- typed collect도 aggregate OID와 argument expression만 따로 다루면 DISTINCT lower target 축소와
  future cached-property slot handoff가 다시 aggregate tree local rule에 묶인다.
  `CypherTypedCollectHandoff`는 aggregate input expr, scalar value type, typed aggregate OID,
  property signature를 한 번에 넘겨 final/scalar handoff와 같은 descriptor 계열로 맞춘다.
- expression index matching도 matched expression만 반환하면 caller가 어떤 property descriptor를
  기준으로 index surface를 선택했는지 잃는다. `CypherPropertyIndexHandoff`는 query expr,
  matched index expr, property signature를 함께 넘겨 다음 cached-property slot 생성 지점이
  같은 `(container, key path, semantic/physical type)` descriptor를 재사용할 수 있게 한다.
- scalar final, typed collect, property index handoff가 각각 다른 optional signature field를
  들면 cached-property slot candidate를 만들 때 다시 case별 변환이 필요하다.
  `CypherPropertyHandoffDescriptor`로 property signature, final materializer, typed aggregate,
  matched index expr을 한 곳에 모으는 구조가 slot descriptor 생성 API로 넘어가기 전 단계다.
- simple property projection CustomScan이 key만 받으면 executor boundary가 cached-property
  slot 후보의 semantic/physical type을 모른다. `CypherCachedPropertySlotDescriptor`를 만들고
  key, semantic value type, physical field result type을 CustomScan private data로 넘기면
  이후 typed/scalar cached slot으로 확장할 수 있는 실제 planner/executor contract가 생긴다.
- executor가 physical field result type을 사용해 scalar Datum을 직접 채우면 property lookup 뒤
  `agtype` wrapper를 만들고 다시 PostgreSQL scalar로 변환하는 경로를 피할 수 있다. simple
  property projection CustomScan은 이 contract의 첫 적용 지점이며, 이후 ordered/refetch,
  aggregate lower target도 같은 slot descriptor를 재사용해야 한다.
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
- PostgreSQL expression index matching은 planner가 보관한 index expression과 query restriction의
  structural equality에 민감하다. AGE가 property signature로 semantic match를 인정한 뒤에도
  caller에 raw index expression만 넘기면 cached-property slot metadata와 restriction rewrite가
  다시 갈라진다. matched index expression을 설정할 때 slot descriptor에서 canonical field
  expression을 먼저 재구성하면 index, scalar final, typed collect가 같은 property column
  contract를 공유한다.
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

## 되돌린 접근 요약

- Generic comparator micro fast path 반복: benchmark 개선 폭이 작아 구조 후보로 전환.
- Sort key를 final output으로 직접 재사용: cast semantic이 바뀔 수 있어 되돌림.
- Lower path가 output `agtype`를 함께 carry: sort width 증가로 악화되어 되돌림.
- Broad terminal node skip: chained `*0..0` path를 깨뜨려 단일 VLE segment + constrained
  start 조건으로 제한.
- Helper-only property predicate rewrite: expression index matching을 깨뜨려 index-aware
  rewrite로 대체.
