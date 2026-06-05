# AGE 성능 개선 방향

## 현재 브랜치의 범위

이 브랜치는 Apache AGE의 Cypher/VLE/property execution을 PostgreSQL planner와
executor에 더 가깝게 낮춰 성능 병목을 줄이는 실험 브랜치다. fresh install 기준
동작을 우선하며, extension upgrade 하위 호환성은 보장하지 않는다.

## 최근 반영된 기능

- `age_auto_column` marker signature 기반 자동 컬럼 변환.
- AGE built-in function semantic metadata catalog.
- `age_adjacency` 기반 fixed MATCH/edge payload provider.
- VLE stream/custom path와 terminal endpoint/property direct helper.
- Raw path/node/edge builder와 fixed path arity-specific helper 제거.
- Property projection custom scan.
- Scalar property predicate/index-aware rewrite.
- Typed/scalar collect, sort, hash, equality, typecast fast path.
- `gin_agtype_path_ops`와 nested containment recheck fast path.
- `age_upgrade` regression policy smoke test.

## 핵심 방향

### PostgreSQL 표준 구조 재사용

- Cypher parsing 결과를 PostgreSQL planner가 이해할 수 있는 Query/Path/TargetEntry
  구조로 최대한 낮춘다.
- AGE custom cache는 metadata lookup/cache로 제한하고, plan lifecycle은 PostgreSQL
  plan cache와 invalidation에 더 많이 맡긴다.
- CustomScan은 필요한 경우에만 사용하고, path target과 qual pushdown이 PostgreSQL
  planner와 자연스럽게 상호작용하도록 유지한다.

### Materialization 지연

- 최종 출력 직전까지 `agtype` wrapper 생성을 늦춘다.
- property fetch, predicate, projection, aggregate 사이에서는 가능한 한 typed/scalar
  Datum과 compact descriptor를 전달한다.
- path/list/entity 최종 projection이 필요한 shape만 full materialization fallback을
  유지한다.

### VLE boundary 명확화

- `length(p)`, `count(p)`, endpoint-only, property-only consumer는 raw VLE descriptor나
  direct helper로 처리한다.
- arbitrary VLE에서 `nodes(p)`/`relationships(p)` full list, list element projection,
  entity/path agtype projection이 필요한 경우는 semantic 보존을 위해 materialization
  fallback을 둔다.
- Terminal endpoint/property access는 전체 path/list/object materialization 없이 처리한다.

## 남은 구조 후보

- Generic property projection의 final output scalar descriptor.
- HashAggregate/GROUP BY/DISTINCT key를 typed physical value로 유지하는 handoff metadata.
- Ordered projection에서 sort key와 final output을 공유하되 semantic이 바뀌지 않는
  planner/executor contract.
- Global graph cold-load에서 label-unrelated vertex/edge scan을 줄이는 load contract.
- `age_adjacency` payload와 dense VLE traversal state를 더 직접 연결하는 구조.

## 성공 기준

- Fresh install focused regression 통과.
- 주요 workload에서 plan이 의도한 direct/custom path를 사용한다는 assertion 또는
  EXPLAIN 근거.
- `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)`와 lldb call stack 기준으로 wrapper
  materialization 또는 heap/property refetch가 줄어든다.
- 큰 workload benchmark에서 의미 있는 개선이 있을 때만 DEFAULT-ON 판단을 한다.
