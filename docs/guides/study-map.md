# 학습 지도 — 전체 흐름부터, 핵심 위주로

기본 구현(스텝 1~8)이 끝난 코드 전체를 한 장으로 보고, 어떤 순서로 공부할지 정한 문서입니다. 먼저 1~4장으로 전체 흐름을 머리에 넣고, 5장 순서대로 핵심 함수만 깊게 봅니다. 스텝별 자세한 내용은 `step1.md` ~ `step8.md`에 있습니다.

## 목차

1. 한 장 요약
2. 파일 지도
3. 큰 흐름 다섯 개
4. 데이터 모양과 공유 상태
5. 학습 순서 (7단계)
6. 계속 반복되는 원리 세 가지

---

## 1. 한 장 요약

프로그램은 두 개입니다.

```
[curl · 브라우저]
      │ 연결 1 (8080)
      ▼
┌──────────────── build/server ────────────────┐
│ main          시작 · accept 반복 · 종료        │
│  └ thread_pool  큐 + 워커 8명                   │
│      └ conn     연결 하나의 수명 (요청 반복)     │
│          ├ http_parser  바이트 → 요청 구조체     │
│          ├ router       경로 → 핸들러 고르기     │
│          ├ static_handler  www/ 파일 응답       │
│          └ proxy  ───────── 연결 2 ─────────┐  │
└──────────────────────────────────────────────┼─┘
                                               ▼
                              build/backend 8001 · 8002 (테스트용)
```

- `common/`: 서버와 백엔드가 같이 쓰는 부품 (소켓, 버퍼, 파서, 응답 만들기)
- `server/`: 서버에만 있는 것 (설정, 라우터, 핸들러, 스레드 풀, 연결)
- `backend/`: 프록시 시험용 작은 서버 (`common/`만 씀)

## 2. 파일 지도

줄 수는 빈 줄 · 주석을 뺀 수입니다. ★ = 꼭 볼 함수.

| 층 | 파일 | 줄 | 한 줄 역할 | ★ 핵심 함수 | 스텝 |
|---|---|---|---|---|---|
| 시작 | `server/main.c` | 128 | 설정 읽고, 손님 받아 큐에 넣고, 신호 오면 정리 | `main`, `accept_loop`, `start_pool` | 1 → 8 |
| 동시성 | `server/thread_pool.c` | 98 | 큐 + 워커 N명 | `pool_submit`, `worker_main` | 5 |
| 연결 | `server/conn.c` | 170 | 연결 하나: 읽기 → 처리 → keep-alive 반복 | `conn_serve`, `conn_read_request`, `conn_dispatch` | 5 |
| 고르기 | `server/router.c` | 27 | 가장 긴 prefix 라우트 | `router_match` | 4 |
| 설정 | `server/config.c` | 201 | 설정 파일 · 인자 → `struct server_config` | `config_apply_args`, `parse_route_line` | 4 |
| 핸들러 | `server/static_handler.c` | 193 | 경로 방어 → 파일 응답 | `static_handle`, `static_resolve_path` | 3 |
| 핸들러 | `server/proxy.c` | 163 | 백엔드 고르기 → 전달 → 응답 중계 | `proxy_handle`, `relay_response`, `pick_backend` | 7 |
| 보조 | `server/mime.c` | 33 | 확장자 → Content-Type | `mime_from_path` | 3 |
| 공용 | `common/net.c` | 128 | 소켓 I/O 유일 창구 | `net_listen`, `net_read`, `net_write_all` | 1 · 5 · 7 · 8 |
| 공용 | `common/buf.c` | 46 | 늘어나는 바이트 버퍼 | `buf_append` | 1 |
| 공용 | `common/http_parser.c` | 291 | 바이트 → `http_message` (증분) | `http_parse`, `end_of_headers` | 2 |
| 공용 | `common/http_response.c` | 80 | 상태 줄 · 헤더 · 오류 응답 만들기 | `http_build_header` | 3 |
| 백엔드 | `backend/main.c` | 115 | `/api/time` · `/api/echo` · `/api/slow` | `route_request` | 6 |

나머지 함수(`is_ows`, `hex_value`, `add_line`, `appendf` 같은 것)는 이름 그대로 하는 작은 도우미라 처음에는 건너뛰어도 됩니다.

## 3. 큰 흐름 다섯 개

여기만 이해하면 어느 함수를 열어도 "지금 전체 중 어디인지" 압니다.

