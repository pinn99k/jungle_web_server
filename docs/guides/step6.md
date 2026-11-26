# 스텝 6 읽기 가이드 — 테스트용 백엔드

페이즈 5 (리버스 프록시) 앞부분 · 이슈 #6 · 2026-11-24

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `backend/main.c` | 115 | 새로 만듦. 단일 스레드 백엔드 |
| `Makefile` | +15 −4 | `build/backend` · `build/asan/backend` (common/만 링크) |

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `main` | 포트 인자 → SIGPIPE 무시 → 리스닝 → accept · 처리 · close 반복 | — |
| `handle_conn` (static) | 요청 하나를 읽고 파싱 (5초 타임아웃) → `route_request` 또는 오류 응답 | `main` |
| `route_request` (static) | `GET /api/time` · `POST /api/echo` · `GET /api/slow` · 그 외 404 | `handle_conn` |
| `path_is` (static) | `?` 앞 경로가 정확히 같은가 | `route_request` |
| `respond` (static) | `http_build_header` + 본문 → `net_write_all` (항상 Connection: close) | `route_request` |
| `respond_error` (static) | `http_build_error` → `net_write_all` | `route_request`, `handle_conn` |

서버 쪽에서 가져다 쓰는 것: `buf` · `net` · `http_parser` · `http_response` (모두 `common/`). `server/`는 하나도 쓰지 않습니다.

## 3. 요청 하나 기준 호출 순서

`curl -d hello localhost:8001/api/echo`:

1. `main`의 `net_accept` → fd
2. `handle_conn`: `net_set_timeout(5초)` → `net_read` → `buf_append` → `http_parse` DONE (본문 5바이트까지)
3. stderr에 `backend 8001: POST /api/echo`
4. `route_request`: POST + `/api/echo` → `respond(200, "application/x-www-form-urlencoded", raw + header_len, 5)` (curl이 보낸 Content-Type 그대로)
5. `main`이 close → 다음 accept

`/api/slow`면 4에서 `sleep(5)` 동안 이 백엔드는 다른 요청을 받지 못합니다 (단일 스레드).

흐름 지도에서는 ⑦ 프록시 화살표 끝의 "backend 8001 · 8002" 상자가 실제로 생긴 것입니다.

## 4. 추천 읽기 순서

1. `main` — 서버의 스텝 1 `main`과 거의 같은 모양 (스레드 없음)
2. `handle_conn` — 서버의 `conn_read_request`를 아주 단순하게 줄인 것
3. `route_request` — 각 경로가 프록시의 어떤 기능을 시험하려고 있는지 (주석의 "이유")
4. `respond` / `respond_error` — 서버와 같은 `http_response`를 쓰는 방법

## 5. 직접 실행해 볼 명령

컨테이너 안 `/work`에서:
```
make
./build/backend 8001 &                       # 백엔드 1
./build/backend 8002 &                       # 백엔드 2 (스텝 7 Round-Robin용)
curl -s localhost:8001/api/time              # {"port":8001,"time":"..."}
curl -s localhost:8002/api/time              # {"port":8002,...}
curl -s -d hello localhost:8001/api/echo     # hello
time curl -s localhost:8001/api/slow         # 약 5초
curl -s -o /dev/null -w "%{http_code}\n" localhost:8001/nope   # 404
```

단일 스레드 확인 (스텝 7의 504 · Round-Robin 이해에 필요):
```
curl -s localhost:8001/api/slow &            # 8001이 5초 동안 바쁨
time curl -s localhost:8001/api/time         # 약 5초 기다린 뒤 응답
```

## 6. 확인 질문

백엔드는 일부러 단일 스레드로 만들었습니다. 스텝 7에서 프록시가 8001과 8002를 번갈아 쓰는데 8001이 `/api/slow`를 처리하는 중이라면, 그 사이 8001로 배정된 `/api/time` 요청은 어떻게 될까요? (프록시의 응답 제한은 3초)

## 알려진 한계

- 연결 하나에 요청 하나만 처리 (프록시가 Connection: close로 보내므로 충분)
- 백엔드는 모든 주소에서 듣지만, 컨테이너 밖으로 열린 포트는 8080뿐이라 바깥에서는 못 들어옴 (Q5 지도 Q5)
