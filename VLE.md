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
- 다음 구조 후보는 VLE/property/aggregate 사이의 final output scalar descriptor를 공유하는
  것이다.

## 남은 병목 후보

1. Cold global graph load
   - vertex/edge table full scan
   - label-unrelated load
   - graph-name/label-name lookup 반복

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
- constrained start + terminal anonymous 단일 VLE path.
- nested agtype containment 100k/200k row workload.
- scalar property GROUP BY/DISTINCT/sort/collect synthetic workload.

측정 시 최소 2개 이상을 같이 본다.

- `EXPLAIN (ANALYZE, BUFFERS, VERBOSE)`
- lldb breakpoint/call count
- focused synthetic runtime 반복
- normalized `EXPLAIN` 출력 regression

## 최근 검증 기록

- `age_upgrade` focused installcheck: `All 1 tests passed.`
- clean warning fix: `make clean -j16` 후 `make -j16 COPT=-Werror` 통과.
- VLE/property focused sets는 최근 commit 묶음에서 build/install 후 실행했다.
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
- 단기 micro fast path보다 typed/scalar descriptor handoff와 dense traversal state contract를
  우선한다.
