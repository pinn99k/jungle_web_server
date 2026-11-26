# 스텝 7 읽기 가이드 — 리버스 프록시

페이즈 5 (리버스 프록시) 뒷부분 · 이슈 #7 · 2026-11-24

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `server/proxy.c` | 163 (임시 14 → 163) | 백엔드 선택 · 연결 · 요청 전달 · 응답 중계 |
| `common/net.c` · `.h` | +약 42 | `net_connect` (논블로킹 connect + poll, 1초 제한) |
| `server/conn.c` | +약 15 | `conn_log` 접근 로그, 핸들러 시간 재기 |
| `server/handler.h` | +4 | `status` · `bytes` · `upstream` 칸 (핸들러가 채움) |
| `server/static_handler.c` | +4 | 정적 핸들러도 상태 · 바이트 기록 |
| `server/config.h` · `.c` | −3 | 라우트 안의 `rr` 카운터 삭제 (proxy.c로 이동, Q4) |

새 코드 합계 약 211줄.

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `proxy_handle` | 연결(실패 502) → 요청 만들어 보내기 → 응답 중계 | `router_handler`가 돌려준 포인터로 (`conn_dispatch`) |
| `connect_backend` (static) | `pick_backend`로 고르고 `net_connect`, 실패하면 다음 백엔드로 1번 더 | `proxy_handle` |
| `pick_backend` (static) | `atomic_fetch_add(&rr_counters[칸], 1) % 백엔드 수` | `connect_backend` |
| `build_upstream_request` (static) | 요청 줄 그대로 + 헤더(hop-by-hop 빼고) + `X-Forwarded-For` + `Connection: close` + 본문 | `proxy_handle` |
| `relay_response` (static) | 헤더까지 읽기 → 손님 쪽 헤더 → 받은 본문부터 흘려보내기 | `proxy_handle` |
| `read_response_head` (static) | 응답 헤더가 다 올 때까지 읽기. 첫 바이트 전 시간 초과 504, 그 외 실패 502 | `relay_response` |
| `build_client_header` (static) | 상태 줄 + 백엔드 헤더(hop-by-hop 빼고) + 우리 기준 `Connection` | `relay_response` |
| `is_hop_by_hop` · `add_line` · `send_error` (static) | 넘기면 안 되는 헤더 · 잘림 검사 한 줄 · 502/504 응답 | 위 함수들 |
| `net_connect` · `connect_with_timeout` (net.c) | 1초 안에 연결 | `connect_backend` |
| `conn_log` (conn.c) | `시각 IP "메서드 경로" 상태 바이트 ms [백엔드]` | `conn_dispatch`, `conn_send_error` |

## 3. 요청 하나 기준 호출 순서

`GET /api/time` (백엔드 8001 · 8002):

1. `conn_dispatch` → `router_match` → `/api/` 라우트 → `router_handler` → `proxy_handle(&ctx)`
2. `connect_backend` → `pick_backend`: `rr_counters[0]`을 원자적으로 +1 → 받은 값 % 2 → 8001
   → `ctx->upstream = "127.0.0.1:8001"` → `net_connect(1초)` (실패면 8002로 한 번 더, 둘 다 실패면 502)
3. `net_set_timeout(백엔드 fd, 3초)` → `build_upstream_request`:
   ```
   GET /api/time HTTP/1.1          ← 경로 그대로 (Q1)
   Host: localhost:8080            ← 그대로 (Q2)
   User-Agent: curl/8.5.0
   X-Forwarded-For: 127.0.0.1      ← 손님 IP (이미 있으면 뒤에 붙임, Q6)
   Connection: close               ← 손님이 보낸 Connection · Keep-Alive는 빠짐
   ```
   → `net_write_all(백엔드 fd)`
4. `relay_response` → `read_response_head`: 응답 모드 파서로 헤더까지 (3초 안에 아무것도 없으면 504)
5. `build_client_header`: `HTTP/1.1 200 OK` + 백엔드 헤더(Content-Type · Content-Length 등) + `Connection: keep-alive` (Content-Length가 있고 손님이 keep-alive면)
6. 이미 받아 둔 본문 → 손님, 나머지는 Content-Length만큼 16KB씩 읽어 손님에게
7. `ctx->status = 200`, `ctx->bytes = 44` → 백엔드 fd close → `conn_log` 한 줄 → keep-alive면 다음 요청

흐름 지도로는 ⑦ 프록시 → "연결 2" → 백엔드 → 응답 중계 → ⑧이 실제로 이어졌습니다.

## 4. 추천 읽기 순서

1. `proxy_handle` — 전체 순서 세 덩어리
2. `pick_backend` → `connect_backend` — Round-Robin과 재시도 (Q4의 배열과 칸 번호)
3. `build_upstream_request` — 무엇을 그대로 두고 무엇을 바꾸나 (경로 · Host · XFF · Connection)
4. `relay_response` → `read_response_head` — 502와 504가 갈리는 곳, 응답 모드 파서를 헤더까지만 쓰는 방법
5. `build_client_header` — 백엔드의 `Connection: close`를 손님에게 넘기지 않는 이유
6. `common/net.c` `net_connect` — 논블로킹 connect + poll + SO_ERROR
7. `conn.c` `conn_log` — 핸들러가 채운 칸을 로그로

## 5. 직접 실행해 볼 명령

컨테이너 안 `/work`에서 (터미널 여러 개):
```
make
./build/backend 8001 & ./build/backend 8002 &
./build/server -c conf/server.conf                       # 접근 로그가 이 터미널에 찍힘
for i in 1 2 3 4; do curl -s localhost:8080/api/time; done      # 8001 8002 8001 8002
curl -s -d hello localhost:8080/api/echo                 # hello
time curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/api/slow   # 3초쯤 504
kill %1                                                  # 8001 끄기 → 전부 8002로
kill %2                                                  # 둘 다 끄기 → 502
```

프록시가 백엔드에 무엇을 보내는지 직접 보기:
```
printf 'route /raw/ proxy 127.0.0.1:8003\nroute / static\n' > /tmp/raw.conf
./build/server -c /tmp/raw.conf &
nc -l 8003 &                                             # 가짜 백엔드: 받은 요청을 화면에 찍음
curl -s -m 2 -H 'X-Forwarded-For: 10.0.0.9' localhost:8080/raw/x
```

## 6. 확인 질문

`relay_response`는 백엔드 응답에 Content-Length가 없으면 손님 연결을 keep-alive로 두지 않고 닫습니다. 손님 연결을 계속 열어 두면 손님(브라우저) 쪽에서 무슨 문제가 생길까요?

## 알려진 한계

- 백엔드 연결은 요청마다 새로 맺음 (R3 연결 재사용)
- 죽은 백엔드도 매번 연결을 시도해서 그때마다 거절 · 시간 초과를 겪음 (R7)
- 응답 헤더가 33개 이상이거나 값이 512바이트를 넘으면 파서 상한 때문에 502
- `Connection: X-Foo`처럼 Connection 헤더에 적힌 추가 hop-by-hop 헤더는 지우지 않음
