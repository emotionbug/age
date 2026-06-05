# Graph Database Indexing 기술 조사

작성일: 2026-06-05

목표는 Apache AGE 및 PostgreSQL 기반 Graph Database에서 Cypher query 성능을
개선할 수 있는 차세대 index architecture를 정리하는 것이다. 2023~2026년 자료를
기준으로 보면, AGE에 가장 실용적인 방향은 단일 property index 확대가 아니라
다음 네 계층을 함께 갖추는 것이다.

1. property lookup/range filter용 expression/B-tree/GIN index.
2. source/destination endpoint별 adjacency index.
3. path/pattern/WCOJ용 sorted adjacency, trie, compressed graph index.
4. graph statistics catalog와 selectivity estimator.

현재 이 브랜치에는 `ag_catalog.create_property_index(...)`와 `age_adjacency` index
access method가 이미 존재한다. `age_adjacency`는 `CREATE ACCESS METHOD
age_adjacency TYPE INDEX`로 등록되며, payload v4 index가
`(endpoint_id, edge_id, next_vertex_id)` 형태의 세 `graphid` key column을 요구한다.
SQL-visible key column contract는 유지하지만, bulk-built main posting은 directory run
key와 중복되는 `endpoint_id`를 저장하지 않고 `(heap_tid, edge_id, next_vertex_id)`만
보관한다. insert delta posting은 unordered scan을 위해 full key를 유지한다. 따라서 AGE의
다음 단계는 PostgreSQL에 완전히 낯선 구조를 새로 붙이는 것보다, 기존 property index와
adjacency AM을 graph statistics, path index, pattern-aware executor와 연결하는 것이다.

## 요약 결론

- Property index는 시작 vertex/edge 후보를 줄이는 데 강하지만 multi-hop fan-out,
  triangle, repeated variable, path uniqueness는 해결하지 못한다.
- Path index는 bounded VLE에서 반복 BFS/DFS를 줄이지만 저장 공간과 update 비용이
  커진다. AGE에는 full transitive closure보다 bounded materialized path summary와
  CSR/bitmap frontier cache가 현실적이다.
- Pattern index는 triangle/fork/cycle 같은 반복 workload에는 강하지만 query shape가
  다양하면 유지보수 비용이 높다.
- WCOJ + trie/sorted adjacency index는 triangle, 4-cycle, diamond pattern에서 binary
  join의 intermediate 폭발을 줄인다. AGE에 장기적으로 가장 큰 성능 개선 여지가 있다.
- Graph statistics catalog는 모든 index 선택의 전제다. degree histogram, top-k heavy
  vertex, `(src_label, edge_label, dst_label)` count 없이 planner는 index가 있어도 잘못된
  방향으로 expansion할 수 있다.
- PostgreSQL extension 수준에서 가능한 것은 property expression index, custom operator
  class, custom index AM, planner hook, custom scan/SRF, side catalog다. core 수정이
  필요한 것은 PostgreSQL planner가 graph pattern을 first-class join/search object로
  이해하는 일반화된 WCOJ/path operator다.

## 기술별 분석

### Property Graph Indexing

핵심 아이디어:

- vertex/edge label과 property value를 key로 하여 entity id 또는 heap TID를 찾는다.
- Neo4j의 range/text/point/full-text/vector index, Memgraph의 label-property/edge-property
  index, JanusGraph의 graph index가 이 범주다.

대표/최신 자료:

- Neo4j 2025/2026 문서: range, point, text, full-text, token lookup, vector index를
  제공한다. relationship type property index와 composite range index도 지원한다.
- Memgraph 문서와 2025~2026 release/blog: label-property composite, edge-type,
  edge-type property, global edge property, point, vector edge index를 제공한다.
- JanusGraph 문서: global graph index와 vertex-centric index를 구분한다.
- PostgreSQL 18 문서: B-tree, Hash, GiST, SP-GiST, GIN, BRIN 및 extension AM을 제공한다.

자료구조:

- equality/range: B-tree, skip list, LSM/B-tree backend, sorted dictionary.
- document/string: inverted index 또는 Lucene full-text.
- JSON/agtype: expression index 또는 GIN-style key posting list.
- vector: HNSW/ANN 계열.

복잡도:

- 저장 공간: `O(N indexed entities * key_size)` plus posting/TID overhead.
- 탐색: equality/range는 보통 `O(log N + K)`, inverted index는 `O(term lookup + posting)`.
- update: insert/delete/update property당 `O(log N)` 또는 posting list update. full-text와
  vector는 background/eventual consistency 선택지가 있을 수 있다.

장점:

- Query 1~3, 5~6에서 시작점과 종단점 후보를 빠르게 줄인다.
- PostgreSQL extension 수준 구현이 가장 쉽다.
- `CREATE INDEX`와 planner selectivity model에 자연스럽게 연결된다.

단점:

- edge traversal 자체를 빠르게 하지 않는다.
- `MATCH (a)-[]->(b)-[]->(c)`에서 `a`와 `c`를 찾은 뒤 연결성 검사는 여전히 adjacency
  access가 필요하다.
- property correlation과 degree skew를 반영하지 못하면 잘못된 시작점을 고른다.

적용 사례/오픈소스:

- Neo4j, Memgraph, JanusGraph, Neptune, PostgreSQL, AGE 모두 property index 계층을 가진다.
- AGE는 `create_property_index`를 제공하며 PostgreSQL index 기능을 이용할 수 있다.

PostgreSQL/AGE 적용 가능성:

- PostgreSQL: 매우 높음. expression index, GIN opclass, generated column, partial index로
  가능하다.
- AGE: 매우 높음. `agtype` property extraction 함수를 immutable/stable하게 설계하고
  label relation별 expression index를 생성하면 된다.

Planner/Executor 고려사항:

- Cypher `a.country = 'KR'`를 PostgreSQL가 indexable qual로 볼 수 있게 rewrite해야 한다.
- property value type 안정성이 중요하다. `agtype` 안의 numeric/string comparison semantics와
  PostgreSQL operator class가 일치해야 한다.
