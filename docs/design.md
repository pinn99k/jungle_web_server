# 미니 웹 서버 설계 문서

C로 만드는 HTTP 웹 서버 + 리버스 프록시의 설계 문서입니다. 구현은 이 문서의 스텝 순서와 함수 목록을 그대로 따릅니다. 함수 시그니처를 바꿔야 하면 코드보다 이 문서를 먼저 고칩니다.

## 목차

1. [목표와 범위](#1-목표와-범위)
2. [전체 구조](#2-전체-구조)
3. [폴더와 파일](#3-폴더와-파일)
4. [의존 규칙](#4-의존-규칙)
5. [결정 사항](#5-결정-사항)
6. [공용 자료구조](#6-공용-자료구조)
7. [스텝별 설계](#7-스텝별-설계)
8. [엣지 케이스 대응표](#8-엣지-케이스-대응표)
9. [테스트 전략](#9-테스트-전략)
10. [리팩토링 학습 로드맵](#10-리팩토링-학습-로드맵)

---

## 1. 목표와 범위

### 목표

- 명세서(`docs/requirements.md`)의 필수 항목을 모두 구현합니다.
- 코드를 함수 단위로 읽고 이해할 수 있게 만듭니다. 그다음 리팩토링으로 학습합니다.

### 이번에 하는 것

- GET / HEAD / POST, HTTP/1.0과 1.1, keep-alive
- 정적 파일 서빙: MIME 매핑, 403 / 404 / 500
- 리버스 프록시: 경로 기반 라우팅, Round-Robin, 백엔드 실패 시 502 / 504
- 테스트용 백엔드: `/api/echo`, `/api/time`, `/api/slow`
- 스레드 풀 기반 동시 처리 (결정 대기, 5장 참고)
- SIGINT / SIGTERM 안전 종료, SIGPIPE 무시
- 단위 테스트, 기능 테스트, 부하 테스트, ASan 검사

### 이번에 하지 않는 것 (리팩토링 단계로 미룸)

- `Transfer-Encoding: chunked` → 501로 응답
- HTTPS, `CONNECT`
- 포워드 프록시, 캐싱
- 직접 만든 클라이언트 (`client/`)
- epoll 이벤트 루프

---

## 2. 전체 구조

```
curl / 브라우저
     │  TCP 연결
     ▼
┌──────────────────────── server/ ────────────────────────┐
│  main ── 설정 로드, 시그널 등록, 리스닝 소켓, accept 루프   │
│    │ accept한 fd                                          │
│    ▼                                                      │
│  thread_pool ── 작업 큐에 fd를 넣고, 워커 스레드가 꺼냄     │
│    │                                                      │
│    ▼                                                      │
│  conn ── 연결 하나의 수명: 읽기 → 파싱 → 라우팅 → 핸들러    │
│    │      → keep-alive면 반복, 아니면 닫기                 │
│    ├─▶ http_parser (common)  바이트 → 요청 구조체          │
│    ├─▶ router                 경로 → 라우트 + 핸들러        │
│    │      ├─▶ static_handler  파일 찾아 보내기 ── mime     │
│    │      └─▶ proxy           백엔드로 전달 ──────────┐     │
│    └─▶ http_response (common) 상태 라인·헤더·오류 본문 │     │
└───────────────────────────────────────────────────────┼─────┘
                                                        │ TCP 연결 (새로)
                                                        ▼
                                        backend/ (8001, 8002, …)
```

요청 하나의 호출 순서 (정적 파일, keep-alive 연결):

1. `main`의 accept 루프가 `net_accept()`로 연결 fd를 받습니다.
2. `pool_submit()`으로 작업 큐에 넣습니다.
3. 워커 스레드가 꺼내 `conn_new()` → `conn_serve()`를 호출합니다.
4. `conn_serve()` 안에서 반복합니다.
   - `conn_read_request()`: `net_read()` + `http_parse()`를 요청이 완성될 때까지 반복
   - `conn_dispatch()`: `router_match()`로 라우트를 고르고, 그 라우트의 핸들러를 호출
   - `static_handle()`: 경로 변환 → 파일 열기 → `http_build_header()` → 파일 전송
   - 처리한 요청 바이트를 버퍼에서 지우고(`buf_consume()`), keep-alive면 다음 요청으로
5. 연결이 끝나면 `conn_free()`가 fd를 닫고 메모리를 해제합니다.

---

## 3. 폴더와 파일

```
mini_web_server/
├─ CLAUDE.md                 Claude Code 작업 규칙
├─ Makefile                  server, backend, tests 빌드
├─ conf/
│  └─ server.conf            포트, 문서 루트, 스레드 수, 타임아웃, 라우팅 규칙
├─ www/                      정적 파일 루트 (테스트용 html, css, js, png, jpg)
├─ common/                   server와 backend가 함께 쓰는 모듈
│  ├─ buf.c / .h             크기가 늘어나는 바이트 버퍼
│  ├─ net.c / .h             소켓 헬퍼: listen, accept, connect, read, write
│  ├─ http_parser.c / .h     요청·응답 파서 (증분 파싱)
│  └─ http_response.c / .h   응답 헤더·오류 응답 바이트 생성
├─ server/
│  ├─ main.c                 시작·종료, 시그널, accept 루프
│  ├─ config.c / .h          설정 파일 + CLI 인자 → struct server_config
│  ├─ thread_pool.c / .h     작업 큐 + 워커 스레드
│  ├─ conn.c / .h            연결 하나의 수명 관리
│  ├─ handler.h              핸들러 공통 시그니처와 struct request_ctx
│  ├─ router.c / .h          경로 → 라우트 + 핸들러
│  ├─ static_handler.c / .h  경로 변환, 경로 탐색 방어, 파일 전송
│  ├─ mime.c / .h            확장자 → Content-Type
│  └─ proxy.c / .h           리버스 프록시
├─ backend/
│  └─ main.c                 테스트용 백엔드 (common만 사용)
├─ tests/
│  ├─ test.h                 assert 매크로
│  ├─ test_http_parser.c
│  ├─ test_mime.c
│  ├─ test_router.c
│  ├─ test_static_path.c
│  └─ functional.sh          curl 기반 기능 테스트
└─ docs/
   ├─ requirements.md        과제 명세서
   ├─ design.md              이 문서
   ├─ adr/                   설계 결정 기록
   └─ guides/                스텝별 읽기 가이드 (스텝마다 하나씩 추가)
```

예상 코드 규모는 빈 줄과 주석을 빼고 약 2,000~2,500줄입니다.

---

## 4. 의존 규칙

```
[최상위]  server/main.c, backend/main.c
              ↓
[구조]    thread_pool
              ↓
[연결]    conn
              ↓
[핸들러]  router → static_handler, proxy
              ↓
[I/O]     net                         ← 소켓을 다루는 유일한 공용 모듈
[순수]    http_parser, http_response, buf, mime, config
```

1. **위층은 아래층을 호출할 수 있고, 아래층은 위층을 `#include`하지 않습니다.**
2. **순수 층은 fd를 받지 않습니다.** 바이트와 구조체만 받으므로 소켓 없이 단위 테스트합니다.
3. **`backend/`는 `common/`만 사용합니다.** `server/` 파일을 `#include`하지 않습니다.
4. **연결마다 따로여야 하는 상태는 `struct conn` 안에만 둡니다.** 전역 가변 상태는 종료 플래그와 Round-Robin 카운터 두 개뿐이고, 둘 다 원자적(`atomic`) 변수로 둡니다.

---

## 5. 결정 사항

### 결정 대기

| ID | 항목 | 추천 | 기록 |
|---|---|---|---|
| D1 | 동시성 모델 | A. 스레드 풀 + 블로킹 I/O | `docs/adr/0001-concurrency-model.md` |

D1이 정해지기 전까지 스텝 1~4는 영향을 받지 않습니다. 스텝 5부터 영향을 받습니다.

### 확정

| ID | 항목 | 결정 | 기록 |
|---|---|---|---|
| D2 | 프록시 종류, 폴더 구조 | 리버스 프록시. `server/` + `backend/` + `common/` | `docs/adr/0002-structure-and-proxy.md` |

### 기본값 (바꾸려면 이 표를 고친 뒤 구현)

| 항목 | 값 |
|---|---|
| 언어 / 표준 | C11 + POSIX (`-std=c11 -D_POSIX_C_SOURCE=200809L`) |
| 빌드 | Makefile: `make`, `make test`, `make asan`, `make clean` |
| 설정 형식 | `conf/server.conf`에 `키 값` 한 줄씩. CLI 인자가 파일 값을 덮어씀 |
| 요청 라인 상한 | 8KB (초과 → 414) |
| 헤더 상한 | 전체 8KB, 최대 32개, 이름 64B, 값 512B (초과 → 431) |
| 본문 상한 | 1MB (초과 → 413) |
| 라우팅 | 경로 prefix 기반, 가장 긴 prefix 우선 |
| 스레드 수 / 작업 큐 크기 | 8 / 128 (큐가 가득 차면 503) |
| 클라이언트 타임아웃 | `read()` 한 번당 5초, 요청 하나 전체 10초 (초과 → 408) |
| 연결당 최대 요청 수 | 100 (넘으면 `Connection: close`) |
| 백엔드 타임아웃 | 연결 1초, 응답 3초 (연결 실패 → 502, 응답 초과 → 504) |
| 백엔드 연결 | 요청마다 새 연결, `Connection: close`로 보냄 |
| 백엔드 실패 처리 | 연결 실패 시 다음 백엔드로 한 번 재시도, 그래도 실패하면 502 |

설정 파일 예시:

```
port 8080
root ./www
threads 8
client_timeout_ms 5000
request_deadline_ms 10000
backend_connect_timeout_ms 1000
backend_timeout_ms 3000
route /api/ proxy 127.0.0.1:8001 127.0.0.1:8002
route / static
```

---

## 6. 공용 자료구조

스텝마다 실제 정의를 헤더에 둡니다. 여기서는 무엇을 담는지만 정리합니다.

| 구조체 | 위치 | 담는 것 | 생성 → 해제 |
|---|---|---|---|
| `struct buf` | common/buf.h | `data`, `len`(채운 길이), `cap`(할당 크기) | `buf_init` → `buf_free` |
| `struct http_header` | common/http_parser.h | `name[64]`, `value[512]` | 파서 안 |
| `struct http_message` | common/http_parser.h | 요청: `method`, `target`. 응답: `status`. 공통: `minor_version`, 헤더 배열, `content_length`(-1 = 없음), `header_len`, `total_len`, `keep_alive` | 파서 안 |
| `struct http_parser` | common/http_parser.h | `mode`(요청/응답), `state`, `pos`, `error_status`, `msg` | `http_parser_init` |
| `struct server_config` | server/config.h | 포트, 루트, 스레드 수, 타임아웃들, 라우트 배열 | `config_defaults` (정적 메모리, 해제 없음) |
| `struct route` | server/config.h | `prefix`, `type`(정적/프록시), 백엔드 배열, 백엔드 개수, Round-Robin 카운터 | 설정 로드 시 |
| `struct request_ctx` | server/handler.h | 클라이언트 fd·IP, 요청 메시지, 원본 요청 바이트(본문 포함), 라우트, 설정, keep-alive 여부 | `conn_dispatch` 안의 지역 변수 |
| `struct conn` | server/conn.c (불투명) | fd, 클라이언트 IP, 수신 `buf`, `http_parser`, 처리한 요청 수, 요청 마감 시각, 설정 포인터 | `conn_new` → `conn_free` |
| `struct thread_pool` | server/thread_pool.c (불투명) | 워커 스레드 배열, 원형 작업 큐, mutex, 조건 변수 2개, 종료 플래그 | `pool_create` → `pool_shutdown` |

헤더 값을 버퍼를 가리키는 포인터가 아니라 **고정 크기 배열에 복사**하는 이유: 본문을 더 읽다가 `buf_append()`가 메모리를 다시 할당하면, 버퍼를 가리키던 포인터는 모두 무효가 됩니다. 복사해 두면 이 문제가 없습니다. 대신 크기 상한이 생기므로 넘으면 431로 거부합니다.

핸들러 공통 시그니처 (server/handler.h):

```c
// 핸들러: 요청 하나를 처리해 응답을 보낸다. 0 = 계속 가능, -1 = 연결을 닫아야 함
typedef int (*handler_fn)(struct request_ctx *ctx);
```

`static_handle`과 `proxy_handle`이 이 시그니처를 따릅니다. 라우터는 어느 함수를 부를지만 정하고, `conn`은 상대가 누구인지 모르고 호출합니다.

---

## 7. 스텝별 설계

각 스텝은 300줄 이하이고, 끝나면 실행해서 확인할 수 있습니다. 표의 "호출하는 곳"은 그 함수를 부르는 파일입니다. `static` 표시가 있는 함수는 그 파일 안에서만 쓰는 내부 함수입니다.

| 스텝 | 내용 | 예상 줄 수 |
|---|---|---|
| 1 | `buf` + `net` + 최소 서버 | ~150 |
| 2 | `http_parser` + 단위 테스트 | ~300 |
| 3 | `http_response` + `mime` + `static_handler` | ~300 |
| 4 | `config` + `router` | ~230 |
| 5 | `thread_pool` + `conn` | ~320 |
| 6 | `backend/` | ~150 |
| 7 | `proxy` | ~250 |
| 8 | 시그널 종료, 엣지 케이스, 기능·부하 테스트 | ~200 |

### 페이즈

스텝을 "만든다 → 써 보면 문제가 드러난다 → 다음 페이즈"로 묶습니다. 이슈 번호는 스텝 번호와 같고, GitHub 마일스톤이 페이즈입니다.

| 페이즈 | 스텝 | 끝나면 되는 것 | 드러나는 문제 → 다음 페이즈 | 시작 전에 닫을 것 |
|---|---|---|---|---|
| 1 연결 | 1 | 소켓으로 받고 고정 응답 | 무슨 요청이든 같은 답 → 해석이 필요 | `make asan` 시점 (닫힘: 스텝 1부터), 헤더 상한, accept 실패 |
| 2 이해 | 2 | 바이트 → `http_message` (단위 테스트) | 알아들어도 돌려줄 게 없음 → 응답이 필요 | — |
| 3 정적 웹서버 | 3 → 4 | 브라우저에서 `www/` 페이지가 뜸 (스레드 1개) | 한 명이 붙잡으면 모두 멈춤, 요청마다 새 연결 → 동시성 | — |
| 4 동시성 | 5 | 동시 접속, keep-alive, 408 / 503 | 동적 요청 처리 · 프록시(필수)가 없음 → 프록시 | D1 (#9). 300줄 넘으면 5a `thread_pool` / 5b `conn`으로 나눔 |
| 5 리버스 프록시 | 6 → 7 | `/api/`가 백엔드 2대로 번갈아 감, 502 / 504 | Ctrl+C · 손님 끊김에 약함 → 종료 처리 | 경로 그대로 전달 여부, `Host` 헤더, 접근 로그 |
| 6 단단하게 | 8 | 안전 종료, 엣지 케이스 대응표 전부, 부하 측정 | 기본 구현 완료. 측정으로 한계가 숫자로 보임 | — |
| 7 리뷰 · 리팩토링 | 10장 | 팀 코드 리뷰 후 고른 차별점 | — | 팀 대화 |

페이즈가 끝날 때 실무 도구와 비교해 봅니다 (학습용, 2026-11-23 추가).

| 페이즈 끝 | 비교 |
|---|---|
| 3 | nginx로 같은 `www/`를 띄워 응답 헤더 비교 |
| 4 | 우리 스레드 수 ↔ Tomcat `maxThreads` ↔ nginx `worker_connections`가 각각 무엇을 제한하나 |
| 5 | `conf/server.conf`와 같은 구성을 nginx 설정으로 쓰고, 줄마다 우리 함수와 짝짓기 |
| 6 | wrk로 우리 서버와 nginx 처리량 비교, 차이의 원인 찾기 |

### 스텝 1. 버퍼, 소켓 헬퍼, 최소 서버

목표: HTTP 파싱 없이 "소켓 준비 → accept → 헤더 끝까지 누적 → 고정 응답 → close" 전체 흐름을 돌립니다.

**common/buf.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `void buf_init(struct buf *b)` | 빈 버퍼로 초기화한다 | server/main.c |
| `int buf_append(struct buf *b, const char *data, size_t n)` | 뒤에 n바이트를 붙이고, 공간이 모자라면 2배로 늘린다. 성공 0, 실패 -1 | server/main.c |
| `void buf_consume(struct buf *b, size_t n)` | 앞에서 n바이트를 지우고 나머지를 앞으로 당긴다 | 스텝 5부터 conn.c |
| `void buf_free(struct buf *b)` | 메모리를 해제하고 빈 상태로 되돌린다 | server/main.c |

**common/net.h** (스텝 1에서 만드는 함수)

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `int net_listen(uint16_t port, int backlog)` | `socket` → `SO_REUSEADDR` → `bind` → `listen`을 하고 리스닝 fd를 반환한다. 실패 -1 | server/main.c |
| `int net_accept(int lfd, char *ip, size_t ip_len)` | 연결 하나를 받아 fd를 반환하고, 클라이언트 IP를 `ip`에 적는다. `EINTR`이면 다시 시도한다 | server/main.c |
| `ssize_t net_read(int fd, void *buf, size_t n)` | `read()`를 감싼다. `EINTR`이면 다시 시도한다. 반환값 의미는 `read()`와 같다 | server/main.c |
| `int net_write_all(int fd, const void *data, size_t n)` | 부분 쓰기가 나도 n바이트를 다 보낼 때까지 반복한다. 성공 0, 실패 -1 | server/main.c |

**server/main.c** (임시. 스텝 2~5에서 교체)

| 함수 | 한 줄 설명 |
|---|---|
| `int main(int argc, char **argv)` | 포트 인자를 읽고, 리스닝 소켓을 연 뒤 "accept → 처리 → close"를 반복한다 |
| `static void handle_client(int fd)` | 헤더 끝까지 읽은 뒤 고정 응답 "hello"를 보낸다 |
| `static long find_header_end(const struct buf *b)` | 버퍼에서 `\r\n\r\n`을 찾아 헤더 길이를 반환한다. 없으면 -1. 스텝 2에서 파서로 교체 |

**Makefile**: `make`로 `build/server`를 만듭니다. `make asan`은 `-fsanitize=address,undefined -g`로 `build/asan/`에 따로 빌드합니다 (스텝 2부터는 테스트까지 실행). 스텝 1 서버는 Ctrl+C로 끝나므로 누수 보고는 나오지 않고, 범위 밖 접근만 잡힙니다. (Q1, 2026-11-23)

**완료 확인**
- `./build/server 8080` 실행 후 `curl -v localhost:8080/` → `hello`
- 쪼개서 보내도 응답이 오는지: `(printf 'GET / HTTP/1.1\r\nHo'; sleep 1; printf 'st: x\r\n\r\n') | nc localhost 8080`

### 스텝 2. HTTP 파서

목표: 바이트를 `struct http_message`로 바꾸는 증분 파서를 만들고, 소켓 없이 단위 테스트합니다.

**common/http_parser.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `void http_parser_init(struct http_parser *p, enum parse_mode mode)` | 요청 모드 또는 응답 모드로 파서를 초기화한다 | conn.c, proxy.c, backend |
| `enum parse_result http_parse(struct http_parser *p, const char *buf, size_t len)` | 누적된 바이트를 지난번 위치부터 이어서 해석한다. `NEED_MORE` / `DONE` / `ERROR` 반환 | conn.c, proxy.c, backend |
| `const char *http_header_get(const struct http_message *m, const char *name)` | 헤더 이름으로 값을 찾는다. 대소문자 무시. 없으면 NULL | static_handler, proxy, backend |
| `static long find_crlf(const char *buf, size_t from, size_t len)` | `from`부터 `\r\n` 위치를 찾는다. 없으면 -1, `\r`이나 `\n`이 따로 나오면 -2 (Q2 지도 Q2) | 내부 |
| `static enum parse_result fail(struct http_parser *p, int status)` | 오류 상태로 바꾸고 상태 코드를 기록한다 | 내부 (구현 중 추가) |
| `static int parse_version(const char *s, size_t n, int *minor)` | `HTTP/x.y`를 검사한다. 성공 0, 형식 오류 400, 지원 안 하는 버전 505 | 내부 (구현 중 추가. 요청 줄 · 상태 줄 공용) |
| `static int has_token(const char *list, const char *token)` | 쉼표 목록에 토큰이 있는지 본다 | 내부 (구현 중 추가) |
| `static int end_of_headers(struct http_parser *p, size_t header_len)` | 빈 줄에서 Host 필수 · 본문 길이 · keep-alive를 정한다 | 내부 (구현 중 추가) |
| `static enum parse_result pending_limit(struct http_parser *p, size_t len)` | 줄 끝이 아직 없을 때 상한(414 / 431)을 넘었는지 본다 | 내부 (구현 중 추가) |
| `is_ows` · `is_method_token` · `is_visible` · `is_field_value` · `trim_ows` (static) | 글자 판별과 앞뒤 공백 다듬기 | 내부 (페이즈 2 점검 단순화) |
| `static int parse_request_line(struct http_message *m, const char *line, size_t n)` | 메서드, 요청 대상, 버전을 꺼낸다. 성공 0, 실패 시 상태 코드(400/414/501/505) | 내부 |
| `static int parse_status_line(struct http_message *m, const char *line, size_t n)` | 응답 모드에서 버전과 상태 코드를 꺼낸다. 성공 0, 실패 400 | 내부 |
| `static int parse_header_line(struct http_message *m, const char *line, size_t n)` | `이름: 값`을 나눠 헤더 배열에 넣는다. 성공 0, 실패 시 상태 코드(400/431) | 내부 |
| `static int parse_content_length(const char *s, long *out)` | 숫자만 허용하고 오버플로를 막으며 정수로 바꾼다. 성공 0, 실패 -1 | 내부 |
| `static int resolve_body_length(struct http_parser *p)` | `Content-Length`와 `Transfer-Encoding` 규칙으로 본문 길이를 정한다. 성공 0, 실패 시 상태 코드(400/411/413/501) | 내부 |
| `static void decide_keep_alive(struct http_message *m)` | 버전과 `Connection` 헤더로 연결 유지 여부를 정한다 | 내부 |

규칙 요약:
- HTTP/1.1이고 `Host`가 없으면 400
- 헤더 이름과 콜론 사이에 공백이 있으면 400
- `Content-Length`와 `Transfer-Encoding`이 둘 다 있으면 400, `Transfer-Encoding`만 있으면 501
- 응답 모드에서 `Content-Length`가 없으면 `content_length = -1`로 두고 헤더까지만 읽은 뒤 `DONE` 반환 (본문은 연결 종료까지)

**tests/test.h, tests/test_http_parser.c**

| 함수 / 매크로 | 한 줄 설명 |
|---|---|
| `CHECK(cond)` | 조건이 거짓이면 파일·줄 번호와 함께 실패를 기록한다 |
| `CHECK_INT(a, b)`, `CHECK_STR(a, b)` | 두 값이 같은지 확인하고, 다르면 두 값을 출력한다 |
| `RUN(test_fn)` | 테스트 함수를 실행하고 통과/실패 수를 센다 |

테스트 케이스: 단순 GET, 1바이트씩 쪼개 넣기, POST 본문, 파이프라이닝(요청 두 개가 붙어 옴), 400 / 414 / 431 / 501 / 505 / 413, `Content-Length` + `Transfer-Encoding` 동시, 응답 모드.

**완료 확인**: `make test` 통과. 스텝 1의 `find_header_end()`를 `http_parse()`로 교체.

### 스텝 3. 응답 생성, MIME, 정적 파일

목표: 요청 경로에 맞는 파일을 찾아 올바른 헤더와 함께 보냅니다. 오류는 정확한 상태 코드로 응답합니다.

**common/http_response.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `const char *http_reason(int status)` | 상태 코드에 맞는 사유구를 반환한다 (200 → "OK") | 내부, backend |
| `int http_build_header(struct buf *out, int status, const char *content_type, long content_length, int keep_alive, const char *extra)` | 상태 라인과 표준 헤더(Date, Server, Content-Type, Content-Length, Connection)를 `out`에 붙인다. `extra`는 추가 헤더 줄(없으면 NULL) | static_handler, proxy, backend |
| `int http_build_error(struct buf *out, int status, int keep_alive)` | 짧은 HTML 본문을 포함한 오류 응답 전체를 `out`에 붙인다. 405면 `Allow` 헤더를 넣는다 | conn, static_handler, proxy |

**server/mime.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `const char *mime_from_path(const char *path)` | 확장자로 Content-Type을 정한다. 모르면 `application/octet-stream` | static_handler |

**server/handler.h**: `struct request_ctx`와 `handler_fn` 정의 (6장 참고). 함수 없음. 스텝 3의 `request_ctx` 필드: fd, ip, msg, raw, raw_len, root, keep_alive (라우트 · 설정 포인터는 스텝 4에서 추가, Q3 지도 Q9).

**server/static_handler.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `int static_handle(struct request_ctx *ctx)` | 핸들러 진입점. 메서드 확인 → 경로 변환 → 파일 열기 → 헤더 전송 → 본문 전송 | router가 고른 뒤 conn이 호출 |
| `int static_resolve_path(const char *root, const char *target, char *out, size_t out_len)` | 요청 경로를 실제 파일 경로로 바꾼다. 문자열 정규화 뒤 `realpath`로 링크를 풀고, 결과가 루트와 같거나 `루트/`로 시작하는지 확인한다 (Q3 지도 Q1). 성공 0, 실패 시 상태 코드(400/403/404) | static_handle, 테스트 |
| `static int percent_decode(const char *in, size_t n, char *out, size_t out_len)` | `%2e` 같은 인코딩을 푼다. `%00`이나 잘못된 인코딩이면 -1 | 내부 |
| `static int normalize_path(char *path)` | `.`과 `..`을 정리한다. 루트 위로 올라가려 하면 -1 | 내부 |
| `static int open_file(char *path, size_t cap, int slash, int *fd, off_t *size)` | 파일을 열고 크기를 얻는다. 디렉토리면 `index.html`을 붙인다 (끝에 `/`가 없으면 301, Q3 지도 Q2). 마지막 조각은 `O_NOFOLLOW`로 연다. 성공 0, 실패 시 상태 코드(301/403/404/500) | 내부 (구현 중 시그니처 변경) |
| `static int send_file_body(int sock, int file_fd, off_t size)` | 파일을 64KB씩 읽어 소켓으로 보낸다. 성공 0, 실패 -1 | 내부 |
| `static int hex_value(char c)` · `static int send_built(int fd, struct buf *out, int build_rc)` | 16진수 한 글자 값 · 만든 응답 보내고 해제 | 내부 (구현 중 추가) |
| `static int appendf(struct buf *out, const char *fmt, ...)` (http_response.c) | 형식 문자열 한 줄을 잘림 검사와 함께 붙인다 | 내부 (구현 중 추가) |

정적 파일에는 GET과 HEAD만 허용합니다(그 외 405). HEAD면 헤더까지만 보냅니다.

**tests/test_mime.c, tests/test_static_path.c**: 확장자별 MIME, 경로 탐색 공격(`/../`, `/%2e%2e/`, `%00`, 이중 슬래시).

**완료 확인**: `www/`에 html, css, js, png, jpg를 두고 `curl -v`로 Content-Type과 Content-Length 확인. 없는 파일 404, 권한 없는 파일과 `/../etc/passwd` 403, 정적 경로에 POST 405.

### 스텝 4. 설정과 라우터

목표: 포트, 루트, 라우팅 규칙을 설정 파일과 CLI 인자로 받고, 경로에 따라 핸들러를 고릅니다.

**server/config.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `void config_defaults(struct server_config *c)` | 5장의 기본값으로 채운다 | server/main.c |
| `int config_load_file(struct server_config *c, const char *path, char *err, size_t err_len)` | 설정 파일을 한 줄씩 읽어 값을 채운다. 실패하면 줄 번호와 이유를 `err`에 적고 -1 | server/main.c |
| `int config_apply_args(struct server_config *c, int argc, char **argv, char *err, size_t err_len)` | `-p 포트`, `-r 루트`, `-c 설정파일`, `-t 스레드수`로 값을 덮어쓴다 | server/main.c |
| `static int parse_route_line(struct server_config *c, char *line)` | `route <prefix> static` 또는 `route <prefix> proxy <host:port>...`를 해석한다 | 내부 |
| `static int parse_backend(const char *s, struct backend *out)` | `host:port`를 나눈다 | 내부 |

**server/router.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `const struct route *router_match(const struct server_config *c, const char *target)` | 요청 경로와 가장 길게 일치하는 라우트를 반환한다. 없으면 NULL | conn.c, 테스트 |
| `handler_fn router_handler(const struct route *r)` | 라우트 종류에 맞는 핸들러 함수를 반환한다 (정적 → `static_handle`, 프록시 → `proxy_handle`) | conn.c |

스텝 4 시점에는 `proxy_handle`이 아직 없으므로, 502를 돌려주는 임시 함수로 둡니다. 스텝 7에서 교체합니다.

**tests/test_router.c**: 가장 긴 prefix 우선, 일치하는 라우트 없음, `/api`와 `/api/`와 `/apix` 구분.

**완료 확인**: `./build/server -c conf/server.conf`로 실행. `-p 9090`으로 포트 덮어쓰기. `/api/`는 임시 502, 나머지는 정적 파일.

### 스텝 5. 스레드 풀과 연결 수명 (D1 결정 후)

목표: 여러 연결을 동시에 처리하고, 한 연결에서 여러 요청을 처리합니다(keep-alive). 느린 클라이언트는 타임아웃으로 끊습니다.

**server/thread_pool.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `struct thread_pool *pool_create(int nthreads, int queue_cap, pool_work_fn work, void *arg)` | 워커 스레드를 만들고 빈 작업 큐를 준비한다. 실패 NULL | server/main.c |
| `int pool_submit(struct thread_pool *p, int fd, const char *ip)` | 작업 큐에 연결을 넣고 워커 하나를 깨운다. 큐가 가득 차면 -1 (호출한 쪽이 503 응답) | server/main.c |
| `void pool_shutdown(struct thread_pool *p)` | 새 작업을 막고, 남은 작업을 처리한 뒤 모든 워커를 종료·회수하고 해제한다 | server/main.c |
| `static void *worker_main(void *arg)` | 큐에서 작업을 꺼내 `work` 함수를 실행하는 것을 반복한다 | 내부 (스레드 시작 함수) |

`pool_work_fn`의 형태: `void (*)(int fd, const char *ip, void *arg)`

**server/conn.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `struct conn *conn_new(int fd, const char *ip, const struct server_config *cfg)` | 연결 객체를 만들고 소켓 타임아웃을 설정한다. 실패 NULL | server/main.c의 작업 함수 |
| `void conn_serve(struct conn *c)` | 연결이 끝날 때까지 "요청 읽기 → 처리 → keep-alive 판단"을 반복한다 | server/main.c의 작업 함수 |
| `void conn_free(struct conn *c)` | fd를 닫고 버퍼와 객체 메모리를 해제한다 | server/main.c의 작업 함수 |
| `static int conn_read_request(struct conn *c)` | 요청 하나가 완성될 때까지 읽고 파싱한다. 완성 / 파싱 오류 / 클라이언트 종료 / 시간 초과 중 하나 반환 | 내부 |
| `static int conn_dispatch(struct conn *c)` | `request_ctx`를 채우고 라우터가 고른 핸들러를 호출한다 | 내부 |
| `static void conn_send_error(struct conn *c, int status)` | 오류 응답을 만들어 보낸다 | 내부 |
| `static int conn_should_continue(struct conn *c, int handler_rc)` | 핸들러 결과, keep-alive 여부, 처리 요청 수로 연결 유지 여부를 정한다 | 내부 |

`conn_serve()` 반복 한 번의 흐름:
1. `conn_read_request()`. 시간 초과면 408을 보내고 종료. 클라이언트가 끊었으면 그냥 종료.
2. 파싱 오류면 해당 상태 코드로 응답하고 종료 (요청 경계를 잃었으므로 keep-alive여도 닫음).
3. `conn_dispatch()`.
4. `buf_consume(total_len)`으로 처리한 요청을 버퍼에서 지우고, 파서를 다시 초기화.
5. `conn_should_continue()`가 참이면 1로. 버퍼에 남은 바이트(파이프라이닝)가 있으면 그것부터 파싱.

**server/main.c** (갱신): `on_connection()` 작업 함수 = `conn_new` → `conn_serve` → `conn_free`.

**완료 확인**
- `curl -v localhost:8080/a.html localhost:8080/b.css` → 같은 연결 재사용 (`Re-using existing connection` 출력)
- `nc localhost 8080`으로 연결만 열어 두고 5초 뒤 서버가 끊는지 확인
- 동시 요청: `ab -n 1000 -c 50 http://127.0.0.1:8080/index.html`

### 스텝 6. 테스트용 백엔드

목표: 리버스 프록시가 요청을 넘길 대상을 우리 코드로 만듭니다. `common/`만 사용합니다.

**backend/main.c**

| 함수 | 한 줄 설명 |
|---|---|
| `int main(int argc, char **argv)` | 포트 인자로 리스닝하고, 연결을 하나씩 받아 처리한다 (단일 스레드) |
| `static void handle_conn(int fd, uint16_t port)` | 요청 하나를 읽고 파싱한 뒤 경로에 맞는 응답을 보내고 닫는다 |
| `static void respond(int fd, int status, const char *type, const char *body, size_t len)` | 헤더와 본문을 조립해 보낸다 |

| 경로 | 동작 |
|---|---|
| `GET /api/time` | `{"port":8001,"time":"..."}`. 응답에 자기 포트를 넣어 Round-Robin을 눈으로 확인 |
| `POST /api/echo` | 받은 본문을 그대로 돌려줌 |
| `GET /api/slow` | 5초 기다린 뒤 응답 (504 테스트용) |
| 그 외 | 404 |

**common/net.h에 추가**: 없음 (스텝 1의 함수로 충분).

**완료 확인**: `./build/backend 8001` 실행 후 `curl localhost:8001/api/time`, `curl -d hello localhost:8001/api/echo`.

### 스텝 7. 리버스 프록시

목표: `/api/` 요청을 백엔드들에 번갈아 넘기고 응답을 그대로 돌려줍니다. 백엔드 실패는 502 / 504로 응답합니다.

**common/net.h에 추가**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `int net_connect(const char *host, uint16_t port, int timeout_ms)` | 제한 시간 안에 서버에 연결해 fd를 반환한다. 실패 -1 | proxy.c, 나중의 client |
| `int net_set_timeout(int fd, int recv_ms, int send_ms)` | 소켓의 읽기·쓰기 타임아웃을 설정한다 | conn.c(스텝 5), proxy.c |

**server/proxy.h**

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `int proxy_handle(struct request_ctx *ctx)` | 핸들러 진입점. 백엔드 선택 → 연결 → 요청 전달 → 응답 중계 | router가 고른 뒤 conn이 호출 |
| `static const struct backend *pick_backend(struct route *r)` | 원자적 카운터로 다음 백엔드를 고른다 (Round-Robin) | 내부 |
| `static int connect_backend(struct route *r, const struct server_config *cfg)` | 고른 백엔드에 연결하고, 실패하면 다음 백엔드로 한 번 재시도한다. 실패 -1 | 내부 |
| `static int build_upstream_request(const struct request_ctx *ctx, struct buf *out)` | 요청 라인 + 헤더(hop-by-hop 제거, `X-Forwarded-For` 추가, `Connection: close`) + 본문을 만든다 | 내부 |
| `static int relay_response(int backend_fd, struct request_ctx *ctx)` | 백엔드 응답을 읽어 클라이언트에 그대로 쓴다. 응답 헤더를 파싱해 keep-alive 여부를 정한다. 아무것도 받기 전에 시간이 초과되면 504 | 내부 |
| `static int is_hop_by_hop(const char *name)` | 전달하면 안 되는 헤더인지 판단한다 | 내부 |

**완료 확인**
- 백엔드 2개 실행 후 `for i in 1 2 3 4; do curl -s localhost:8080/api/time; done` → 포트가 8001, 8002 번갈아 나옴
- 백엔드 하나를 끄면 나머지로만 감. 둘 다 끄면 502
- `curl localhost:8080/api/slow` → 3초 뒤 504

### 스텝 8. 종료 처리, 엣지 케이스, 기능·부하 테스트

**server/main.c** (최종)

| 함수 | 한 줄 설명 |
|---|---|
| `int main(int argc, char **argv)` | 설정 → 시그널 → 리스닝 → 스레드 풀 → accept 루프 → 종료 정리 |
| `static void install_signals(void)` | SIGPIPE를 무시하고, SIGINT/SIGTERM에 종료 플래그 핸들러를 건다. `SA_RESTART`를 쓰지 않아 `accept()`가 깨어나게 한다 |
| `static void on_signal(int sig)` | 원자적 종료 플래그만 세운다 (async-signal-safe) |
| `static void accept_loop(int lfd, struct thread_pool *pool)` | 종료 플래그가 설 때까지 연결을 받아 풀에 넣는다. 큐가 가득 차면 503 응답 후 닫는다 |
| `static void on_connection(int fd, const char *ip, void *arg)` | 워커가 실행하는 작업: `conn_new` → `conn_serve` → `conn_free` |

**tests/functional.sh**: 서버와 백엔드를 띄우고 curl로 8장 대응표의 항목을 확인합니다.

**누수 확인**: 시그널 처리가 생겼으므로 `build/asan/server`를 Ctrl+C로 정상 종료시켜 서버 전체 누수를 처음으로 확인합니다 (`make asan`은 스텝 1에서 만듦).

**완료 확인**
- Ctrl+C로 종료 시 진행 중 요청이 끝난 뒤 종료, ASan 누수 보고 0
- 응답 도중 클라이언트가 끊어도 서버가 죽지 않음
- `wrk -t4 -c100 -d10s http://127.0.0.1:8080/index.html` 결과 기록 (스레드 수 4 / 8 / 16 비교)

---

## 8. 엣지 케이스 대응표

| 명세서 항목 | 상황 | 처리 | 담당 |
|---|---|---|---|
| 잘못된 HTTP 요청 | 요청 라인 형식 오류, Host 누락, 헤더 이름 뒤 공백 | 400 + 연결 닫기 | http_parser |
| | 메서드 이름 형식 오류 (대문자 토큰 아님) | 400. 16자 초과는 501 | http_parser |
| | 형식은 맞지만 처리하지 않는 메서드 | 정적 경로는 405 (GET · HEAD만), 프록시 경로는 백엔드로 그대로 전달 | static_handler · proxy (Q2 지도 Q1, 2026-11-23) |
| | 지원하지 않는 버전 | 505 | http_parser |
| | 불완전한 헤더 후 무응답 | 10초 뒤 408 | conn |
| 매우 큰 요청 | 본문 1MB 초과 | 413 | http_parser |
| | URI 8KB 초과 | 414 | http_parser |
| | 헤더 8KB 초과 | 431 | http_parser |
| 백엔드 문제 | 연결 거부 | 다음 백엔드 재시도, 모두 실패하면 502 | proxy |
| | 응답 지연 | 3초 뒤 504 | proxy |
| 클라이언트 비정상 종료 | 요청 도중 끊김 | `read()` 0 또는 ECONNRESET → 조용히 정리 | conn |
| | 응답 도중 끊김 | SIGPIPE 무시 → `write()` EPIPE → 정리 | net, main |
| 보안 | 경로 탐색 (`..`, `%2e%2e`, `%00`) | 400 또는 403 | static_handler |
| | 요청 스머글링 (CL + TE) | 400 | http_parser |
| 과부하 | 작업 큐 가득 참 | 503 | main |

---

## 9. 테스트 전략

| 종류 | 대상 | 도구 | 스텝 |
|---|---|---|---|
| 단위 | http_parser, mime, router, static_resolve_path | tests/test.h | 2, 3, 4 |
| 기능 | 정적 파일, 오류 코드, keep-alive, 프록시, 502/504 | curl, nc, tests/functional.sh | 각 스텝 완료 확인, 8 |
| 동시성 | 동시 접속 정상 동작 | ab | 5 |
| 부하 | 동시 연결 수별 처리량 | wrk | 8 |
| 메모리 | 누수, 범위 밖 접근 | `make asan`, valgrind | 매 스텝 |

---

## 10. 리팩토링 학습 로드맵

기본 구현이 끝난 뒤 진행합니다. 각 항목은 설계 질문 → 구현 → 비교 순서로 합니다.

| 순서 | 주제 | 바뀌는 범위 | 학습 포인트 |
|---|---|---|---|
| R1 | 스레드 풀 → epoll 이벤트 루프 | thread_pool → event_loop, conn, proxy | 논블로킹 I/O, 연결 상태 머신, C10K. `wrk`로 전후 비교 |
| R2 | 클라이언트 추가 (`client/`) | common 재사용 | `net_connect` + 응답 파서 재사용 |
| R3 | 백엔드 연결 재사용 (커넥션 풀) | proxy | 매 요청 handshake 비용 제거 |
| R4 | 포워드 프록시 + 캐시 | router, proxy | absolute-form URI, 캐시 정책 |
| R5 | chunked 인코딩 지원 | http_parser | 본문 길이를 모르는 메시지 처리 |
