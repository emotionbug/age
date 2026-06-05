# VLE Follow-up TODO

## 진행 원칙

- TODO는 남은 작업, 바로 다음 실행 계획, 검증 기준만 유지한다.
- 완료 기록은 `HISTORY.md`, 설계 근거는 `RESEARCH.md`, VLE 실행/benchmark 세부는 `VLE.md`,
  index 구조 근거는 `INDEX.md`에 둔다.
- 준비 단계에서 멈추지 않는다. 설계가 필요하면 같은 작업 단위에서 조사, 코드/SQL/test 변경,
  문서 갱신, focused 검증까지 이어서 진행한다.
- 실패하면 되돌리기보다 실패 shape와 깨진 contract를 분리하고 descriptor boundary,
  source/index/cache layout, traversal state, planner/executor handoff에서 돌파구를 찾는다.
- guard/assertion만 추가하지 않는다. source kind, descriptor, planner/executor contract가 더 넓은
  semantic을 올바르게 처리하도록 바꾼다.
- 작은 fixture에서 효용이 작아도 큰 dataset에서 row width 감소, materialization 지연, required
  column 축소, cache/load contract 개선 효과가 크면 우선한다.
- 최우선 참고 소스는 `/Users/emotionbug/IdeaProjects/postgres_proj/pgorca`,
  `/Users/emotionbug/IdeaProjects/neo4j`, `/Users/emotionbug/IdeaProjects/postgres_proj/citus`다.
- 한 파일에 기능을 계속 쌓지 않는다. 책임이 섞이면 같은 작업 단위에서 module boundary 분리까지
  진행한다.
- 보수적 회피보다 공격적이고 진취적인 구조 개선을 우선한다.
- commit은 너무 작게 쪼개지 않는다. 같은 descriptor 전환, caller 정리, 문서 근거, focused 검증은
  의미 있는 하나의 commit으로 묶는다.
- workspace note 문서(`AGENTS.md`, `FEATURES.md`, `HISTORY.md`, `RESEARCH.md`, `TODO.md`,
  `VLE.md`)는 사용자가 명시적으로 요청한 경우에만 commit에 포함한다.

## 현재 우선순위

1. AGE graph pattern join-order descriptor를 실제 후보 테이블로 올린다.
   - 목표: parser 순서 고정 대신 node/property index seek, fixed `AGE Adjacency Match`, VLE
     `ExpandAll`, VLE `ExpandInto`, value join, cartesian/apply를 같은 component/connector vocabulary로
     비교한다.
   - 다음 구현: `AGEGraphJoinComponent`/`AGEGraphJoinConnector` 후보 테이블을 추가한다.
   - component 필드: solved/provided variable bitset, required outer relids, estimated rows, output width.
   - connector 필드: kind, order property, row/cost, source evidence, required/provided variables.
   - parallel 필드: `parallel-safe`, `parallel-aware`, `parallel-workers`, `gather-cost`,
     `order-preserving`, `shared-state-required`.
   - Citus처럼 planner-only 후보와 executor-visible CustomScan payload를 분리한다.
   - 첫 소비 지점: `add_adjacency_match_custom_path()`와 `add_age_vle_stream_custom_path()`.
   - EXPLAIN: 선택된 후보는 기존 `Adjacency Join Order`/`VLE Join Order` line에 계속 드러낸다.
   - 주의: ExpandInto는 두 endpoint가 bound라는 이유만으로 항상 싸게 보지 않는다. source rows가 작거나
     node/property seek로 먼저 좁혀진 shape에서 강하게 둔다.

2. `AGE Adjacency Match`를 graph index/parallel 후보의 첫 실행 대상으로 삼는다.
   - endpoint posting run 단위 partial scan 가능성을 먼저 검토한다.
   - `INDEX.md`의 adjacency payload/index 구조를 기준으로 payload column requirement, terminal label/property
     selectivity, directory fanout을 candidate table 입력으로 올린다.
   - Neo4j식 Cypher DDL/parser 확장으로 graph index create/drop/metadata 조회 surface를 유지한다.

3. `AGE VLE Stream`은 CustomScan 구조를 유지하되 VLE-specific scan node로 더 선명하게 만든다.
   - SQL-visible `age_vle()`/`age_vle_internal()` surface는 되살리지 않는다.
   - `AGE VLE Stream`은 traversal visited state, path uniqueness, payload cache suppression이 worker-local로
     충분한지 확인되기 전까지 parallel-safe descriptor만 만들고 parallel-aware 실행은 별도 단계로 둔다.
   - `ExpandAll`과 `ExpandInto` 후보 비용을 candidate table에서 직접 비교한다.

4. ORCA식 lower/final `PathTarget` descriptor를 property projection/aggregate에 확장한다.
   - collect/property aggregate 함수를 수동으로 계속 나열하지 않는다.
   - required lower expression, cached-property slot, final `agtype` materialization contract를 함께 바꾼다.
   - 관련 entry point는 `cypher_property_paths` module boundary 안에서 정리한다.

5. 큰 fan-out workload에서 VLE/global graph cold-load 병목을 다시 줄인다.
   - 기준 workload는 800-label fan-out과 constrained start + terminal anonymous VLE path다.
   - cold global graph load, label-unrelated scan, endpoint-btree/`age_adjacency` source 선택을 runtime evidence와
     benchmark summary로 비교한다.
   - FalkorDB GraphBLAS 구조는 직접 링크보다 native sparse frontier 후보를 먼저 만든다. 큰 frontier에서
     여러 source vertex를 batch matrix/filter 형태로 묶고, optional GraphBLAS backend는 이후 portability/MVCC
     contract가 정리된 뒤 검토한다.

## 다음 실행 단위

1. fixed adjacency candidate table에 node/property index seek, bound endpoint `age_adjacency`, value join 후보를
   함께 등록하고 cheapest/physical property 선택을 비교한다.
2. VLE candidate table에 `ExpandAll`/`ExpandInto`/composite prefilter 후보를 분리 등록하고 source rows와
   state-sharing cost로 선택한다.
3. 큰 fan-out VLE에 native `matrix-frontier-expand` 후보 descriptor를 추가할 수 있는 source/cache boundary를
   설계한다.
4. selected candidate descriptor가 `Adjacency Join Order`/`VLE Join Order` line에 계속 드러나는지 regression으로
   확인한다.
5. focused regression은 `age_adjacency cypher_vle cypher_match`를 기본으로 돌린다.

## 검증 기준

```sh
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='age_adjacency cypher_vle cypher_match'
git diff --check
```

- argument 없는 전체 `make installcheck`는 기본 검증으로 사용하지 않는다.
- 경고를 오류로 확인해야 하는 요청이 있으면 `make clean` 뒤 `COPT=-Werror` build를 사용한다.
- 이 브랜치의 gate는 fresh install 기준 동작, focused regression, benchmark 근거다.
- extension upgrade 하위 호환성, 예전 catalog/data structure/layout 보존은 gate가 아니다.
- `age_upgrade`는 upgrade parity가 아니라 fresh-install 정책 smoke test로만 본다.