- `MATCH (a {id: 100})`는 label이 없으면 모든 vertex label relation 후보가 생긴다. label
  없는 property lookup을 지원하려면 global property index 또는 label union plan이 필요하다.

### Graph Pattern Indexing

핵심 아이디어:

- 자주 등장하는 graph pattern의 match set 또는 partial binding을 index로 보관한다.
- 예: triangle `(a)-[]->(b)<-[]-(c)` with filters, 2-hop path, fork/star, motif index.

대표/최신 자료:

- "Indexing Patterns in Graph Databases" 계열의 pattern index 개념.
- MV4PG(2024): property graph materialized view를 제안하고 view creation, maintenance,
  query optimization using views를 다룬다. variable-length edge를 포함한 templated
  maintenance를 TuGraph/Neo4j 대상으로 실험했다.
- LDBC/SNB 반복 query workload에서 materialized pattern/view는 실용성이 크다.

자료구조:

- pattern signature -> binding table.
- compressed posting: `a_id -> sorted b_id list`, `(a,c) -> b list`.
- factorized pattern table: `center b -> a-list, c-list`.
- maintenance log와 delta view.

복잡도:

- 저장 공간: pattern 결과 크기에 비례한다. triangle/fork는 최악 `O(|E|^{3/2})` 또는
  결과 수만큼 커질 수 있다.
- 탐색: exact pattern lookup은 `O(log M + K)`, prefix lookup은 sorted/factorized layout에
  따라 `O(log M + output)`.
- update: edge insert/delete가 여러 pattern result를 invalidation한다. triangle index는
  edge update 시 양끝 adjacency intersection 비용이 필요하다.

장점:

- Query 4 triangle/fork와 Query 6 mixed 2-hop pattern에서 매우 강하다.
- repeated analytics/dashboard query에는 materialized view로 큰 효과를 낸다.

단점:

- ad hoc Cypher에는 index 폭발이 생긴다.
- update-heavy OLTP에서는 maintenance cost가 크다.
- Cypher path uniqueness, duplicate semantics, property filter 변화에 따라 view matching이
  복잡하다.

적용 사례/오픈소스:

- TuGraph/Neo4j를 대상으로 한 MV4PG prototype 연구.
- RDF materialized view/reasoning store는 유사 사례.
- PostgreSQL materialized view 자체는 존재하지만 graph pattern-aware rewrite는 없다.

PostgreSQL/AGE 적용 가능성:

- PostgreSQL: 중간. materialized view와 trigger/incremental maintenance로 가능하지만
  graph rewrite는 extension이 담당해야 한다.
- AGE: 중간~높음. 특정 pattern benchmark용으로는 side table과 invalidation trigger를
  만들 수 있다.

Planner/Executor 고려사항:

- Cypher pattern canonicalization이 필요하다.
- view matching이 PostgreSQL rewrite system에 들어가기 어렵기 때문에 AGE planner 단계에서
  pattern index 후보를 주입해야 한다.
- MVCC snapshot과 view freshness 정책을 명확히 해야 한다.

### Path Indexing 및 Materialized Path Index

핵심 아이디어:

- multi-hop reachability/path query를 위해 path, bounded transitive closure, landmark,
  2-hop labeling, CSR frontier, path summary를 저장한다.

대표/최신 자료:

- DuckPGQ CIDR/VLDB 2023 demo: SQL/PGQ path-finding을 recursive SQL로 내리지 않고
  on-the-fly CSR를 만들고 MS-BFS를 수행한다.
- MV4PG(2024): variable-length edge를 포함한 materialized view maintenance를 다룬다.
- 2024 PIM/accelerator 연구와 path algebra 연구는 regular path query 가속을 다룬다.

자료구조:

- CSR/CSC: vertex offset array + edge destination array.
- bounded path table: `(src, dst, min_depth, max_depth, path_count/representative_path)`.
- 2-hop labeling: vertex별 in-label/out-label set.
- landmark index: selected hub까지 거리/parent.
- path materialization DAG: path prefix parent pointer + edge id vector.

복잡도:

- CSR 저장 공간: `O(|V| + |E|)`.
- full transitive closure: 최악 `O(|V|^2)`.
- bounded depth `d` path index: 최악 `O(sum_v degree(v)^d)`로 폭발 가능.
- 탐색: CSR BFS는 `O(visited_edges)`; closure lookup은 `O(log N)` 또는 `O(1)`.
- update: CSR append-friendly가 아니면 rebuild/delta 필요. closure/path index는 edge update
  하나가 많은 pair에 영향을 줄 수 있다.

장점:

- Query 5 `*1..5`에서 큰 효과.
- repeated path-finding, many-source/many-destination workload에 강하다.
- AGE VLE executor와 직접 연결 가능하다.

단점:

- 모든 path를 반환하는 Cypher는 path 수 자체가 지수적으로 커질 수 있다.
- update-heavy graph에서 consistency와 invalidation이 어렵다.
- path uniqueness semantics를 index에 반영해야 한다.

적용 사례/오픈소스:

- DuckPGQ는 open-source DuckDB extension 방향의 대표 사례이며 CSR/MS-BFS를 설명한다.
- Neo4j/Graph DBMS는 내부 adjacency storage와 path operators를 사용하지만 materialized
  path index를 일반 DDL로 노출하지는 않는다.

PostgreSQL/AGE 적용 가능성:

- PostgreSQL: 낮음~중간. recursive CTE는 가능하지만 CSR/path operator는 custom function
  또는 custom scan이 필요하다.
- AGE: 높음. 현재 VLE/adacency cache 구조에 CSR-like snapshot 또는 bounded path summary를
  추가할 수 있다.

Planner/Executor 고려사항:

- `RETURN p`가 path object를 요구하면 representative path와 all paths semantics를 구분해야 한다.
- terminal filter `b.country='US'`를 BFS 중 early check할 수 있어야 한다.
- PostgreSQL index AM은 tuple lookup 중심이므로 path traversal은 index scan보다 custom
  executor node가 더 자연스럽다.

