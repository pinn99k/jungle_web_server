# 스텝 8 읽기 가이드 — 종료 처리, 엣지 케이스, 기능 · 부하 테스트

페이즈 6 (단단하게) · 이슈 #8 · 2026-11-24

## 1. 추가 · 변경된 파일과 줄 수

| 파일 | 변경 |
|---|---|
| `server/main.c` | +78 −27: `on_signal` · `install_signals` · `start_pool` · `accept_loop`, 종료 순서 |
| `server/conn.c` · `.h` | 종료 플래그 `conn_stopping`, 종료 중 keep-alive 끔, 손님 연결에 TCP_NODELAY |
| `common/net.c` · `.h` | `net_accept`가 EINTR을 돌려줌 (Q1), `net_set_nodelay` 추가 |
| `tests/functional.sh` | 새로 만듦. 기능 테스트 25개 |

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `main` | 설정 → `install_signals` → 리스닝 → `start_pool` → `accept_loop` → close → `pool_shutdown` | — |
| `on_signal` (static) | `conn_stopping = 1`만 (원자적 쓰기) | 커널 (SIGINT · SIGTERM) |
| `install_signals` (static) | SIGPIPE 무시, SIGINT · SIGTERM에 `on_signal` (SA_RESTART 없음) | `main` |
| `start_pool` (static) | 시그널을 막은 채 `pool_create` → main에서만 풂 | `main` |
| `accept_loop` (static) | 플래그가 설 때까지 accept → `pool_submit`. EINTR이면 루프 조건으로 | `main` |
| `conn_dispatch` (변경) | 종료 중이면 `keep = 0` → 응답에 `Connection: close` | `conn_serve` |
| `net_set_nodelay` | TCP_NODELAY | `conn_new` |

## 3. 종료 한 번의 호출 순서

손님 하나가 `/api/slow`를 기다리는 중에 `docker stop`(SIGTERM)이 온 경우:

1. 커널이 SIGTERM을 보냄. 워커는 막아 두었으므로 main이 받음 (Q3)
2. `on_signal` → `conn_stopping = 1`
3. main의 `accept`가 EINTR로 깸 → `net_accept`가 -1 (재시도 안 함, Q1) → `accept_loop`가 루프 조건에서 플래그를 보고 빠져나옴
4. "종료 중" 로그 → 리스닝 fd close (새 손님은 연결 거절)
5. `pool_shutdown` → 큐의 대기 연결까지 워커가 처리. `/api/slow` 손님은 3초 뒤 504를 끝까지 받음 (Q2)
6. 처리 중인 keep-alive 연결은 이번 응답을 `Connection: close`로 받고 닫힘. 쉬는 연결은 최대 5초 뒤 닫힘
7. 워커가 모두 끝나면 join → "종료" → `return 0` → ASan이 누수 검사

흐름 지도의 경로 D가 실제로 이어졌습니다.

## 4. 추천 읽기 순서

1. `server/main.c` `main` 끝부분 — 종료 순서 네 줄
2. `on_signal` → `install_signals` — 핸들러는 왜 플래그만
3. `start_pool` — 시그널을 어느 스레드가 받나
4. `accept_loop` — EINTR이 "깨어남 신호"가 되는 곳
5. `conn.c` `conn_dispatch`의 `keep` 한 줄 — 종료 중 keep-alive 끄기
6. `tests/functional.sh` — 엣지 케이스 대응표가 어떻게 자동 확인되나

## 5. 직접 실행해 볼 명령

컨테이너 안 `/work`에서:
```
make && tests/functional.sh                      # 25개, 종료 코드 = 실패 수
./build/asan/server -c conf/server.conf          # 요청 몇 개 보낸 뒤 Ctrl+C → "종료", 누수 보고 없음
```

진행 중 요청 마무리 보기:
```
./build/backend 8001 & ./build/backend 8002 &
./build/server -c conf/server.conf &
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/api/slow &   # 3초 뒤 504
kill -TERM %3                                    # 서버는 504를 보낸 뒤 꺼짐
```

부하 측정:
```
./build/server -c conf/server.conf -t 8 &
wrk -t4 -c100 -d10s http://127.0.0.1:8080/index.html
taskset -c 0 ./build/server -c conf/server.conf -t 8 &    # 코어 1개 고정 (#8 퓨처 플랜)
```

## 측정 결과 (2026-11-24, Docker · CPU 2코어)

`wrk -t4 -c100 -d10s /index.html`

| 서버 스레드 | TCP_NODELAY 끔 (처음) | TCP_NODELAY 켬 | timeout (wrk 2초) |
|---|---|---|---|
| 4 | 초당 77, 평균 52ms | 초당 2,108, 평균 30ms | 170 |
| 8 | 초당 156, 평균 51ms | 초당 1,592, 평균 45ms | 108 |
| 16 | 초당 274, 평균 60ms | 초당 2,089, 평균 20ms | 199 |

코어 1개 고정 (`taskset -c 0`, wrk는 다른 코어, 5초)

| 서버 스레드 | 끔 | 켬 |
|---|---|---|
| 1 | 초당 18 | 초당 154 |
| 4 | 초당 76 | 초당 1,999 |
| 8 | 초당 144 | 초당 2,498 |
| 16 | 초당 289 | 초당 2,995 |

읽는 법:
- **끔일 때 스레드 수에 정비례**한 이유: 요청마다 약 50ms를 "기다리느라" 쓰고 CPU는 거의 안 씀 → 기다리는 동안 다른 스레드가 일함. 코어 1개여도 스레드가 많을수록 빨라짐 (페이즈 4 "코어 하나에서 멀티스레드가 의미 있나"의 실측)
- **원인은 Nagle + 지연 ACK**: 헤더와 본문을 따로 `write`하면, Nagle이 두 번째 조각을 첫 조각의 ACK까지 붙잡고, 손님은 ACK를 최대 약 40ms 모아 보냄 → keep-alive 요청마다 약 40~50ms 정지. 연결을 바로 닫는 ab(비 keep-alive)에서는 안 보였음
- **켠 뒤 timeout**: 연결 100개 > 워커 수라, 큐의 연결이 keep-alive 연결이 끝나기를 기다리다 wrk 2초 제한에 걸림 (페이즈 4 Q11 → R6)
- 페이즈 4에서 `ab -k -c 8`이 4.9초 걸린 것도 같은 원인 (지금은 0.41초). 그때 "끝에서 5초 대기"로 본 추정은 틀렸음

## 6. 확인 질문

워커 스레드에서 SIGINT · SIGTERM을 막지 않으면, Ctrl+C를 눌렀을 때 서버가 종료되지 않을 수 있습니다. 왜 그럴까요?

## 알려진 한계

- 플래그를 확인한 직후 `accept`에 들어가기 직전에 신호가 오면, 다음 연결이 올 때까지 깨지 않음 (정석 해법: self-pipe · `pselect`)
- 쉬는 keep-alive 연결이 있으면 종료가 최대 5초 늦음
- 큐가 가득 찼을 때 503 테스트는 기능 테스트에 없음 (스텝 5에서 `-t 1`로 확인)
