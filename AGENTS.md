# Agent Notes

## 문서 작성 언어

- Workspace note 문서는 기본적으로 한글로 작성한다. 사용자가 다른 언어를
  명시적으로 요청하거나 외부 원문을 그대로 옮기는 경우는 예외다.
- 이 규칙은 `AGENTS.md`, `FEATURES.md`, `HISTORY.md`, `RESEARCH.md`,
  `TODO.md`, `VLE.md`에 적용한다.
- 설명 문장을 한글로 옮기더라도 code identifier, file path, command name,
  SQL/C function name, benchmark label, 논문 제목은 원래 표기를 유지한다.

## 문서 역할

- `AGENTS.md`: 작업 규칙, commit/push/test 절차, 환경 특이사항.
- `TODO.md`: 남은 작업과 바로 다음 실행 계획. 완료 로그를 길게 남기지 않는다.
- `HISTORY.md`: 완료된 변경, 검증 결과, 되돌린 시도 요약.
- `RESEARCH.md`: PostgreSQL source, 논문, graph DB/project 조사와 설계 판단.
- `VLE.md`: VLE 실행 구조, cache layout, traversal state, benchmark와 regression
  coverage.
- `FEATURES.md`: 사용자 관점의 기능/방향과 장기 구조 제안.

## TODO 진행 기록

- `TODO.md`의 작업을 진행할 때는 코드 변경만 남기지 말고, 새로 확인한 연구
  근거와 설계 판단을 함께 기록한다.
- 논문, PostgreSQL source, 다른 graph database/project를 참고해 TODO 항목의
  방향을 결정하거나 바꾸면 `RESEARCH.md`에 근거, 비교 대상, 결론을 추가한다.
- VLE 실행 구조, cache layout, adjacency representation, traversal state,
  benchmark 결과, regression coverage와 직접 관련된 내용은 `VLE.md`에도 반영한다.
- `TODO.md`는 남은 작업과 다음 턴 계획 중심으로 유지한다. 상세한 배경 설명이나
  결정 이유가 길어지면 `RESEARCH.md` 또는 `VLE.md`로 옮긴 뒤 `TODO.md`에서는
  짧게 참조한다.

## Commit Message Rules

- Subject는 짧고 명령형으로 쓴다. 예: `Fix cache invalidation`.
- Subject 끝에 마침표를 붙이지 않는다.
- 설명이 필요하면 subject 뒤에 빈 줄을 하나 두고 body를 쓴다.
- Body는 무엇을 왜 바꿨는지, side effect와 tradeoff를 적는다.
- 중간중간 commit은 하되, 너무 작은 변경을 여러 commit으로 쪼개지 않는다. 같은
  최적화 주제, 같은 regression/focused 검증 범위, 같은 설계 판단에 속한 코드/SQL/문서
  변경은 의미 있는 하나의 commit으로 묶는다.
- 단일 assertion 추가, 단일 guard 추가, 단일 helper 호출 변경처럼 독립 의미가 약한
  변경은 바로 commit하지 말고 같은 materialization boundary, descriptor 전환,
  planner/executor contract 변경 묶음이 완성될 때 함께 commit한다.
- 진행 중인 큰 구조 변경은 helper 분리나 caller 정리 하나가 끝났다는 이유만으로
  커밋하지 않는다. descriptor/API 이동, caller 정리, 문서 근거, focused 검증이 같은
  주제 안에 있으면 그 단위가 완성된 뒤 하나의 commit으로 묶는다.
- 코드 정리는 같은 분류를 최대한 묶어 대분류 단위로 진행한다. 단일 파일 정돈, 단일
  helper rename, 호출부 일부 정리처럼 작은 청소를 따로 끊지 말고, 기존보다 훨씬 큰
  범위(대략 10배 큰 묶음)를 목표로 module boundary, descriptor family, source/cache
  lifecycle, projection/index handoff 같은 같은 계열 전체를 한 작업 단위로 정리한다.
  작은 정리 10개 정도를 모아 하나의 대분류 정리로 만드는 것을 기본 기준으로 삼는다.