### Materialized Pattern Index

핵심 아이디어:

- materialized path index가 linear path에 초점을 둔다면, materialized pattern index는
  triangle, fork, k-hop with filters 같은 subgraph pattern 결과를 저장한다.

대표/최신 자료:

- MV4PG(2024).
- Kùzu/GraphflowDB의 factorized intermediate는 materialized pattern index와 직접 같지는
  않지만 pattern result를 factorized layout으로 다루는 설계 참고가 된다.

자료구조/복잡도:

- 저장 공간: materialized result size. factorized view를 쓰면 flat tuple보다 작아질 수 있다.
- 탐색: pattern signature lookup + bound variable prefix scan.
- update: affected local neighborhood recomputation. triangle은 새 edge `(u,v)` 삽입 시
  `N_in(v)`, `N_out/u` 등 관련 adjacency intersection이 필요하다.

장점:

- Query 4, Query 6에서 filter가 고정되거나 반복되면 강하다.
- dashboard, fraud motif, recommendation motif에 적합하다.

단점:

- view selection 문제가 어렵고 workload drift에 취약하다.
- AGE core에 넣기보다 optional advisor/extension 기능이 적합하다.

### Trie-based Index

핵심 아이디어:

- relation/triple/edge를 variable order에 맞춘 trie로 저장해 prefix binding과 intersection을
  빠르게 한다. Leapfrog Triejoin, Generic Join, WCOJ의 핵심 물리 구조다.

대표/최신 자료:

- Leapfrog Triejoin, Generic Join은 고전 기반.
- ADOPT(2023)는 WCOJ attribute order 선택 문제를 adaptive/RL로 다룬다.
- "New Compressed Indices for Multijoins on Graph Databases" (2024)는 WCO multijoin을
  compact representation 위에서 지원하고 query time을 개선한다.

자료구조:

- edge relation `E(src, dst, edge_id, props)`를 `(src,dst)`, `(dst,src)`, `(src,label,dst)`
  order trie로 저장.
- 각 depth는 sorted unique key array와 child range offset을 가진다.
- RDF triple store의 SPO/POS/OSP permutation index도 trie/B-tree prefix index로 볼 수 있다.

복잡도:

- 저장 공간: permutation 수 `p`에 대해 `O(p * |E|)`가 기본. compressed trie는 delta,
  bitmap, Elias-Fano 등으로 줄인다.
- 탐색: prefix lookup `O(depth * log fanout + K)`, sorted intersection은 list 길이에 비례.
- update: dynamic trie는 `O(log N)` 가능하지만 compressed/static trie는 batch rebuild가 유리.

장점:

- Query 4 triangle/fork, WCOJ, repeated variable pattern에 강하다.
- binary join intermediate를 만들지 않고 prefix를 확장한다.

단점:

- 여러 variable order를 지원하려면 permutation index가 늘어난다.
- PostgreSQL B-tree 여러 개와 달리 executor가 trie cursor를 이해해야 한다.

적용 사례/오픈소스:

- RDF-3X/Hexastore/QLever/LogicBlox/EmptyHeaded 계열.
- Kùzu/GraphflowDB는 WCOJ/factorized execution 참고 구현.
- PostgreSQL 직접 구현체는 제한적이다.

PostgreSQL/AGE 적용 가능성:

- PostgreSQL: 낮음. index AM으로 trie scan은 가능하지만 multi-relation synchronized cursor는
  executor 확장이 필요하다.
- AGE: 중간~높음. adjacency AM과 VLE cache에 sorted adjacency/trie cursor API를 추가하고
  Cypher executor가 사용하게 할 수 있다.

Planner/Executor 고려사항:

- variable order cost model이 필요하다.
- `IndexScan` 하나가 아니라 여러 edge label index cursor를 lockstep으로 움직이는
  `AgeWCOJScan` 같은 custom node가 필요하다.

### WCOJ용 Index

핵심 아이디어:

- WCOJ는 attribute-at-a-time join을 수행하므로 각 relation이 여러 attribute order의
  sorted access path를 제공해야 한다.

대표/최신 자료:

- Kùzu CIDR 2023: binary join과 WCOJ multiway join operator를 함께 사용.
- GraphflowDB thesis(2023): WCOJ + factorized vector execution.
- "New Compressed Indices for Multijoins on Graph Databases" (2024): compact WCO multijoin
  index.

자료구조:

- `E(src,dst)` forward adjacency, `E(dst,src)` reverse adjacency.
- edge label/type partitioned adjacency.
- property-filtered posting 또는 late property verification.
- intersection-friendly sorted graphid list.

복잡도:

- 탐색: AGM bound에 맞는 WCOJ 실행을 지원. triangle은 `O(|E|^{3/2})` bound 가능.
- 공간: forward/reverse 2개면 `O(2|E|)`, label/property permutation까지 넣으면 증가.
- update: 각 permutation에 insert/delete. compressed static layout은 delta + merge 필요.

장점:

- Query 4와 cyclic pattern에서 가장 큰 구조적 개선.
- high-degree vertex의 intermediate explosion을 줄인다.

단점:

- acyclic chain에는 과투자일 수 있다.
- PostgreSQL planner의 binary join tree와 맞지 않는다.

PostgreSQL/AGE 적용:

- AGE 전용 custom executor로 먼저 구현하는 것이 현실적이다.
- PostgreSQL core 수정 없이도 SRF/custom scan 형태로 가능하지만 cost/EXPLAIN/parallel
  통합은 제한된다.

### Graph Cardinality Estimation Structures

핵심 아이디어:

- index가 있어도 planner가 cardinality를 잘못 추정하면 잘못된 index와 expansion 방향을
  고른다. graph-specific synopsis를 catalog화해야 한다.

대표/최신 자료:

- HomeRun(2024): graph database cardinality estimation advisor.
- GNCE(2023, revised 2024): KG embedding + GNN cardinality estimation.
- property graph semantic feature transfer CE(2024).
- hyper-relational KG CE(2024/2025).