### 흐름 A — 서버 시작 (main 스레드)
```
main                                    server/main.c
 ├ config_defaults → config_apply_args  설정 (-c 파일 먼저, 인자가 덮음)
 ├ stat(root)                           루트 폴더 확인
 ├ install_signals                      SIGPIPE 무시, SIGINT · SIGTERM → on_signal
 ├ net_listen(8080, 128)                common/net.c   리스닝 소켓
 ├ start_pool                           신호를 막은 채 워커 8명 생성 → main만 신호 받음
 └ accept_loop                          ↓ 흐름 B로 연결을 넘김
```

### 흐름 B — 연결 하나 (main → 워커)
```
accept_loop (main)
 ├ net_accept          손님 하나 꺼냄 (없으면 잠듦)
 └ pool_submit         큐에 {fd, ip} 넣고 워커 깨움 (가득 차면 503)
        ▼
worker_main (워커)     server/thread_pool.c   큐에서 꺼냄 (락 안) → 락 풀고 실행
 └ on_connection       server/main.c
    ├ conn_new         타임아웃 5초, TCP_NODELAY
    ├ conn_serve       ── 반복 ──────────────────────────────┐   server/conn.c
    │   ├ conn_read_request   net_read → buf_append → http_parse
    │   │                     (덜 왔으면 다시 read, 오류면 400 · 408 …)
    │   ├ conn_dispatch       keep-alive 결정 → router_match → handler(ctx)
    │   │                     → conn_log 한 줄
    │   ├ buf_consume         처리한 요청만큼 지움 (뒤에 남은 건 다음 요청)
    │   └ conn_should_continue  keep-alive면 반복 ──────────────┘
    └ conn_free        (필요하면 lingering close) → close
```

### 흐름 C — 정적 파일 요청 (`/style.css`)
```
static_handle                           server/static_handler.c
 ├ GET · HEAD가 아니면 405
 ├ static_resolve_path   디코드 → 정규화(.. 막기) → 점 파일 404 → realpath → 루트 안인지
 ├ open_file             디렉토리면 / 없을 때 301, 있으면 index.html
 ├ http_build_header     common/http_response.c   (mime_from_path로 타입)
 └ send_file_body        64KB씩 읽어 보내기
```

### 흐름 D — 프록시 요청 (`/api/time`)
```
proxy_handle                            server/proxy.c
 ├ connect_backend
 │   ├ pick_backend        rr_counters[라우트] 원자적 +1 → 8001 / 8002
 │   └ net_connect         1초 안에 연결, 실패면 다른 백엔드로 1번 더 (둘 다 실패 502)
 ├ build_upstream_request  경로 · Host 그대로, XFF 추가, Connection: close
 ├ net_write_all           백엔드로 요청 전송
 └ relay_response
     ├ read_response_head  응답 헤더까지 (3초 안에 아무것도 없으면 504)
     ├ build_client_header 손님용 헤더 (Connection은 우리 기준)
     └ 본문 16KB씩 흘려보내기
```

### 흐름 E — 종료 (Ctrl+C · SIGTERM)
```
on_signal               conn_stopping = 1 (플래그만)
accept_loop             accept가 EINTR로 깸 → 루프 탈출
main                    리스닝 close → pool_shutdown
pool_shutdown           큐에 남은 연결까지 처리, 처리 중 응답은 Connection: close → 워커 회수
main                    "종료" → return 0 (이때 ASan 누수 검사)
```

## 4. 데이터 모양과 공유 상태

데이터가 어떻게 바뀌어 가나:
```
손님 바이트 ─net_read─▶ chunk ─buf_append─▶ struct buf (힙, 연결마다)
  ─http_parse─▶ struct http_message (메서드 · 경로 · 헤더 32개 · 길이 · keep-alive)
  ─conn_dispatch─▶ struct request_ctx (fd · msg · 원본 바이트 · 라우트 · 설정)
  ─handler─▶ 응답 바이트 (헤더 buf + 파일 / 백엔드 응답 조각)
  ─handler가 채움─▶ ctx.status · bytes · upstream ─conn_log─▶ 로그 한 줄
```

여러 스레드가 함께 보는 것은 네 가지뿐입니다:

| 무엇 | 어디 | 누가 쓰나 | 왜 안전한가 |
|---|---|---|---|
| 설정 `cfg` | main.c (정적) | 시작할 때 main만 | 워커가 생기기 전에 다 쓰고, 그 뒤 아무도 안 씀 (읽기 전용) |
| 작업 큐 | thread_pool.c | main(넣기) · 워커(꺼내기) | mutex 하나 + 조건 변수 |
| Round-Robin 카운터 | proxy.c `rr_counters` | 워커들 | `atomic_fetch_add` (끊김 없는 +1) |
| 종료 플래그 | conn.c `conn_stopping` | 시그널 핸들러 | 원자적 변수 |

나머지(`struct conn`, 버퍼, 파서)는 연결 하나를 맡은 워커 하나만 써서 락이 없습니다.

## 5. 학습 순서 (7단계)

순서는 "요청 하나가 들어오는 순서"를 따릅니다. 각 단계에서 ★ 함수만 깊게 보고, 나머지는 흐름 그림의 이름만 확인하고 넘어갑니다. 가이드의 "직접 실행해 볼 명령"을 꼭 한 번씩 쳐 봅니다.

| 단계 | 주제 | ★ 볼 함수 (파일) | 질문 하나 | 가이드 | 시간 |
|---|---|---|---|---|---|
| 0 | 전체 흐름 | 이 문서 3장 흐름 A~E | 흐름 B를 안 보고 종이에 그릴 수 있나 | — | 30분 |
| 1 | 바이트 받기 | `net_listen` · `net_read` · `net_write_all` (net.c), `buf_append` (buf.c) — `grep -n "핵심:"` | 손님 연결은 누가 맺고, 우리는 언제 꺼내나 | step1 | 1시간 |
| 2 | 해석하기 | `http_parse` · `find_crlf` · `end_of_headers` · `resolve_body_length` (http_parser.c) | 왜 위치를 포인터가 아니라 `pos`로 기억하나 | step2 | 1.5시간 |
| 3 | 답하기 | `static_handle` · `static_resolve_path` · `normalize_path` (static_handler.c), `http_build_header` | 경로 방어 4겹이 각각 무엇을 막나 | step3 | 1시간 |
| 4 | 고르기 | `router_match` (router.c), `config_apply_args` (config.c) | `/apix`는 왜 `/api` 라우트로 안 가나 | step4 | 30분 |
| 5 | 동시에 하기 | `pool_submit` · `worker_main` (thread_pool.c), `conn_serve` · `conn_read_request` (conn.c) | 락은 어디서 잡고 어디서 푸나, 왜 거기서 푸나 | step5 | 1.5시간 |
| 6 | 대신 전달하기 | `pick_backend` · `build_upstream_request` · `relay_response` (proxy.c), `net_connect` | 502와 504는 어디서 갈리나 | step6 · 7 | 1.5시간 |
| 7 | 안전하게 끄기 | `on_signal` · `start_pool` · `accept_loop` · `main` 끝 (main.c) | 워커에서 신호를 막지 않으면 왜 안 꺼지나 | step8 | 45분 |

각 단계가 끝나면 질문에 한두 줄로 답을 보내 주세요. 맞으면 다음 단계, 빠진 게 있으면 그 부분만 다시 봅니다.
마지막에 `docs/explain-log.md`의 대기 항목을 확인 질문으로 정리하고, GitHub 이슈 #1~#9의 개념 체크리스트를 닫습니다.

## 6. 계속 반복되는 원리 세 가지

코드 곳곳에서 같은 이유가 계속 나옵니다. 이 세 가지를 잡으면 처음 보는 함수도 "왜 이렇게 했는지" 추측할 수 있습니다.

1. **TCP는 끊김 없는 바이트 흐름이다** — `read`는 아무 데서나 끊어 준다. 그래서 버퍼에 쌓고(buf), 경계를 찾는 파서가 있고(http_parser), 끝을 알리려고 Content-Length · 연결 닫기가 필요하다(proxy Q8).
2. **기다리는 동안 스레드는 멈춘다 (블로킹)** — accept · read · connect는 기다린다. 그래서 워커를 여럿 두고(thread_pool), 기다림에 상한을 두고(타임아웃 · 408 · 504), 기다림을 깨우는 방법이 필요하다(EINTR · 시그널).
3. **함께 쓰는 것은 최소로** — 연결 상태는 워커 하나만(struct conn), 설정은 읽기 전용, 함께 바꾸는 건 큐 · 카운터 · 종료 플래그 셋뿐이고 각각 락 또는 원자적 연산으로 지킨다.
