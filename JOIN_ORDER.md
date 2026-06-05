# Graph Pattern Matching 및 Cypher Query Optimization 조사

작성일: 2026-06-05

대상 범위는 2023~2026년에 확인 가능한 Graph Pattern Matching, Cypher/PGQ
query optimization, PostgreSQL join-order 연구와 구현이다. 결론부터 말하면
Apache AGE에 가장 현실적인 순서는 다음과 같다.

1. AGE graph catalog와 adjacency cache에 기반한 graph cardinality estimation.
2. fixed-length pattern의 join-order/expansion-order 재배치.
3. bounded VLE(`*1..5`) 전용 adaptive source 선택과 frontier pruning.
4. cycle, diamond, high fan-out 패턴에 한정한 WCOJ/Generic Join style operator.
5. 장기적으로 factorized intermediate representation 또는 vectorized VLE executor.

Hypergraph/DPhyp는 PostgreSQL planner 자체의 join-order 탐색 개선에는 유용하지만,
AGE Cypher가 이미 SQL 함수, SRF, custom path/executor로 내려간 뒤에는 그래프
패턴의 구조가 PostgreSQL optimizer에 충분히 보이지 않는다. 따라서 AGE에는
PostgreSQL join_search_hook만 붙이는 방식보다 Cypher transform/planner 단계에서
graph pattern IR을 유지하고 별도 graph-aware plan node를 만드는 편이 효과가 크다.

## 기준선: PostgreSQL Optimizer, GEQO, DPccp, DPhyp

PostgreSQL 기본 optimizer는 base relation별 scan path를 만들고, 둘 이상의 relation은
binary join tree로 조립한다. 지원 join node는 nested loop, merge join, hash join이고,
여러 relation은 최종 결과가 binary join step들의 tree로 만들어진다. relation 수가
`geqo_threshold` 미만이면 near-exhaustive search를 수행하고, 초과하면 GEQO로
heuristic search를 사용한다. 공식 문서도 많은 join에서 모든 실행 방식을 보는 것이
과도해지므로 GEQO를 사용한다고 설명한다.

GEQO는 join order를 integer string으로 표현하고 genetic algorithm으로 탐색한다.
장점은 planning time 상한을 낮추는 것이고, 단점은 최적성이 보장되지 않으며 graph
pattern의 cycle/degree/skew 구조를 직접 이해하지 못한다는 점이다. AGE에서 label
table, edge table, property filter가 여러 self-join으로 펼쳐지면 GEQO는 relation 수만
보고 탐색을 줄일 수 있지만, high-degree vertex fan-out이나 bounded path의 frontier
폭발은 보지 못한다.

DPccp는 query graph의 connected subgraph와 complement pair를 열거해 cross product를
피하면서 join tree를 찾는 dynamic programming 방식이다. 일반 binary join optimizer의
search-space pruning에는 좋지만 predicate가 여러 relation 집합을 동시에 묶는 경우에는
한계가 있다.

DPhyp는 query graph를 hypergraph로 모델링하고 connected hypergraph subset을 열거한다.
`pg_dphyp`는 PostgreSQL extension으로 공개되어 있으며, PostgreSQL의 DPsize와 GEQO
사이에 hypergraph 기반 join enumeration을 넣는다. README에 따르면 DPhyp는 relation
끼리 무작정 조합하기보다 table 간 link를 이용해 search space를 줄이고, PostgreSQL
hook과 `RestrictInfo`, equivalence class, outer join clause에서 hyperedge를 만든다.

AGE 관점의 핵심 차이는 다음과 같다.

- PostgreSQL/GEQO/DPccp/DPhyp: relation-level binary join order 문제를 푼다.
- WCOJ/Generic Join/Leapfrog Triejoin: join variable 또는 attribute 순서로 multiway
  intersection을 수행해 cyclic graph pattern의 중간 결과 폭발을 줄인다.
- Factorized query processing: 피할 수 없는 many-to-many intermediate를 평탄화하지
  않고 압축 표현으로 전달한다.
- Graph cardinality/adaptive/learned optimization: 그래프 degree, label, edge type,
  property correlation, runtime feedback을 이용해 expansion 방향과 operator 선택을
  고른다.

## Hypergraph 기반 Query Optimization

핵심 아이디어: query를 relation node와 predicate hyperedge로 표현하고, connected
sub-hypergraph만 열거해 불필요한 cartesian product와 predicate 누락 계획을 줄인다.
binary join node를 유지하더라도 탐색 공간을 query 구조에 맞게 제한할 수 있다.