자료구조:

- label/edge label count.
- `(src_label, edge_label, dst_label)` count.
- direction별 degree histogram, quantile, top-k heavy vertex.
- property MCV/histogram by label/type.
- path length sample, random walk sample, two-hop/fork summary.
- pattern sketch: triangle count, wedge count, clustering coefficient.

복잡도:

- 저장 공간: `O(labels^2 * edge_types + histograms + top-k)`.
- 탐색: planner lookup은 `O(pattern_edges)`.
- update: ANALYZE/rebuild 또는 incremental counters. high-throughput OLTP에서는 stale
  허용 정책 필요.

장점:

- Query 2~6에서 시작점, direction, index 선택의 품질을 높인다.
- property index와 adjacency index를 연결하는 핵심 계층이다.

단점:

- 통계 수집 비용과 stale 통계 문제가 있다.
- label 없는 pattern과 dynamic property key가 많으면 catalog가 커진다.

PostgreSQL/AGE 적용:

- AGE side catalog로 구현 가능.
- PostgreSQL `ANALYZE` hook과 `pg_statistic_ext`를 재사용할 수 있으나 graph-specific
  degree/path 통계는 AGE 내부 catalog가 더 자연스럽다.

### Learned Index

핵심 아이디어:

- key distribution 또는 graph query workload를 학습해 lookup/ranking/selectivity를 보조한다.
- graph context에서는 learned cardinality estimator, learned plan re-ranker, vector/ANN
  graph index가 더 실용적이다.

대표/최신 자료:

- Learned Query Optimizers survey(2024), Lero(2024), VLDB 2024 learned optimizer 평가.
- Hybrid Multimodal Graph Index(HMGI, 2025)는 vector search와 graph traversal 결합을 다룬다.
- Memgraph/Neo4j는 2025~2026 vector index를 graph engine 안으로 통합하고 있다.

자료구조:

- CDF model + fallback B-tree.
- HNSW/ANN graph for vector properties.
- learned CE model artifact + feature store.

복잡도:

- lookup은 모델 inference `O(1)` plus local correction search.
- update는 model drift와 retraining 비용이 핵심.
- vector HNSW는 저장 공간 `O(N * M)` edges, search `O(ef_search * log N)` 계열.

장점:

- GraphRAG/semantic graph query, property distribution이 안정적인 workload에 유리.
- selectivity estimation 보정에 유용하다.

단점:

- AGE core 성능 문제인 structural traversal에는 직접 효과가 제한적이다.
- 운영 복잡도와 재현성 문제가 크다.

PostgreSQL/AGE 적용:

- PostgreSQL: pgvector/HNSW, learned re-ranker extension 방식 가능.
- AGE: vector property index와 graph traversal 결합은 가능하지만 기본 Cypher pattern
  최적화의 1순위는 아니다.

### Adaptive Index

핵심 아이디어:

- workload와 runtime feedback에 따라 index를 생성, 재구성, delta merge, hot path cache를
  조정한다.

대표/최신 자료:

- qEndpoint(2024): read-optimized compressed HDT main partition과 write-optimized RDF4J
  delta partition을 병행하고 merge한다.
- Neo4j는 plan cache/replanning과 full-text eventual consistency 옵션을 제공한다.
- Memgraph 3.4(2025)는 non-blocking index creation과 edge vector index를 발표했다.

자료구조:

- main/delta index, hot adjacency cache, auto-created property index advisor.
- read-mostly compressed CSR + write delta posting.

복잡도:

- read는 main + delta merge iterator.
- update는 delta에 빠르게 쓰고 background merge.
- merge 비용은 batch size와 전체 index size에 비례한다.

장점:

- AGE의 read-heavy analytical branch와 fresh install 실험 정책에 잘 맞는다.
- compressed static index와 update workload를 절충한다.

단점:

- snapshot visibility와 WAL/recovery가 복잡하다.
- planner가 main/delta cost를 알아야 한다.

### Compressed Graph Index

핵심 아이디어:

- adjacency, trie, RDF permutation index를 압축해 memory residency를 높이고 sequential
  scan/intersection 성능을 개선한다.

대표/최신 자료:

- DuckPGQ CSR(2023).
- qEndpoint/HDT(2024): compressed main partition.
- "New Compressed Indices for Multijoins on Graph Databases" (2024).
- QLever/HDT/k2-tree/RDFCSA 계열 compact RDF store.

자료구조:

- CSR/CSC with delta-coded sorted graphid.
- bitmap index, Roaring bitmap, Elias-Fano list.
- HDT dictionary + compressed triple arrays + bitmaps.
- compressed trie/suffix array for RDF triple patterns.

복잡도:

- 저장 공간: plain edge table보다 작거나 비슷할 수 있다. qEndpoint는 WDBench index size
  비교에서 19.7GB로 Jena/Neo4j/Virtuoso/Blazegraph보다 작다고 보고했다.
- 탐색: sequential compressed list scan + skip/select support.
- update: 대부분 delta + rebuild/merge가 유리하다.

장점:

- graph traversal은 random heap lookup보다 compact sequential adjacency 접근이 유리하다.
- WCOJ list intersection에 적합하다.

단점:

- MVCC tuple visibility와 직접 충돌한다.
- PostgreSQL heap/TID 기반 index AM과 다른 수명 모델이 필요하다.

### RDF / Triple Store Index Structures

핵심 아이디어:

- RDF triple/quad `(S,P,O,G)`의 여러 permutation index를 유지해 SPARQL triple pattern을
  prefix search로 처리한다.

대표/최신 자료/제품:

- Amazon Neptune: 기본 `SPOG`, `POGS`, `GPSO` 세 index를 유지하고, Lab Mode로 `OSGP`를
  추가할 수 있다. OSGP는 insert를 최대 23% 늦추고 storage를 최대 20% 늘릴 수 있다고
  문서화되어 있다.
