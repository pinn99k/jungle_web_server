# 스텝 5 읽기 가이드 — 스레드 풀과 연결 수명

페이즈 4 (동시성) · 이슈 #5 · D1 = 스레드 풀 (#9) · 2026-11-24

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `server/thread_pool.h` · `.c` | 8 · 98 | 새로 만듦. 원형 작업 큐 + 워커 N개 |
| `server/conn.h` · `.c` | 8 · 150 | 새로 만듦. 연결 하나의 수명: 읽기 · 타임아웃 · keep-alive 반복 · lingering close |
| `common/net.h` · `.c` | +9 | `net_set_timeout` (스텝 7에서 앞당김) |
| `server/main.c` | 88 (−9) | `handle_client` · `dispatch`를 conn으로 옮기고, 스레드 풀 · SIGPIPE 무시 · 503 추가 |
| `server/handler.h` | −1 | `root` 필드 삭제 (`cfg->root` 사용) |
| `server/static_handler.c` | +2 | HEAD 오류 응답은 헤더만 |
| `Makefile` | +1 | thread_pool · conn 링크 |

새 코드 합계 약 265줄.

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `main` | 설정 → SIGPIPE 무시 → 리스닝 → `pool_create` → accept 반복 → `pool_submit` | — |
| `reject_busy` (main.c) | 큐가 가득 차면 503 보내고 닫기 | `main` |
| `on_connection` (main.c) | 워커가 실행: `conn_new` → `conn_serve` → `conn_free` | `worker_main` (함수 포인터로) |
| `pool_create` | 워커 N개 + 빈 큐 | `main` |
| `pool_submit` | 큐에 `{fd, ip}` 넣고 워커 하나 깨움. 가득 차면 -1 | `main` |
| `worker_main` (static) | 큐가 빌 때 잠들고, 꺼내서 `work` 실행을 반복 | 스레드 시작 함수 |
| `pool_shutdown` · `pool_destroy` (static) | 워커를 깨워 끝내고 회수 · 해제 | (스텝 8 종료 처리에서 씀), `pool_create` 실패 시 |
| `conn_new` | 연결 객체 + 읽기 · 쓰기 타임아웃 5초 | `on_connection` |
| `conn_serve` | 읽기 → (오류면 응답 후 끝) → dispatch → 소비 → 계속할지 판단, 반복 | `on_connection` |
| `conn_read_request` (static) | 요청 하나가 완성될 때까지 읽기. DONE · ERROR · CLOSED · IDLE · TIMEOUT | `conn_serve` |
| `conn_dispatch` (static) | keep-alive 결정 → `router_match` → 핸들러 호출 | `conn_serve` |
| `conn_send_error` (static) | 오류 응답 (HEAD면 헤더만), 닫을 때 lingering 표시 | `conn_serve`, `conn_dispatch` |
| `conn_should_continue` (static) | 핸들러 성공 && keep-alive면 계속 | `conn_serve` |
| `conn_free` · `lingering_close` (static) | 필요하면 쓰기만 닫고 남은 입력 버린 뒤 close | `on_connection` |
| `net_set_timeout` | SO_RCVTIMEO · SO_SNDTIMEO | `conn_new`, `lingering_close` |

## 3. 요청 하나 기준 호출 순서

브라우저가 keep-alive로 `/index.html`과 `/style.css`를 차례로 요청:

1. `main`이 `net_accept`로 fd를 받고 `pool_submit(pool, fd, ip)` → 큐에 넣고 `not_empty` 신호. main은 곧장 다음 accept로.
2. 잠들어 있던 워커 하나가 깨어나 큐에서 꺼냄 (락 안에서) → 락을 풀고 `on_connection(fd, ip, &cfg)`.
3. `conn_new` → 소켓 타임아웃 5초.
4. `conn_serve` 반복 1회차:
   - `conn_read_request`: `net_read` → `buf_append` → `http_parse` DONE
   - `conn_dispatch`: HTTP/1.1이라 keep = 1 → `router_match` → `static_handle` (응답 헤더 `Connection: keep-alive`)
   - `buf_consume(total_len)`, 파서 초기화, 계속
5. 2회차: 같은 연결에서 `/style.css`를 읽어 같은 길로 응답.
6. 3회차: 브라우저가 더 보내지 않음 → 5초 뒤 `net_read`가 EAGAIN, 버퍼가 비어 있으므로 IDLE → 조용히 끝.
7. `conn_free` (lingering 필요 없음) → close → 워커는 다시 큐 앞에서 잠듦.

흐름 지도로는 ② ③이 새로 생겼고, ④ ~ ⑧이 한 연결 안에서 반복됩니다.

## 4. 추천 읽기 순서

1. `server/main.c` `main` — accept 스레드는 받기만 하고 넘긴다
2. `server/thread_pool.c` `struct thread_pool` → `pool_submit` → `worker_main` — 락을 잡는 구간과 푸는 구간
3. `server/conn.c` `struct conn` — 연결마다 따로인 상태
4. `conn_serve` — 반복 한 바퀴
5. `conn_read_request` — 5초(한 번) · 10초(전체) · IDLE과 TIMEOUT의 차이
6. `conn_send_error` → `conn_free` → `lingering_close` — 왜 바로 닫지 않나 (Q2)

## 5. 직접 실행해 볼 명령

컨테이너 안 `/work`에서:
```
make test && make asan
./build/server -c conf/server.conf                                     # 터미널 1
curl -sv localhost:8080/index.html localhost:8080/style.css 2>&1 | grep Re-using
nc localhost 8080                                                      # 아무것도 안 보내면 5초 뒤 끊김
ab -n 1000 -c 50 http://127.0.0.1:8080/index.html | grep -E "Complete|Failed"
(printf 'GET / HTTP/1.1\r\nHo'; sleep 7) | nc localhost 8080           # 408
printf 'GET /style.css HTTP/1.1\r\nHost: x\r\n\r\nGET /app.js HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc localhost 8080   # 응답 2개
```

페이즈 1에서 막혔던 실험이 이제 풀립니다:
```
nc localhost 8080 &              # 연결만 붙잡음
curl -s -m 2 localhost:8080/     # 바로 응답 (다른 워커가 받음)
```

스레드 풀의 한계 보기 (Q11, 리팩토링 R6):
```
ab -k -n 300 -c 9 http://127.0.0.1:8080/index.html | grep "Time taken"   # 약 6초: 9번째 연결이 5초 기다림
ab -k -n 300 -c 8 http://127.0.0.1:8080/index.html | grep "Time taken"   # 비교
```

## 6. 확인 질문

`worker_main`은 큐에서 작업을 꺼낸 뒤 락을 풀고 나서 `work`(= `on_connection`)를 부릅니다. 만약 락을 잡은 채로 `work`를 불렀다면, 워커 8명과 main 스레드는 각각 어떻게 될까요?

## 알려진 한계

- keep-alive로 쉬는 연결이 워커를 최대 5초 붙잡음. 연결이 워커보다 많으면 새 손님이 기다림 (Q11 → R6, R1)
- 큐가 가득 찼을 때의 503은 lingering 없이 닫아 유실될 수 있음 (Q10)
- `pool_shutdown`은 아직 부르는 곳이 없음. Ctrl+C면 바로 죽음 → 스텝 8