대표 자료:

- Moerkotte, Neumann, "Dynamic Programming Strikes Back" 계열의 DPccp/DPhyp.
- `TantorLabs/pg_dphyp`: PostgreSQL용 DPhyp extension.
- MySQL 8.0 hypergraph optimizer 계열은 제품 적용 사례이나 AGE와 직접 통합 가능한
  오픈소스 PostgreSQL 코드는 아니다.

알고리즘 개요:

1. base relation 집합을 hypernode로 둔다.
2. binary predicate는 simple edge, multi-relation predicate는 hyperedge로 둔다.
3. connected subgraph/sub-hypergraph를 열거한다.
4. 각 connected subset과 neighbor subset을 결합해 `RelOptInfo` 또는 내부 plan state를
   확장한다.
5. 기존 cost model로 cheapest path를 보관한다.

복잡도:

- 최악의 join-order 문제는 여전히 NP-hard다.
- DPsize는 relation 수 `n`에 대해 대략 `O(3^n)` 계열 상태 전이가 가능하다.
- DPccp/DPhyp는 connected subgraph 수 `csg`와 connected complement pair 수 `ccp`에
  비례한다. sparse query graph에서는 크게 줄지만 clique나 dense hyperedge에서는
  여전히 커진다.
- 공간은 열거한 subset별 best path 저장으로 `O(2^n)` 상한을 가진다.

Binary Join 방식과의 차이:

- 실행 방식은 binary join일 수 있지만 탐색 단위가 단순 relation subset이 아니라
  graph/hypergraph connectivity다.
- cyclic pattern 자체의 중간 결과 폭발을 제거하지는 못한다. triangle을 `(a,b)`와
  `(b,c)`로 먼저 크게 만든 뒤 `(c,a)`를 검사하는 plan은 여전히 생긴다.

장점:

- PostgreSQL planner 구조와 상대적으로 잘 맞는다.
- AGE fixed-length pattern이 SQL join으로 충분히 노출되는 경우 즉시 이익을 볼 수 있다.
- cross join 억제와 complex predicate 표현에 강하다.

단점:

- graph-specific degree distribution과 adjacency locality는 별도 통계 없이는 반영이
  약하다.
- VLE/recursive path는 일반 join graph가 아니라 stateful traversal이라 적용 범위가
  제한된다.

제품/오픈소스:

- MySQL hypergraph optimizer, SQL Server/Cascades 계열은 제품 사례.
- PostgreSQL용 참고 구현은 `pg_dphyp`.

PostgreSQL 적용 가능성:

- 높음. `join_search_hook` 또는 core planner patch로 가능하다.
- 난관은 `RelOptInfo`, `SpecialJoinInfo`, equivalence class, lateral/outer join,
  parameterized path와의 정합성이다.

Apache AGE 적용 가능성:

- 중간. AGE가 Cypher pattern을 SQL relation join으로 충분히 노출하면 이익이 있다.
- 하지만 AGE의 adjacency match, VLE, path uniqueness, graphid/label semantics가
  custom executor 쪽에 숨어 있으면 PostgreSQL DPhyp만으로는 효과가 작다.

예상 난관:

- Cypher parser/transform 후에도 pattern graph IR을 보존해야 한다.
- label table self-join과 edge direction predicate를 hyperedge로 복원해야 한다.
- cost model이 `ag_label`, edge type, adjacency cache, property selectivity를 알아야
  한다.

## Worst-Case Optimal Join, Generic Join, Leapfrog Triejoin

핵심 아이디어: binary join tree 대신 query variable/attribute 순서대로 multiway
intersection을 수행한다. cyclic join에서 중간 결과를 크게 만들지 않고 AGM bound에
맞는 worst-case output-size complexity를 달성한다.

대표 자료:

- Ngo, Porat, Re, Rudra의 NPRR, Veldhuizen의 Leapfrog Triejoin은 고전 기반.
- Kùzu CIDR 2023은 property graph DBMS에서 binary join과 WCOJ multiway join operator를
  함께 사용한다고 설명한다.
- GraphflowDB thesis(2023)는 graph pattern이 many-to-many join으로 큰 intermediate를
  만들며, WCOJ와 factorized representation이 필요하다고 정리한다.
- ADOPT(2023)는 WCOJ의 attribute order 선택을 runtime feedback과 RL로 adaptive하게
  최적화한다.
- GraphMatch(2024)는 FPGA 기반 WCOJ subgraph query accelerator 연구다.

알고리즘 개요:

- Generic Join:
  1. query 변수 순서 `x1, x2, ...`를 정한다.
  2. 현재 prefix와 관련된 모든 relation/index에서 가능한 다음 값 집합을 찾는다.
  3. sorted list/trie intersection으로 공통 값을 구한다.
  4. prefix를 확장하며 재귀적으로 반복한다.
- Leapfrog Triejoin:
  1. 각 relation을 attribute order에 맞춘 trie 또는 sorted index로 본다.
  2. 같은 depth의 iterator들을 leapfrog seek로 맞춘다.
  3. 공통 key가 있으면 다음 depth로 내려간다.

복잡도:

- cyclic conjunctive query에 대해 `O(AGM_bound + input/output overhead)`가 핵심
  보장이다.
- triangle count/query는 binary join이 dense graph에서 `O(|E|^2)` intermediate를 만들
  수 있지만 WCOJ는 `O(|E|^{3/2})` bound가 가능하다.
- 공간은 trie/index 또는 sorted adjacency representation에 좌우된다. 기존 B-tree만
  쓰면 random seek 비용이 커지고, CSR/adjacency list/trie가 있으면 prefix state
  중심으로 낮아진다.

Binary Join 방식과의 차이:

- binary join은 relation pair를 먼저 결합해 intermediate tuple relation을 만든다.
- WCOJ는 변수 하나씩 binding하고 관련 edge constraint를 동시에 검사한다.
- join order가 relation order가 아니라 attribute/variable order다.

장점:

- triangle, cycle, clique, diamond 같은 cyclic pattern에서 중간 결과 폭발을 근본적으로
  줄인다.
- graph adjacency intersection과 잘 맞는다.
- high-degree vertex가 있는 social graph에서 binary join보다 안정적이다.

단점:

- acyclic chain query에서는 좋은 index nested loop와 차이가 작거나 느릴 수 있다.
- attribute order와 index layout이 성능을 좌우한다.
- PostgreSQL executor의 tuple-at-a-time, binary join node model과 구조가 다르다.

제품/오픈소스:

- LogicBlox/LogicBlox 계열, EmptyHeaded, Souffle/Datalog 계열이 WCOJ 또는 유사
  multiway join을 사용한다.
- Kùzu는 open-source property graph DBMS로 WCOJ와 factorized query processor를 핵심
  설계로 공개했다. 단, 확인 시점의 GitHub 저장소는 2025-10-10에 archive 상태였다.
- GraphflowDB는 연구 구현체로 WCOJ/binary hybrid optimizer와 factorized vector
  execution을 구현했다.

PostgreSQL 적용 가능성:

- core planner에 일반 WCOJ path node를 넣는 것은 어려움이 높다.
- extension 수준에서는 특정 SQL 함수/SRF/custom scan으로 제한된 pattern operator를
  만들 수 있다.
- 필요한 index API는 `(src -> sorted dst)`, `(dst -> sorted src)`, label/type별
  adjacency list, property-filtered candidate source다.

Apache AGE 적용 가능성:

- 높음. AGE는 그래프 pattern 구조를 Cypher transform 시점에 알고 있으므로, fixed-length
  cyclic/branching pattern을 `AgeWCOJMatch` 같은 custom plan으로 내릴 수 있다.
- 우선순위는 chain보다 `(a)-[]->(b)<-[]-(c)`의 공통 `b` fan-in intersection, triangle,
  4-cycle, multi-edge pattern이다.

예상 난관:

- path uniqueness, relationship identity, repeated variable semantics를 prefix binding
  단계에서 정확히 처리해야 한다.
- PostgreSQL snapshot/MVCC와 AGE adjacency cache 일관성 보장이 필요하다.
- `EXPLAIN`, costing, parameterized path, parallel safety, memory context 수명 관리가
  모두 새 operator에 들어간다.

## Factorized Query Processing

핵심 아이디어: many-to-many join 결과를 평탄한 tuple stream으로 모두 펼치지 않고,
공통 prefix와 독립적인 multi-valued dependency를 nested/vector block 형태로 유지한다.

대표 자료:

- Kùzu CIDR 2023: factorized query processor, binary/WCOJ operator 조합.
- DuckPGQ CIDR 2023: analytical RDBMS 안에서 SQL/PGQ를 구현하며 joins와 factorized
  query processing 기법을 활용.
- GraphflowDB thesis(2023): factorized vector execution, f-representation,
  d-representation, nested hash table reuse.

알고리즘 개요:

1. query plan의 intermediate를 flat tuple array가 아니라 factorized table로 저장한다.
2. 공통 binding(`a`) 아래에 여러 `b`, `c` list를 따로 둔다.
3. 다음 operator가 필요한 column만 vector block으로 읽고, 최종 projection/aggregation
   시점까지 cartesian product materialization을 늦춘다.
4. d-representation은 반복 subrelation을 DAG/nested hash table로 재사용한다.

복잡도:

- flat output 자체가 `|A| * avg_deg1 * avg_deg2`이면 최종 반환에는 그 비용이 필요하다.
- 중간 처리와 aggregation/filter가 factorized form 위에서 가능하면 공간은
  `O(|A| + |E1| + |E2|)`에 가까워질 수 있다.
- worst-case output이 필요한 query에는 출력 비용 하한이 남는다.

Binary Join 방식과의 차이:

- binary join은 매 단계 tuple relation을 materialize한다.
- factorized processing은 join 결과의 종속 구조를 유지한다.

장점:

- path expansion, star/branch pattern, aggregation에서 메모리와 CPU를 크게 줄인다.
- PostgreSQL의 vectorized executor 부재를 AGE 내부 VLE/materializer 단위에서 보완할
  수 있다.

단점:

- 일반 PostgreSQL plan node와 tuple slot 인터페이스에 맞추려면 결국 flatten 경계가
  필요하다.
- operator 전반을 factorized-aware로 바꾸지 않으면 이익이 제한된다.

제품/오픈소스:

- Kùzu, GraphflowDB가 대표 참고 구현.
- DuckPGQ는 DuckDB 기반 PGQ 구현과 CSR/path operator를 공개 논문으로 설명한다.

PostgreSQL 적용 가능성:

- 낮음~중간. core executor는 tuple slot 중심이라 factorized intermediate를 일반화하기
  어렵다.
- custom scan/custom executor 내부에서만 factorized representation을 유지하고 boundary
  에서 tuple로 풀어내는 방식은 가능하다.

Apache AGE 적용 가능성:

- 높음. AGE VLE materialization, adjacency cache, traversal state에서 이미 graph-specific
  layout을 다루므로 bounded path와 branch pattern에 국소 적용할 수 있다.

예상 난관:

- `RETURN path`, `relationships(p)`, `nodes(p)`처럼 path object를 요구하는 projection은
  factorized state를 언제 materialize할지 정해야 한다.
- memory context 폭증과 detoasting/agtype 생성 지연 전략이 필요하다.

## Graph Cardinality Estimation

핵심 아이디어: relation histogram만으로는 label, edge type, degree distribution,
property correlation, path selectivity를 추정하기 어렵다. graph-specific synopsis,
sampling, learned model을 통해 pattern cardinality와 expansion fan-out을 추정한다.

대표 자료:

- "A General Cardinality Estimation Framework for Subgraph Matching in Property Graphs"
  (2023).
- GNCE, "Cardinality Estimation over Knowledge Graphs with Embeddings and Graph Neural
  Networks" (2023, revised 2024): KG embedding과 GNN으로 conjunctive query cardinality를
  예측한다.
- HomeRun(2024): graph database cardinality estimation advisor.
- "Using query semantic and feature transfer fusion to enhance cardinality estimating of
  property graph queries" (2024): property graph query를 vector로 featurize하고 semantic
  feature fusion으로 Q-error/RMSE를 낮춘다.
- "Cardinality Estimation on Hyper-relational Knowledge Graphs" (2024, revised 2025):
  qualifier-aware GNN으로 hyper-relational KG query CE를 수행한다.

알고리즘 개요:

- 통계형: label별 vertex count, edge type별 count, `(label,type,label)` count,
  in/out-degree histogram, top-k heavy vertex, property MCV/histogram, path-length sample.
- sampling형: query graph의 partial binding을 random walk/edge sample로 추정한다.
- learned형: query pattern을 graph/sequence/vector로 encode하고 GNN/LSTM/MLP로 cardinality
  또는 q-error objective를 학습한다.

복잡도:

- 통계 lookup은 `O(pattern_edges)` 수준이고 저장 공간은 label/type/property 조합 수에
  비례한다.
- sampling은 sample 수 `s`와 pattern size `k`에 대해 `O(s*k)`이며 tail/skew variance가
  문제다.
- learned model inference는 모델 크기와 query graph 크기에 비례한다. 학습 비용과
  workload drift 관리 비용이 크다.

Binary Join 방식과의 차이:

- 기존 PostgreSQL CE는 relation column 통계 중심이고 multi-hop degree correlation을
  잘 보지 못한다.
- graph CE는 expansion fan-out과 repeated variable/cycle selectivity를 직접 추정한다.

장점:

- 모든 optimizer 기법의 기반이다. 잘못된 cardinality는 WCOJ를 써야 할 곳과 binary join을
  써야 할 곳을 뒤바꾼다.
- AGE에는 가장 먼저 도입할 가치가 있다.

단점:

- graph update가 많으면 stale statistic 문제가 크다.
- property graph는 schema가 약해 조합 폭발이 생긴다.
- learned CE는 운영 안정성과 explainability가 낮다.

제품/오픈소스:

- Neo4j는 Cypher planner가 cost 기반 plan search를 하고 database statistics 변경에 따라
  replanning한다.
- TigerGraph 4.2 문서는 multi-hop path pattern의 declarative traversal plan을 optimizer가
  정하며, cost-based optimization은 pre-computed data distribution statistics를 사용한다고
  설명한다.
- Graph CE 연구 구현체는 논문별로 다양하지만 PostgreSQL/AGE에 바로 붙일 수 있는 표준
  구현은 없다.

PostgreSQL 적용 가능성:

- core 적용은 statistics kind 확장, `pg_statistic_ext`, selectivity estimator hook이 필요하다.
- extension으로는 AGE catalog table에 graph statistics를 저장하고 Cypher planner 내부
  cost로 사용하는 방식이 현실적이다.

Apache AGE 적용 가능성:

- 매우 높음. `ag_label`, edge label relation, adjacency cache build 단계에서 통계를
  수집할 수 있다.
- 우선 수집 항목: label count, edge label count, direction별 degree histogram, top-k
  heavy graphid, `(src_label, edge_label, dst_label)` count, property predicate selectivity,
  VLE depth별 frontier sample.

예상 난관:

- PostgreSQL ANALYZE와 AGE graph metadata 갱신 시점을 어떻게 맞출지 정해야 한다.
- graphid는 label id와 local id를 포함하므로 label partition 통계를 relation 통계와
  연결해야 한다.
- VLE cardinality는 path uniqueness와 max depth에 따라 단순 degree power보다 작거나
  클 수 있다.

## Adaptive Graph Query Optimization

핵심 아이디어: compile/planning 시점 추정이 틀릴 수 있으므로 runtime feedback으로
source vertex, expansion direction, attribute order, compiled/interpreted execution mode를
바꾼다.

대표 자료:

- "Adaptive query compilation in graph databases" (2023): Poseidon에서 graph algebra
  expression을 machine code로 만들고, interpreter로 시작한 뒤 compilation 완료 후 compiled
  code로 전환하며 code cache를 사용한다.
- ADOPT(2023): WCOJ attribute order를 episode 단위로 바꾸고 RL로 탐색/활용 균형을 잡는다.
- Linked Data Eddies 계열과 adaptive SPARQL processing은 graph/RDF query의 runtime
  reorder 사례다.

알고리즘 개요:

1. 초기 plan은 통계 기반 또는 heuristic으로 시작한다.
2. 일정 tuple/frontier/episode 단위로 실제 fan-out, selectivity, elapsed time을 측정한다.
3. 다음 source, next variable, join/expand direction, compiled path를 바꾼다.
4. 이미 처리한 prefix를 memo/cache해 중복 work를 줄인다.

복잡도:

- 최악 실행 복잡도는 선택한 물리 알고리즘에 따른다.
- adaptive overhead는 feedback sampling과 plan switch 비용이다.
- 잘 설계하면 skew query에서 잘못된 초기 plan 비용을 early stop/early switch로 제한한다.

Binary Join 방식과의 차이:

- binary plan은 executor 시작 후 join tree가 고정된다.
- adaptive graph plan은 traversal 중 direction/order/source를 바꿀 수 있다.

장점:

- high-degree vertex, skewed edge label, stale statistics에 강하다.
- bounded VLE에서 frontier 폭발을 조기에 감지할 수 있다.

단점:

- deterministic plan과 EXPLAIN 재현성이 낮아진다.
- PostgreSQL executor는 plan tree runtime mutation에 보수적이다.

제품/오픈소스:

- Neo4j는 query plan cache와 replanning, compiled operator/expressions 옵션을 제공한다.
- Poseidon은 연구/프로토타입 성격의 adaptive compilation 사례다.
- ADOPT는 연구 구현 성격이다.

PostgreSQL 적용 가능성:

- 낮음~중간. executor node 내부에서 local adaptive behavior는 가능하지만, 상위 plan tree를
  바꾸는 것은 어렵다.

Apache AGE 적용 가능성:

- 높음. VLE custom executor 내부에서 source candidate ordering, frontier batching,
  depth별 pruning은 AGE가 통제할 수 있다.

예상 난관:

- runtime switch가 Cypher path ordering/duplicate semantics를 깨지 않아야 한다.
- parallel execution 시 shared feedback과 worker-local state를 나눠야 한다.

## Learned Graph Query Optimization

핵심 아이디어: join/order/plan 선택 또는 cardinality/cost 추정을 ML/RL 모델로 대체하거나
보조한다. graph query에서는 query graph와 data graph의 embedding, GNN, RL policy가
주로 쓰인다.

대표 자료:

- "Reinforcement Learning-based SPARQL Join Ordering Optimizer" (ESWC 2023): RDF/SPARQL
  join ordering에 RL을 적용하고 graph DB/RDF store에서는 연구가 제한적이라고 지적한다.
- ADOPT(2023): WCOJ attribute order를 RL로 adaptive 선택.
- "Is Your Learned Query Optimizer Behaving As You Expect?" (VLDB 2024): learned optimizer가
  PostgreSQL classical optimizer를 체계적으로 항상 이기지는 못하며 평가 안정성이 중요하다고
  분석한다.
- Lero(2024): PostgreSQL native optimizer 위에서 learning-to-rank로 plan 선택을 개선한다.
- JoinGym(2023), GTDD(2024) 등 RL join-order 환경/모델.
- Property graph CE 논문들: GNCE, semantic feature transfer fusion, hyper-relational KG CE.

알고리즘 개요:

1. query graph, predicates, candidate plan/operator를 feature로 encode한다.
2. supervised model은 latency/cardinality/q-error label을 학습한다.
3. RL model은 plan construction step을 action으로 보고 reward를 latency/cost로 둔다.
4. production 적용 시 native optimizer 후보 plan을 재랭킹하는 conservative 방식이 안전하다.

복잡도:

- inference는 보통 planning time 안에 들어갈 수 있지만, plan 후보 생성과 feature extraction이
  커질 수 있다.
- 학습 비용, workload drift, retraining pipeline이 실질 비용이다.

Binary Join 방식과의 차이:

- binary join model 자체를 바꾸지 않고 plan choice만 개선할 수도 있다.
- WCOJ/adaptive와 결합하면 relation order가 아니라 variable/attribute order를 학습한다.

장점:

- AGE workload가 특정 graph schema와 반복 query shape에 집중되어 있으면 효과가 크다.
- rule/cost model로 잡기 어려운 property correlation과 skew를 학습할 수 있다.

단점:

- cold start, 데이터 drift, regression 설명 가능성, 보안/운영 복잡도가 크다.
- core PostgreSQL upstream에 넣기 어렵고 extension maintenance 부담이 크다.

제품/오픈소스:

- Lero는 PostgreSQL 위 learned re-optimizer 사례다.
- Bao/Neo/JoinGym류 연구 구현이 있으나 graph-Cypher 전용 production 오픈소스는 성숙도가
  낮다.

PostgreSQL 적용 가능성:

- 중간. `planner_hook`으로 후보 plan feature를 뽑아 re-rank하는 방식은 가능하다.
- core replacement optimizer는 난이도가 높다.

Apache AGE 적용 가능성:

- 중간. 먼저 graph statistics와 plan logging이 있어야 학습 데이터가 생긴다.
- 추천 단계는 CE 보정 또는 VLE source 선택 모델부터다.

예상 난관:

- Cypher query normalization, parameterization, plan id 안정화가 필요하다.
- model artifact versioning과 extension upgrade/test 체계가 필요하다.

## 예시 Cypher 패턴별 계획 분석

### `MATCH (a)-[]->(b)-[]->(c)`

기본 binary plan:

- `edge e1` scan 또는 `a` candidate scan 후 `e1.src = a.id`.
- `e1.dst = b.id`로 `b`를 얻고, `edge e2`를 `b.id = e2.src`로 nested/index expand.
- `c.id = e2.dst`.

가능한 개선:

- Graph CE: label/property filter가 있으면 가장 selective한 endpoint에서 시작한다. 예를 들어
  `c` filter가 강하면 reverse expand `c <- e2 <- b <- e1 <- a`.
- Hypergraph/DPhyp: SQL self-join 형태라면 chain connectivity를 이용해 cross join 없이
  cheaper left-deep/bushy order를 찾는다.