- Virtuoso: `PSOG` primary, `POGS` bitmap, `SP`, `OP`, `GS` partial index.
- Blazegraph: B+Tree statement index와 `SPOKeyOrder` 계열.
- RDFox: in-memory tuple table과 triple/quad table type을 제공한다.
- qEndpoint(2024): HDT main + RDF4J delta 구조.

자료구조:

- B+Tree permutation: SPO, POS, OSP 등.
- bitmap partial index.
- dictionary-encoded IDs.
- tuple table / columnar compressed arrays.

복잡도:

- 저장 공간: permutation 수만큼 `O(pN)`, compressed/bitmap이면 감소.
- 탐색: bound prefix는 `O(log N + K)`, unbound leading key는 scan 또는 다른 permutation 필요.
- update: permutation 수만큼 insert/delete. compressed main은 delta merge.

장점:

- property graph edge table에도 그대로 응용 가능하다.
- AGE edge를 `(src, type, dst, edge_id)` statement로 보면 forward/reverse/type index 설계가
  명확해진다.

단점:

- RDF는 edge property를 별도 statement로 표현하므로 LPG edge property filtering과 layout이 다르다.
- 모든 permutation을 유지하면 write amplification이 크다.

### Hypergraph 기반 Indexing

핵심 아이디어:

- query pattern을 hypergraph로 보고, 자주 쓰이는 hyperedge/predicate set에 맞는 composite
  index와 materialized structure를 선택한다.

대표/최신 자료:

- DPhyp/DPccp hypergraph optimizer.
- PostgreSQL용 `pg_dphyp`.
- Factorized/WCOJ 연구는 hypergraph width/AGM bound와 연결된다.

자료구조:

- hyperedge signature -> candidate relation/index.
- multi-attribute composite index.
- join synopsis: subset cardinality, connected component count, fractional edge cover estimate.

복잡도:

- 저장 공간은 선택한 hyperedge 수에 비례.
- 탐색은 composite prefix lookup 또는 WCOJ cursor intersection.
- update는 hyperedge index 수만큼 증가.

장점:

- Query 6처럼 vertex property + edge property + terminal property가 섞인 pattern에서
  단일 index보다 좋은 index family를 고를 수 있다.

단점:

- PostgreSQL index selection은 relation-local이라 cross-relation hyperedge index를 직접
  이해하지 못한다.

### Graph Statistics Catalog 및 Selectivity Estimation

핵심 아이디어:

- index catalog와 통계 catalog를 분리하지 말고 함께 설계한다. graph index는 “어떤 access
  path가 있는가”뿐 아니라 “어느 방향이 작은가”가 핵심이다.

필수 catalog:

- `ag_graph_stats`: graph-level vertex/edge count, last analyze xid/time.
- `ag_label_stats`: label별 count, property null fraction, MCV/histogram.
- `ag_edge_stats`: edge label/type별 count, source/destination label count.
- `ag_degree_stats`: direction별 histogram, quantile, top-k heavy vertex.
- `ag_path_stats`: label/type sequence별 depth selectivity sample.
- `ag_pattern_stats`: wedge/triangle/cycle sample count.

Planner 통합:

- Cypher pattern IR에서 each variable의 candidate cardinality를 계산한다.
- property selectivity와 degree fan-out을 곱하되, correlation 보정과 cap을 둔다.
- VLE는 depth별 branching factor와 visited uniqueness 보정을 쓴다.

## 시스템별 인덱스 구조 비교

| 시스템 | 주요 index 구조 | 장점 | 한계/AGE 참고점 |
| --- | --- | --- | --- |
| PostgreSQL | B-tree, Hash, GiST, SP-GiST, GIN, BRIN, custom AM | 확장성, WAL/MVCC, expression/partial index | graph traversal/pattern은 first-class가 아님 |
| Apache AGE | label relation, `create_property_index`, `age_adjacency` AM, VLE cache | PostgreSQL index를 재사용하고 custom AM도 가능 | Cypher pattern과 PostgreSQL planner 사이 정보 손실 |
| Neo4j | token lookup, range, text, point, full-text(Lucene), vector, relationship property index | Cypher planner와 통합, graph-native adjacency | 일반 materialized path/pattern index는 제한적 |
| Memgraph | skip-list 기반 label/property, composite, edge-type/property, point/vector | in-memory update/read 빠름, edge vector까지 확장 | PostgreSQL/AGE와 storage model이 다름 |
| TigerGraph | type/attribute 기반 storage, cost-based optimizer 통계, graph traversal plan | distributed graph traversal과 통계 기반 plan | proprietary, index 내부 구조 공개 제한 |
| DuckDB/DuckPGQ | relational indexes + on-the-fly CSR/MS-BFS | SQL/PGQ path query에 CSR가 강함 | update-oriented graph index는 아님 |
| Umbra | analytical relational engine, SQL/PGQ 연구 비교 대상 | compiled/vectorized execution | 공개 구현 제한 |
| RDFox | in-memory tuple tables, triple/quad table type, reasoning materialization | reasoning + query 통합 | LPG edge property와 다름, proprietary |
| Blazegraph | B+Tree statement indices, SPO key orders, bloom/temp BTree | RDF permutation index 성숙 | 프로젝트 활동성/AGE 직접 활용 제한 |
| Virtuoso | RDF_QUAD `PSOG`, `POGS`, `SP`, `OP`, `GS`, bitmap/column-wise | web-scale RDF 경험, compact column-wise index | LPG Cypher와 직접 모델 차이 |
| Amazon Neptune | `SPOG`, `POGS`, `GPSO`, optional `OSGP` | managed graph, permutation tradeoff 명확 | custom index 불가, OSGP는 empty cluster에서만 |
| JanusGraph | graph index, mixed index, composite index, vertex-centric relation index | supernode traversal에 vertex-centric index 유용 | backend 의존, Cypher가 아니라 Gremlin 중심 |

## Cypher 워크로드별 분석

### Query 1: Vertex Property Lookup

```cypher
MATCH (a {id: 100})-[]->(b)
RETURN b
```