- Workspace note 문서(`AGENTS.md`, `FEATURES.md`, `HISTORY.md`,
  `RESEARCH.md`, `TODO.md`, `VLE.md`)는 사용자가 명시적으로 요청하지 않는 한
  커밋하지 않는다.

## Push 실행 방식

- 사용자가 push를 요청하면 `wsl.exe`를 통해 실행하지 말고 현재 shell에서
  `git push`를 직접 실행한다.
- 히스토리 재작성 뒤 push가 필요하면 기본은 `--force-with-lease`다.

## Testing Apache AGE in this workspace

macOS workspace 기준:

```sh
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config install
git diff --check
```

경고를 오류로 확인할 때:

```sh
make clean -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config
make -j16 PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config COPT=-Werror
```

focused installcheck 예시:

```sh
make PG_CONFIG=/Users/emotionbug/IdeaProjects/postgres_proj/pg18bin/bin/pg_config installcheck \
  REGRESS='expr cypher_match cypher_create cypher_set cypher_delete cypher_merge cypher_vle age_global_graph age_adjacency security'
```

`age_upgrade`는 regression 목록에 남아 있지만 이 최적화 브랜치에서는 하위 호환성
catalog parity를 검증하지 않는다. fresh install 전용 정책을 확인하는 smoke test로
유지한다.

WSL에서 테스트가 필요한 경우 repo는 `/home/redfi/postgresql-age`, PostgreSQL 18
install은 `/home/redfi/postgresql/pg18bin`이다. regression temp instance는 Windows
mount가 아니라 `/tmp` 아래에 둔다.

```sh
wsl.exe --cd /home/redfi/postgresql-age sh -lc \
  'make -s PG_CONFIG=/home/redfi/postgresql/pg18bin/bin/pg_config'

wsl.exe --cd /home/redfi/postgresql-age sh -lc \
  'make -s PG_CONFIG=/home/redfi/postgresql/pg18bin/bin/pg_config install'

wsl.exe --cd /home/redfi/postgresql-age bash -lc \
  "rm -rf /tmp/age_regress_instance /tmp/age_regress_out; \
   mkdir -p /tmp/age_regress_out; \
   /home/redfi/postgresql/pg18bin/lib/pgxs/src/makefiles/../../src/test/regress/pg_regress \
     --bindir=/home/redfi/postgresql/pg18bin/bin \
     --load-extension=age \
     --inputdir=/home/redfi/postgresql-age/regress \
     --outputdir=/tmp/age_regress_out \
     --temp-instance=/tmp/age_regress_instance \
     --port=61958 \
     --encoding=UTF-8 \
     --temp-config /home/redfi/postgresql-age/regress/age_regression.conf \
     --dbname=contrib_regression \
     cypher_vle age_global_graph cypher_match security"
```

## Regression 운영 원칙

- argument 없는 전체 `make installcheck`는 기본으로 실행하지 않는다.
- 변경 범위에 맞는 focused `installcheck REGRESS='...'`만 사용한다.
- 큰 실험 주제가 끝나면 build, install, focused installcheck, `git diff --check`를
  한 번 실행한다.
- 같은 큰 실험 주제 안에서는 여러 commit을 만들어도 focused installcheck를 매번
  반복하지 않는다.
- regression 실패 시 먼저 `regress/regression.diffs` 또는 `/tmp/.../regression.diffs`
  와 `log/`를 확인한다.

## 현재 브랜치 정책

- 이 브랜치는 성능 최적화 실험 브랜치다.
- extension upgrade 하위 호환성, 예전 catalog/data structure/layout 보존은 gate가
  아니다.
- fresh install 기준 동작, focused regression, benchmark 근거를 우선한다.
- 구조 병목이 반복되면 작은 guard 추가보다 자료구조, metadata, auxiliary index/cache,
  load contract, traversal state layout, planner/executor boundary 변경을 우선 검토한다.