- WCOJ: 단순 acyclic chain이라 이익은 제한적이다. high fan-out `b`에서 `e1.dst`와
  `e2.src` candidate를 동시에 다룰 수 있지만 index nested loop와 큰 차이가 없을 수 있다.
- Factorized: `a -> [b] -> [c]`를 factorized path state로 유지하면 `RETURN count(*)`,
  `RETURN a, count(c)`류에서 flat path materialization을 늦출 수 있다.
- Adaptive: depth 1 frontier가 폭발하면 반대 방향 source로 전환하거나 degree-heavy `b`를
  pruning한다.

예상 효과:

- selective endpoint가 있으면 2~10배 이상 가능.
- 아무 filter 없는 전체 2-hop enumeration은 출력량 자체가 크므로 factorization/aggregation
  없이는 제한적이다.

### `MATCH (a)-[]->(b)<-[]-(c)`

기본 binary plan:

- `e1.dst = b.id`와 `e2.dst = b.id`를 각각 만들고 `b`에서 join하거나,
  `b` scan 후 incoming adjacency를 두 번 확장한다.
- high in-degree `b`가 있으면 `deg_in(b)^2` 결과가 생긴다.

가능한 개선:

- Graph CE: in-degree histogram/top-k heavy `b`를 이용해 heavy center를 늦게 처리하거나
  aggregation pushdown을 선택한다.
- WCOJ/Generic Join: variable order를 `b -> a -> c` 또는 selective endpoint 우선으로 잡고
  incoming adjacency list를 prefix 아래에서 처리한다. 추가 predicate `a.prop = c.prop` 또는
  `a = c`/triangle edge가 있으면 multiway intersection 이익이 커진다.
- Factorized: `b -> {a-list}, {c-list}`로 유지하면 `RETURN b, count(*)`는 `deg1*deg2`를
  flat tuple로 만들지 않고 계산할 수 있다.
- Adaptive: 실제 `b` fan-in을 보며 heavy center는 별도 batch/late materialization 처리한다.

예상 효과:

- `RETURN a,b,c`처럼 모든 pair를 반환하면 출력 하한이 있어 제한적이다.
- count/existence/filter 중심이면 factorized aggregation으로 큰 메모리 절감과 수배~수십 배
  개선 가능성이 있다.

### `MATCH (a)-[*1..5]->(b)`

기본 binary/recursive plan:

- recursive expansion 또는 VLE executor가 depth 1~5 frontier를 만든다.
- path uniqueness, relationship uniqueness, min/max depth, path projection 때문에 상태가
  커진다.

가능한 개선:

- Graph CE: depth별 평균/분위수 degree, top-k heavy source, label/type filter별 branching
  factor로 시작점과 direction을 고른다.
- Adaptive: depth별 frontier size를 측정하고, max depth 5 안에서 frontier cap, bidirectional
  expansion, selective terminal filter early check를 적용한다.
- Factorized: path prefix DAG, parent pointer, relationship id vector로 path를 늦게
  materialize한다.
- WCOJ: 순수 reachability/path enumeration에는 직접적이지 않다. 다만 bounded path를
  `e1,e2,...,e5` fixed joins로 unroll하고 endpoint/cycle predicate가 있는 경우 일부 depth를
  WCOJ로 평가할 수 있다.
- DuckPGQ식 접근: repeated path search가 많으면 CSR를 만들고 multi-source BFS를 batch/SIMD로
  처리한다. DuckPGQ는 SQL/PGQ path-finding에서 on-the-fly CSR와 MS-BFS/Bellman-Ford를
  사용한다고 설명한다.

예상 효과:

- selective `a` 또는 `b`가 있으면 direction 선택만으로 큰 개선 가능.
- 여러 source-destination pair reachability/shortest path는 CSR + multi-source BFS가
  PostgreSQL recursive CTE식 plan보다 훨씬 유리하다.
- 모든 path 반환은 path 수가 지수적으로 커질 수 있어 pruning과 materialization 지연이 핵심이다.

## 종합 평가

점수는 1 낮음, 5 높음이다. "난이도"는 5가 어렵다는 뜻이다.