현재 AGE 예상 실행:

- label이 없으면 graph 내 vertex label relation을 넓게 보거나 property extraction filter를
  수행한다.
- `a` 후보를 찾은 뒤 outgoing edge label relation 또는 adjacency index/cache로 `b`를 확장한다.

PostgreSQL B-tree 한계:

- `properties->'id'` extraction이 indexable expression으로 노출되지 않으면 seq scan이다.
- `a` lookup은 해결하지만 `a -> b` adjacency는 별도 edge endpoint index가 필요하다.

Cardinality 문제:

- label 없는 `{id:100}`의 global uniqueness를 알 수 없다.
- `a`의 out-degree가 heavy인지 모르면 expansion cost를 틀린다.

적용 index:

- global/label별 property index on `id`.
- unique constraint 가능하면 selectivity 1.
- forward `age_adjacency(start_id, id, end_id)`.

성능 향상 요인:

- vertex scan 제거, heap random lookup 감소, outgoing edge list direct scan.

확장 포인트:

- expression index 또는 AGE `create_property_index`.
- `age_adjacency` AM.
- Cypher rewrite가 property qual과 adjacency function을 index-aware path로 내려야 한다.

Extension/Core:

- extension 수준 가능. core 수정 불필요.

### Query 2: Vertex + Edge Property Filtering

```cypher
MATCH (a)-[r]->(b)
WHERE a.country = 'KR'
AND r.weight > 0.8
RETURN b
```

현재 AGE 예상 실행:

- `a.country` filter 후보를 만든 뒤 outgoing edge를 확장하고 `r.weight`를 filter하거나,
  edge relation을 scan하면서 property filter 후 endpoint join을 수행한다.

B-tree 한계:

- vertex property와 edge property는 서로 다른 relation/local index다.
- `r.weight > 0.8` index를 써도 `a.country='KR'`와 연결된 edge인지 late check가 필요하다.

Cardinality 문제:

- `country='KR'` selectivity와 `weight>0.8` edge selectivity의 correlation을 모른다.
- KR vertex의 out-degree 분포가 전체 평균과 다를 수 있다.

적용 index:

- `a.country` property index.
- edge property range index on `weight`.
- endpoint + property composite edge index: `(start_id, weight, edge_id, end_id)` 또는
  `(edge_label, weight, start_id, end_id)`.
- vertex-centric edge property index.

성능 향상 요인:

- KR vertex 집합이 작으면 adjacency-first.
- high weight edge가 작으면 edge-property-first.
- graph stats가 두 plan 중 하나를 선택한다.

확장 포인트:

- edge label relation expression index.
- `age_adjacency`에 property payload 또는 edge property prefilter hook 추가.
- custom selectivity estimator.

Extension/Core:

- basic은 extension 가능.
- endpoint+property를 하나의 physical index로 planner가 일반적으로 이해하려면 AGE planner
  확장이 필요하지만 core 수정은 필수는 아니다.

### Query 3: Multi-hop Pattern Query

```cypher
MATCH (a)-[]->(b)-[]->(c)
WHERE a.age > 30
AND c.name = 'OpenAI'
RETURN a, b, c
```

현재 AGE 예상 실행:

- `a.age` 후보에서 forward 2-hop 또는 `c.name` 후보에서 reverse 2-hop.
- PostgreSQL planner가 graph direction selectivity를 모르거나 Cypher executor 내부에서
  고정 방향을 쓸 수 있다.

B-tree 한계:

- `a.age`와 `c.name` lookup은 가능하지만 중간 `b` 연결성은 B-tree join/edge endpoint lookup
  반복이다.

Cardinality 문제:

- `c.name='OpenAI'`가 unique인지, OpenAI로 들어오는 in-degree가 얼마인지 중요하다.
- `age>30` vertex의 outgoing degree가 전체와 다를 수 있다.

적용 index:

- endpoint property indexes.
- forward/reverse adjacency index.
- 2-hop path summary `(src, mid, dst)` 또는 `(src,dst,count)` bounded index.
- CSR/CSC for reverse traversal.

성능 향상 요인:

- selective terminal에서 reverse expand.
- 2-hop materialized summary가 있으면 중간 edge traversal 감소.

확장 포인트:

- graph stats catalog.
- reverse `age_adjacency(end_id, id, start_id)`.
- AGE pattern reorder.

Extension/Core:

- extension 가능. general SQL planner 통합은 어려우나 AGE Cypher planner에서 가능.

### Query 4: Triangle/Fork Pattern

```cypher
MATCH (a)-[]->(b)<-[]-(c)
WHERE a.country = 'KR'
AND c.country = 'US'
RETURN a, b, c
```

현재 AGE 예상 실행:

- KR `a` 후보에서 outgoing to `b`, US `c` 후보에서 outgoing to same `b`, 이후 join/filter.
- binary join이면 `b`별 fan-in product가 커질 수 있다.

B-tree 한계:

- property index는 `a`, `c` 후보만 줄인다.
- `same b` constraint를 multiway intersection으로 처리하지 못한다.

Cardinality 문제:

- KR -> b와 US -> b의 overlap을 추정하기 어렵다.
- high in-degree `b`가 있으면 중간 결과가 폭발한다.

적용 index:

- `country` property index.
- reverse/forward sorted adjacency.
- WCOJ trie index.
- materialized fork/triangle pattern index: `b -> KR_in_sources, US_in_sources`.
- degree heavy-hitter stats.

성능 향상 요인:

- WCOJ는 `b`를 prefix로 잡고 candidate `a`, `c` list를 filter/intersect한다.
- factorized output은 `b -> {a-list}, {c-list}`로 유지해 aggregation/filter 시 flat product를
  늦춘다.

확장 포인트:

- `AgeWCOJScan` custom node 또는 SRF.
- sorted adjacency cursor API.
- pattern statistics.

Extension/Core:

- AGE custom executor로 extension 가능.
- PostgreSQL core binary join tree만으로 일반화하려면 core 수정 또는 큰 planner patch가 필요.

