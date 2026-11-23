# 스텝 3 읽기 가이드 — 응답 생성, MIME, 정적 파일

페이즈 3 (정적 웹서버) 앞부분 · 이슈 #3 · 2026-11-23

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `common/http_response.h` · `.c` | 8 · 72 | 새로 만듦. 사유구 표, 표준 헤더, 오류 응답 (HTML 본문) |
| `server/mime.h` · `.c` | 4 · 33 | 새로 만듦. 확장자 → Content-Type 표 |
| `server/handler.h` | 15 | 새로 만듦. `struct request_ctx`, `handler_fn` |
| `server/static_handler.h` · `.c` | 7 · 184 | 새로 만듦. 경로 변환 · 방어 · 파일 전송 |
| `server/main.c` | +19 −25 | `send_result` 지우고 `dispatch`가 `static_handle`을 부름 |
| `Makefile` | +6 −4 | `LIB_SRC`(main 뺀 모듈)로 서버 · 테스트를 함께 링크 |
| `www/` | — | index.html · style.css · app.js · img.png · photo.jpg · docs/ |
| `tests/test_mime.c` · `test_static_path.c` | 23 · 92 | 새로 만듦. 테스트 6개 |

새 코드 합계 약 320줄 (300줄 제한 초과).

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `http_reason` | 상태 코드 → 사유구 | `http_build_header`, `http_build_error` |
| `appendf` (static) | 형식 한 줄을 잘림 검사와 함께 붙임 | `http_build_header` |
| `http_build_header` | 상태 줄 + Date · Server · Content-Type · Content-Length · Connection + extra | static_handler, `http_build_error` |
| `http_build_error` | 오류 응답 전체 (HTML 본문, 405면 Allow) | static_handler, main.c `dispatch` |
| `mime_from_path` | 마지막 `/` 뒤 확장자 → 타입 | `static_handle`, 테스트 |
| `static_handle` | 메서드 확인 → 경로 변환 → 열기 → 헤더 · 본문 전송 | main.c `dispatch` (스텝 4부터 라우터) |
| `static_resolve_path` | 디코드 → 정규화 → 점 파일 → realpath → 루트 안인지 | `static_handle`, 테스트 |
| `percent_decode` (static) | `%XX` 풀기, `%00` · 잘못된 인코딩 거부 | `static_resolve_path` |
| `hex_value` (static) | 16진수 한 글자 값 | `percent_decode` |
| `normalize_path` (static) | `//` · `.` · `..` 정리, 루트 위로 가면 -1 | `static_resolve_path` |
| `open_file` (static) | stat → 디렉토리면 301 또는 index.html → `O_NOFOLLOW`로 열기 | `static_handle` |
| `send_file_body` (static) | 64KB씩 읽어 소켓으로 | `static_handle` |
| `send_built` (static) | 만든 응답 보내고 버퍼 해제 | `static_handle` |
| `dispatch` (main.c, static) | DONE이면 `static_handle`, ERROR면 `http_build_error` | `handle_client` |

## 3. 요청 하나 기준 호출 순서

`GET /docs` (끝에 `/` 없음)과 이어지는 `GET /docs/`:

1. `handle_client`가 읽고 `http_parse` → DONE.
2. `dispatch`가 `request_ctx`(fd, ip, msg, raw, root=`./www`, keep_alive=0)를 채워 `static_handle`을 부릅니다.
3. `static_handle`: 메서드가 GET이니 통과.
4. `static_resolve_path("./www", "/docs")`:
   - `?` 앞까지 `percent_decode` → `/docs`
   - `normalize_path` → `/docs` (`..` 없음)
   - `/.` 없음 → 점 파일 아님
   - `realpath("./www")` → `/work/www`, `realpath("/work/www/docs")` → `/work/www/docs`
   - `/work/www`로 시작하고 다음 글자가 `/` → 루트 안 → 0
5. `open_file`: `stat` → 디렉토리, 요청 끝에 `/` 없음 → **301**.
6. `static_handle`이 `Location: /docs/`를 담아 `http_build_header(301)` → `send_built`.
7. 브라우저가 `GET /docs/`를 다시 보냄 → 4번과 같고, `open_file`이 `/index.html`을 붙여 `O_NOFOLLOW`로 엶.
8. `http_build_header(200, text/html, 크기)` → `net_write_all` → `send_file_body`(64KB씩) → 파일 `close`.
9. main이 연결을 닫습니다.

흐름 지도로는 ① → ④ → ⑤ → ⑦ 정적 → ⑧ → ⑨입니다. ⑦ · ⑧이 새로 생겼습니다. ⑥(라우터)은 스텝 4.

## 4. 추천 읽기 순서

1. `server/handler.h` — 핸들러가 받는 것 (`request_ctx`)과 약속 (`handler_fn` 반환값)
2. `static_handle` — 처음부터 끝까지 흐름
3. `static_resolve_path` — 방어 4겹: 디코드 → 정규화 → 점 파일 → realpath 비교
4. `normalize_path` — `..` 하나에 조각 하나를 지우는 방법 (w 되감기)
5. `open_file` — 상태 코드를 어떻게 고르나, `O_NOFOLLOW`가 막는 것
6. `common/http_response.c` — 헤더 순서, 날짜 형식, `appendf`의 잘림 검사
7. `tests/test_static_path.c` `setup` — 링크 탈출 · 앞부분 일치 함정을 어떻게 만들었나

## 5. 직접 실행해 볼 명령

컨테이너 안에서:
```
make test && make asan
./build/server 8080                                     # 터미널 1 (/work에서)
curl -v localhost:8080/style.css                        # Content-Type: text/css
curl -sI localhost:8080/img.png                         # HEAD: 헤더만
curl -si localhost:8080/docs | head -5                  # 301 + Location: /docs/
curl -s --path-as-is localhost:8080/../etc/passwd       # 403
curl -si -X POST localhost:8080/ | head -6              # 405 + Allow: GET, HEAD
```

링크 탈출과 점 파일 (저장소를 더럽히지 않게 /tmp 복사본에서):
```
mkdir -p /tmp/site && cp -r /work/www /tmp/site/ && cd /tmp/site
ln -s /etc www/x && echo KEY=1 > www/.env
/work/build/server 8080 &
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/x/hostname    # 403 (Q1)
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/.env          # 404 (Q3)
```

Windows 브라우저: `http://localhost:8080/` — 제목이 파랗고 "JavaScript 실행됨"이 보이면 CSS · JS 타입이 맞는 것. `docs (끝에 / 없음)` 링크를 누르면 주소가 `/docs/`로 바뀌고 글자가 초록색.

## 6. 확인 질문

`static_resolve_path`는 문자열 정규화(`normalize_path`)로 `..`을 막은 뒤에도 `realpath`로 한 번 더 확인합니다. 반대로 `realpath`만 쓰고 문자열 정규화를 빼면 어떤 요청에서 결과가 달라질까요? (힌트: realpath는 파일이 없으면 실패한다)

## 알려진 한계

- HEAD 요청이 오류(404 등)를 받으면 오류 HTML 본문까지 보냅니다 (HEAD는 본문이 없어야 함) → 스텝 5 `conn_send_error`에서 정리
- `realpath` 확인과 `open` 사이에 링크를 바꿔치기하는 틈(TOCTOU)은 막지 않습니다 (Q1 범위 밖)
- 상한 초과 오류 뒤 바로 닫아 RST로 응답이 유실될 수 있는 문제는 그대로 → 스텝 5