| 기법 | PostgreSQL 적용 난이도 | AGE 적용 난이도 | 예상 성능 향상 | 연구 성숙도 | 오픈소스 참고 | 제품 적용 | 유지보수 복잡도 | AGE 우선순위 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Graph Cardinality Estimation | 3 | 2 | 4 | 4 | 2 | 4 | 3 | 1 |
| Fixed-pattern expansion reorder | 2 | 2 | 3 | 4 | 2 | 4 | 2 | 2 |
| Adaptive VLE/source selection | 4 | 3 | 4 | 3 | 2 | 3 | 4 | 3 |
| Factorized VLE/intermediate | 5 | 3 | 4 | 4 | 4 | 3 | 4 | 4 |
| WCOJ/Generic Join operator | 5 | 4 | 5 | 4 | 4 | 3 | 5 | 5 |
| Hypergraph/DPhyp join enumeration | 3 | 4 | 3 | 4 | 4 | 4 | 3 | 6 |
| Learned CE/re-ranking | 4 | 4 | 3 | 3 | 3 | 2 | 5 | 7 |
| Full learned optimizer/RL planner | 5 | 5 | 3 | 2 | 3 | 1 | 5 | 8 |

## Apache AGE 구현 제안

### 1단계: graph statistics와 cost model

- `ag_graph_stats`류 catalog 또는 side table을 fresh install 기준으로 추가한다.
- label별 vertex/edge count, direction별 degree histogram, top-k graphid, label-edge-label
  count를 수집한다.
- Cypher transform/planner가 `MATCH` pattern graph를 잃기 전에 endpoint selectivity와
  expansion fan-out을 계산한다.
- `EXPLAIN`에 graph cardinality estimate와 chosen expansion direction을 노출한다.

### 2단계: fixed-length pattern reorder

- chain, fork, cycle을 pattern graph로 표현한다.
- edge scan 중심이 아니라 candidate vertex source와 adjacency expansion 중심으로 plan을
  만든다.
- `(a)-[]->(b)<-[]-(c)`는 center `b` materialization을 늦추고 count/exists projection은
  factorized aggregate path를 사용한다.

### 3단계: VLE adaptive execution

- `*1..5`는 depth별 frontier statistic을 runtime에 수집한다.
- terminal filter가 있으면 reverse/bidirectional expansion 후보를 둔다.
- path object가 필요 없으면 reachability/count mode로 materialization을 줄인다.

### 4단계: WCOJ micro-operator

- 처음부터 PostgreSQL general WCOJ를 만들지 말고 AGE graph pattern 전용으로 제한한다.
- 입력은 sorted adjacency list 또는 adjacency cache descriptor다.
- 대상은 triangle, 4-cycle, shared-center fork, repeated variable pattern이다.
- variable order는 통계 기반으로 시작하고, 이후 ADOPT식 runtime feedback은 실험 옵션으로 둔다.

## 참고 출처

- PostgreSQL 18 Planner/Optimizer 문서: https://www.postgresql.org/docs/18/planner-optimizer.html
- PostgreSQL 18 GEQO 문서: https://www.postgresql.org/docs/18/geqo.html
- `pg_dphyp`: https://github.com/TantorLabs/pg_dphyp
- Kùzu CIDR 2023: https://vldb.org/cidrdb/2023/kuzu-graph-database-management-system.html
- Kùzu source archive: https://github.com/kuzudb/kuzu
- GraphflowDB thesis: https://uwspace.uwaterloo.ca/items/44eb25bd-b86a-4e0b-a4d5-cad28ad14f6d
- DuckPGQ CIDR 2023: https://vldb.org/cidrdb/papers/2023/p66-wolde.pdf
- Neo4j Cypher query tuning/planner options: https://www.neo4j.com/docs/cypher-manual/25/planning-and-tuning/query-tuning/
- TigerGraph Query Optimizer 4.2: https://docs.tigergraph.com/gsql-ref/4.2/querying/query-optimizer/
- TigerGraph Conjunctive Pattern Matching: https://docs.tigergraph.com/gsql-ref/4.1/tutorials/pattern-matching/adv/conjunctive-pattern-matching
- ADOPT: https://arxiv.org/abs/2307.16540
- Reinforcement Learning-based SPARQL Join Ordering Optimizer: https://ruben.verborgh.org/publications/eschauzier_eswc_2023/
- GNCE: https://arxiv.org/abs/2303.01140
- HomeRun: https://research.tue.nl/nl/publications/homerun-a-cardinality-estimation-advisor-for-graph-databases/
- Property graph query semantic feature transfer CE: https://www.sciencedirect.com/science/article/pii/S014193822400218X
- Hyper-relational KG CE: https://arxiv.org/abs/2405.15231
- Adaptive query compilation in graph databases: https://link.springer.com/article/10.1007/s10619-023-07430-4