### Query 5: Variable Length Path

```cypher
MATCH p=(a)-[*1..5]->(b)
WHERE a.id = 100
AND b.country = 'US'
RETURN p
```

현재 AGE 예상 실행:

- `a.id=100`으로 source를 찾고 VLE executor가 depth 1~5 forward traversal.
- terminal `b.country='US'`는 traversal 중 또는 결과 후 filter될 수 있다.

B-tree 한계:

- source/terminal property lookup만 해결한다.
- recursive expansion은 edge endpoint lookup 반복과 visited/path state 관리가 병목이다.

Cardinality 문제:

- branching factor가 depth마다 달라지고 cycle/path uniqueness로 단순 `degree^d` 추정이 틀린다.
- terminal US selectivity가 depth별 frontier와 독립이 아니다.

적용 index:

- source `id` unique property index.
- terminal `country` bitmap/posting set.
- CSR/CSC path index, bounded path summary, landmark/2-hop index.
- adaptive VLE frontier cache.

성능 향상 요인:

- CSR sequential traversal.
- terminal country bitmap으로 frontier early pruning.
- repeated source query는 bounded path cache 재사용.

확장 포인트:

- VLE executor 내부 CSR snapshot.
- graph stats depth histogram.
- path materializer lazy output.

Extension/Core:

- AGE extension 수준 가능.
- PostgreSQL recursive CTE/core 수정은 필요하지 않다.

### Query 6: Mixed Graph Query

```cypher
MATCH (a)-[r1]->(b)-[r2]->(c)
WHERE a.country = 'KR'
AND r1.weight > 0.8
AND c.industry = 'AI'
RETURN a, b, c
```

현재 AGE 예상 실행:

- `a.country`, `c.industry`, `r1.weight` 중 하나를 시작점으로 고르고 2-hop 연결성을 검사한다.
- 통계가 약하면 고정 순서 또는 잘못된 binary join 순서가 될 수 있다.

B-tree 한계:

- 세 predicate가 각각 다른 entity에 걸려 composite selectivity를 직접 알 수 없다.
- `r1` property와 endpoint adjacency를 한 번에 만족하는 index가 없으면 late filter가 많다.

Cardinality 문제:

- KR vertices의 high weight edge 비율, AI industry terminal로 이어지는 2-hop 확률이 필요하다.

적용 index:

- vertex property indexes.
- edge endpoint+property index.
- 2-hop pattern summary with terminal label/property class.
- hypergraph index advisor: `{a.country, r1.weight, c.industry}` pattern cost.
- WCOJ/trie if repeated/cyclic extension exists.

성능 향상 요인:

- 가장 selective predicate에서 시작.
- edge property filter를 adjacency access 안으로 pushdown.
- terminal candidate set bitmap으로 second hop pruning.

확장 포인트:

- AGE property/edge stats.
- custom path that accepts bound candidate sets.
- planner hook 또는 Cypher planner cost model.

Extension/Core:

- AGE extension 수준 가능.
- PostgreSQL core 수정은 general optimizer 통합에는 필요하지만 AGE 내부 계획에는 불필요.

## 핵심 쟁점 정리

### Property Index만으로 해결 가능한 문제

- id/name/country/industry 같은 vertex lookup.
- edge weight/date/status range filtering.
- label/type + property equality/range.
- selective endpoint에서 traversal 시작.

### Property Index만으로 불가능한 문제

- multi-hop path explosion.
- triangle/fork/cycle overlap selectivity.
- high-degree vertex fan-out.
- path uniqueness와 all paths materialization.
- `a`와 `c` property가 동시에 만족되는 연결성 추정.

### Path Index가 VLE를 가속하는 방식

- 반복 endpoint lookup을 CSR/CSC sequential traversal로 바꾼다.
- bounded path summary로 depth 1~5 후보를 미리 줄인다.
- terminal property posting/bitmap과 frontier를 intersect한다.
- path object는 parent-pointer DAG로 늦게 materialize한다.

### Pattern Index가 Triangle Query를 최적화하는 방식

- `b` 중심 fork를 `b -> in_sources_by_country`처럼 저장한다.
- KR source list와 US source list를 prefix 아래에서 바로 꺼낸다.
- `RETURN count(*)`류는 `|KR_list| * |US_list|`만 계산하고 tuple product를 만들지 않는다.

### WCOJ + Trie Index 조합 효과

- relation pair join 대신 variable prefix별 sorted intersection을 한다.
- triangle/cycle에서 AGM bound에 가까운 실행이 가능하다.
- trie permutation이 부족하면 seek가 늘어나므로 forward/reverse/type별 index가 필요하다.

### Graph Statistics와 Cardinality Estimation 관계

- index는 access path이고 statistics는 access path 선택 근거다.
- degree histogram과 top-k heavy vertex가 없으면 adjacency index가 있어도 fan-out cost를
  잘못 본다.
- pattern/path stats가 없으면 VLE와 triangle cardinality는 PostgreSQL 기본 estimator로는
  거의 추정 불가능하다.

### Hypergraph 접근과 Index 구조 관계

- hypergraph optimizer는 query predicate 구조를 보여준다.
- index advisor는 hyperedge별로 어떤 composite/pattern index가 필요한지 결정한다.
- WCOJ는 hypergraph의 fractional edge cover/variable order와 trie permutation 선택으로
  연결된다.

### AGE에서 새 Index Access Method 구현 방안

가능한 방식:

1. 기존 `age_adjacency` AM 확장.
   - forward/reverse endpoint payload.
   - sorted posting compression.
   - property filter pushdown metadata.
   - planner cost 함수 개선.
2. 별도 `age_trie` AM.
   - `graphid` tuple permutation을 key로 저장.
   - prefix cursor API를 SQL-visible function 또는 custom executor에 제공.
   - 일반 `IndexScan`보다 `AgeWCOJScan`에서 직접 사용하는 방향.
