# 스텝 2 읽기 가이드 — HTTP 파서

페이즈 2 (이해) · 이슈 #2 · 2026-11-23

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `common/http_parser.h` | 40 | 새로 만듦. 상한 상수, `struct http_message`, `struct http_parser` |
| `common/http_parser.c` | 268 | 새로 만듦. 함수 13개 (중괄호만 있는 줄 45) |
| `server/main.c` | 97 | `find_header_end` · 431 고정 응답을 지우고 파서로 교체, `send_result` 추가 |
| `Makefile` | 41 | `make test`, `make asan`이 테스트까지 빌드 · 실행 |
| `tests/test.h` | 42 | 새로 만듦. `CHECK` · `CHECK_INT` · `CHECK_STR` · `RUN` · `TEST_EXIT` |
| `tests/test_http_parser.c` | 135 | 새로 만듦. 테스트 8개 |

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `http_parser_init` | 요청 · 응답 모드로 초기화 | main.c `handle_client`, 테스트 |
| `http_parse` | 쌓인 바이트를 멈춘 줄부터 이어서 해석. NEED_MORE / DONE / ERROR | main.c `handle_client`, 테스트 |
| `http_header_get` | 헤더 이름으로 값 찾기 (대소문자 무시) | `decide_keep_alive`, `end_of_headers`, 테스트 |
| `find_crlf` (static) | `\r\n` 위치. 없으면 -1, 따로 나온 `\r` · `\n`이면 -2 | `http_parse` |
| `pending_limit` (static) | 줄 끝을 기다리는 중 상한 초과 검사 → 414 / 431 | `http_parse` |
| `parse_request_line` (static) | 메서드 · 대상 · 버전 | `http_parse` |
| `parse_status_line` (static) | 응답 모드의 버전 · 상태 코드 | `http_parse` |
| `parse_version` (static) | `HTTP/x.y` 검사 → 400 / 505 | `parse_request_line`, `parse_status_line` |
| `parse_header_line` (static) | `이름: 값`을 헤더 배열에 복사 | `http_parse` |
| `end_of_headers` (static) | 빈 줄에서 Host · 본문 길이 · keep-alive 결정 | `http_parse` |
| `resolve_body_length` (static) | CL · TE 규칙으로 본문 길이 → 400 / 411 / 413 / 501 | `end_of_headers` |
| `parse_content_length` (static) | 숫자만, 넘침 없이 정수로 | `resolve_body_length` |
| `decide_keep_alive` (static) | 버전 + Connection 헤더 | `end_of_headers` |
| `has_token` (static) | 쉼표 목록에서 토큰 찾기 | `decide_keep_alive` |
| `fail` (static) | 오류 상태로 바꾸고 상태 코드 기록 | 여러 곳 |
| `send_result` (main.c, static) | DONE이면 `hello 메서드 경로`, ERROR면 상태 코드 응답 | `handle_client` |

## 3. 요청 하나 기준 호출 순서

`POST /api/echo` + 본문 5바이트가 두 조각으로 온 경우:

1. `handle_client`가 `http_parser_init(&p, PARSE_REQUEST)`로 파서를 준비합니다.
2. 첫 `net_read`가 `"POST /api/echo HTTP/1.1\r\nHost: x\r\nConte"`를 받아 `buf`에 쌓습니다.
3. `http_parse(&p, buf, len)`:
   - `find_crlf` → 요청 줄 끝 발견 → `parse_request_line` (메서드 "POST", 대상 "/api/echo", 버전 1.1) → 상태 = 헤더
   - `find_crlf` → `Host: x` 줄 → `parse_header_line`
   - `find_crlf` → `Conte`까지만 있고 줄 끝 없음 → `pending_limit` → **NEED_MORE**. `pos`는 `Conte`의 시작에 멈춰 있음
4. 다음 `net_read`가 `"nt-Length: 5\r\n\r\nhello"`를 받아 이어 붙입니다.
5. `http_parse`가 `pos`부터 이어서:
   - `Content-Length: 5` 줄 → `parse_header_line`
   - 빈 줄 → `end_of_headers` → Host 확인 → `resolve_body_length` (5) → `decide_keep_alive` (1.1이라 유지) → 상태 = 본문
   - 헤더 뒤에 5바이트가 다 있음 → `total_len` 계산 → **DONE**
6. `send_result`가 `hello POST /api/echo`를 보내고, `main`이 연결을 닫습니다.

흐름 지도로는 ① → ④ → ⑤ → ⑨입니다. ⑤가 새로 생겼습니다.

## 4. 추천 읽기 순서

1. `common/http_parser.h` — 구조체 세 개와 상한 상수. 무엇을 채우는지 먼저 봅니다
2. `http_parse` — 상태 기계의 뼈대. while 한 바퀴 = 줄 하나
3. `find_crlf` → `pending_limit` — "덜 왔다"와 "너무 크다"를 가르는 곳
4. `parse_request_line` → `parse_version` — 첫 줄 나누기
5. `parse_header_line` — 공백 다듬기, 고정 배열에 복사하는 이유
6. `end_of_headers` → `resolve_body_length` → `decide_keep_alive` — 빈 줄에서 내리는 결정들
7. `tests/test_http_parser.c` `test_byte_by_byte` · `test_errors` — 무엇을 보장하는지 테스트로 확인

## 5. 직접 실행해 볼 명령

컨테이너 안에서:
```
make test                                     # 테스트 8개
make asan                                     # 같은 테스트를 ASan으로
./build/server 8080                           # 터미널 1
curl -s localhost:8080/a/b.html?x=1           # hello GET /a/b.html?x=1
curl -s -d hello localhost:8080/api/echo      # hello POST /api/echo
curl -s -X DELETE localhost:8080/api/x        # hello DELETE /api/x (Q1: 파서는 통과시킴)
printf 'GET / HTTP/1.1\r\n\r\n' | nc localhost 8080          # 400 (Host 없음)
printf 'GET / HTTP/1.1\nHost: x\n\n' | nc localhost 8080     # 400 (\n만, Q2)
printf 'POST / HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080   # 411
```

본문 쪼개 보내기 (파서가 본문 길이를 기다리는지):
```
(printf 'POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nab'; sleep 1; printf 'cd') | nc localhost 8080
```

## 6. 확인 질문

`struct http_parser`는 버퍼 안의 위치를 포인터가 아니라 `pos`(메시지 시작부터의 거리)로 기억합니다. 만약 포인터로 기억했다면, 요청이 여러 조각으로 오는 상황에서 어떤 문제가 생길까요? (힌트: `buf_append`가 공간이 모자랄 때 하는 일)

## 알려진 한계

- 상한 초과(414 · 431)로 바로 닫으면, 손님이 보낸 바이트가 덜 읽힌 채라 커널이 RST를 보내고 손님은 응답을 못 볼 수 있습니다. 20번 중 약 절반 재현됨 → 스텝 5 `conn`에서 lingering close(쓰기만 닫고 남은 입력을 버린 뒤 닫기)로 해결 예정
- 오류 응답의 사유구가 모두 `Error`입니다 → 스텝 3 `http_reason`
- 연결 하나에 요청 하나만 처리합니다 (파이프라이닝은 파서만 지원) → 스텝 5