3. side-table materialized index.
   - path/pattern index는 PostgreSQL index AM보다 heap table + B-tree/GIN/BRIN 조합이
     운영상 단순하다.
4. compressed snapshot + delta.
   - read-mostly CSR/trie snapshot을 만들고, update는 delta adjacency index로 받아 merge한다.

필요한 PostgreSQL 확장 포인트:

- `CREATE ACCESS METHOD`, `IndexAmRoutine`.
- operator class for `graphid`.
- planner hook 또는 AGE Cypher planner path creation.
- custom scan/custom executor node.
- ANALYZE hook 또는 AGE-specific analyze command.
- background worker for index build/merge는 선택 사항.

Core 수정 필요 가능성:

- PostgreSQL가 graph pattern/WCOJ/path traversal을 일반 SQL plan node로 비용화하려면 core
  수정이 필요하다.
- AGE 내부 Cypher-only 최적화로 제한하면 extension 수준에서 대부분 가능하다.

## 종합 평가

점수는 1 낮음, 5 높음이다. 난이도/복잡도/비용은 5가 어렵거나 비싸다는 뜻이다.

| 기법 | PostgreSQL 적용 난이도 | AGE 적용 난이도 | 예상 성능 향상 | 연구 성숙도 | 제품 사례 | 구현 복잡도 | 유지보수 비용 | 오픈소스 참고 | 우선순위 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Property expression/B-tree index | 1 | 1 | 3 | 5 | 5 | 1 | 1 | 5 | 1 |
| Edge endpoint adjacency index | 3 | 2 | 4 | 4 | 5 | 3 | 3 | 3 | 2 |
| Graph statistics catalog | 3 | 2 | 4 | 4 | 4 | 3 | 3 | 2 | 3 |
| Edge endpoint+property composite index | 3 | 3 | 4 | 4 | 4 | 3 | 3 | 3 | 4 |
| CSR/CSC path index | 4 | 3 | 5 | 4 | 3 | 4 | 4 | 4 | 5 |
| WCOJ trie index | 5 | 4 | 5 | 4 | 3 | 5 | 5 | 4 | 6 |
| Materialized pattern index | 4 | 3 | 5 | 3 | 2 | 4 | 5 | 2 | 7 |
| Compressed graph index | 5 | 4 | 4 | 4 | 3 | 5 | 4 | 4 | 8 |
| Adaptive main/delta graph index | 5 | 4 | 4 | 3 | 3 | 5 | 5 | 2 | 9 |
| Learned index/learned CE | 4 | 4 | 3 | 3 | 2 | 4 | 5 | 3 | 10 |
| Full hypergraph index advisor | 5 | 4 | 3 | 3 | 2 | 5 | 5 | 2 | 11 |

## AGE 실행 로드맵

### 1단계: Property + adjacency index를 planner-visible하게 만들기

- `create_property_index`가 만든 index를 Cypher qual rewrite가 안정적으로 사용하게 한다.
- forward/reverse `age_adjacency` index를 label relation 생성 시 선택적으로 만든다.
- `EXPLAIN`에 property index selectivity와 adjacency index cost를 표시한다.

### 2단계: Graph statistics catalog

- label/property/edge/degree/path sample catalog를 추가한다.
- `ANALYZE GRAPH` 또는 `ag_catalog.analyze_graph(graph_name)`를 제공한다.
- VLE와 fixed pattern reorder에서 stats를 사용한다.

### 3단계: VLE path index

- VLE executor에 CSR-like snapshot을 연결한다.
- terminal property bitmap/posting set과 frontier intersection을 지원한다.
- `RETURN p`가 없는 count/existence mode를 path materialization 없이 처리한다.

### 4단계: WCOJ/trie prototype

- triangle/fork pattern에 한정해 `AgeWCOJScan`을 만든다.
- adjacency AM에서 sorted cursor를 제공한다.
- variable order는 graph stats 기반으로 고른다.

### 5단계: Materialized pattern/path advisor

- workload log에서 반복 pattern을 추출한다.
- materialized pattern index 후보와 maintenance cost를 산출한다.
- 사용자가 opt-in으로 생성하게 한다.

## 참고 출처

- PostgreSQL 18 Index AM API: https://www.postgresql.org/docs/18/index-api.html
- PostgreSQL 18 Built-in Index AM: https://www.postgresql.org/docs/18/indextypes.html
- PostgreSQL GIN: https://www.postgresql.org/docs/current/gin.html
- Neo4j index configuration: https://neo4j.com/docs/operations-manual/current/performance/index-configuration/
- Neo4j Cypher index syntax: https://www.neo4j.com/docs/cypher-manual/25/indexes/syntax/
- Memgraph skip-list index: https://memgraph.com/blog/memgraph-skip-lists-indexes-unique-constraints
- Memgraph index types via GQLAlchemy docs: https://memgraph.github.io/gqlalchemy/how-to-guides/ogm/
- JanusGraph indexing: https://docs.janusgraph.org/v0.6/schema/index-management/index-performance/
- Amazon Neptune statement indexes: https://docs.aws.amazon.com/neptune/latest/userguide/feature-overview-storage-indexing.html
- DuckPGQ CIDR 2023: https://vldb.org/cidrdb/2023/duckpgq-efficient-property-graph-queries-in-an-analytical-rdbms.html
- DuckPGQ VLDB demo PDF: https://szarnyasg.org/papers/duckpgq-vldb2023-demo.pdf
- MV4PG: https://arxiv.org/abs/2411.18847
- New Compressed Indices for Multijoins on Graph Databases: https://arxiv.org/abs/2408.00558
- qEndpoint: https://journals.sagepub.com/doi/10.3233/SW-243616
- RDFox tuple tables: https://docs.oxfordsemantic.tech/tuple-tables.html
- Blazegraph `SPORelation`: https://blazegraph.com/database/apidocs/com/bigdata/rdf/spo/SPORelation.html
- Virtuoso RDF index tuning: https://vos.openlinksw.com/owiki/wiki/VOS/VirtRDFPerformanceTuning
