# 스텝 4 읽기 가이드 — 설정과 라우터

페이즈 3 (정적 웹서버) 뒷부분 · 이슈 #4 · 2026-11-23

## 1. 추가 · 변경된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 변경 |
|---|---|---|
| `server/config.h` · `.c` | 35 · 181 | 새로 만듦. 기본값, 설정 파일, CLI 인자, 라우트 · 백엔드 검사 |
| `server/router.h` · `.c` | 7 · 27 | 새로 만듦. 가장 긴 prefix (경로 조각 단위), 종류별 핸들러 |
| `server/proxy.h` · `.c` | 5 · 14 | 새로 만듦. 502만 보내는 임시 `proxy_handle` (스텝 7에서 채움) |
| `server/handler.h` | +4 −1 | `request_ctx`에 `route` · `cfg` |
| `server/main.c` | +38 −33 (97줄 그대로) | 포트 인자 검사를 지우고 `config` · `router` 사용, 루트 확인 |
| `Makefile` | +2 −1 | config · router · proxy를 링크 |
| `conf/server.conf` | — | 설정 예시 |
| `tests/test_router.c` | 95 | 새로 만듦. 테스트 5개 |

새 코드 합계 약 273줄.

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `config_defaults` | 기본값 + 라우트 `/ static` | main, 테스트 |
| `config_apply_args` | `-c`를 먼저 읽고 `-p` `-r` `-t`로 덮음 | main, 테스트 |
| `config_load_file` | 줄마다 `apply_line`, 오류면 `파일:줄: 이유` | `config_apply_args`, 테스트 |
| `apply_line` (static) | 주석 · 빈 줄 건너뜀, `route`면 `parse_route_line`, 아니면 `apply_key` | `config_load_file` |
| `apply_key` (static) | 키 표에서 찾아 범위 검사 후 적용 (파일 · CLI 공용) | `apply_line`, `config_apply_args` |
| `parse_route_line` (static) | prefix · 종류 · 백엔드 검사 후 라우트 추가 | `apply_line`, `config_defaults`, `config_load_file` |
| `parse_backend` (static) | `IPv4:포트` 나누고 `inet_pton`으로 검사 | `parse_route_line` |
| `parse_int` (static) | 범위 있는 정수 | `apply_key`, `parse_backend` |
| `router_match` | `?` 앞 경로와 가장 길게 맞는 라우트 (경로 조각 단위) | main.c `dispatch`, 테스트 |
| `router_handler` | 정적 → `static_handle`, 프록시 → `proxy_handle` | main.c `dispatch` |
| `proxy_handle` | 지금은 502만 보냄 | `router_handler`가 돌려준 포인터로 |
| `dispatch` (main.c) | 라우트 고르기 → ctx 채우기 → 핸들러 호출. 라우트 없으면 404 | `handle_client` |

## 3. 요청 하나 기준 호출 순서

`./build/server -p 9090 -c conf/server.conf` 후 `GET /api/time`:

1. `main` → `config_defaults` (8080, ./www, 라우트 `/ static`)
2. `config_apply_args`
   - 1차: `-c conf/server.conf`를 찾아 `config_load_file` → 줄마다 `apply_line`
     - `port 8080` → `apply_key` → `parse_int` → port = 8080
     - `route /api/ proxy 127.0.0.1:8001 127.0.0.1:8002` → `parse_route_line` → `parse_backend` × 2
     - `route / static` → 라우트 추가
   - 2차: `-p 9090` → `apply_key("port")` → port = 9090 (파일보다 나중이라 CLI가 이김, Q14)
3. `stat(root)` → 폴더 확인 (Q16) → `net_listen(9090)`
4. 손님 접속 → `handle_client` → `http_parse` DONE
5. `dispatch` → `router_match(cfg, "/api/time")`
   - `/api/`: 앞 5글자 같음, prefix가 `/`로 끝남 → 후보 (길이 5)
   - `/`: 같음 → 후보 (길이 1)
   - 가장 긴 `/api/` 선택
6. `router_handler(/api/)` → `proxy_handle` → ctx를 넘겨 호출 → 502
7. `/index.html`이었다면 5에서 `/`만 맞아 `static_handle` → 스텝 3과 같은 길

흐름 지도로는 ⑤ 다음에 ⑥(라우터)이 새로 생겨 ⑦ 정적 / ⑦ 프록시(임시)로 갈라집니다.

## 4. 추천 읽기 순서

1. `conf/server.conf` — 무엇을 설정할 수 있나
2. `server/config.h` — `struct route` · `struct server_config` 모양
3. `config_apply_args` — 왜 두 번 도는가 (Q14)
4. `config_load_file` → `apply_line` → `apply_key` — 표 하나로 파일 · CLI 검사를 같이 쓰는 방법
5. `parse_route_line` → `parse_backend` — 라우트를 받기 전에 거르는 것들
6. `router_match` — 가장 긴 prefix + 경로 조각 조건 한 줄 (Q13)
7. main.c `dispatch` — 라우터와 핸들러가 만나는 곳

## 5. 직접 실행해 볼 명령

컨테이너 안 `/work`에서:
```
make test && make asan
./build/server -c conf/server.conf                         # 터미널 1
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/api/time    # 502 (임시)
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/apix        # 404 (정적으로 감, Q13)
curl -s -o /dev/null -w "%{http_code}\n" localhost:8080/            # 200
./build/server -p 9090 -c conf/server.conf                 # 9090에서 뜸 (Q14)
./build/server                                             # 설정 없음: / static만 (Q15)
printf 'port 8080\nporto 1\n' > /tmp/bad.conf && ./build/server -c /tmp/bad.conf   # 줄 번호와 이유
./build/server -r /nope                                    # 루트 없음 → 바로 종료 (Q16)
```

포트를 인자로만 주던 이전 방식(`./build/server 8080`)은 이제 "모르는 인자"로 거부됩니다. `-p 8080`을 씁니다.

## 6. 확인 질문

`dispatch`는 라우터가 돌려준 함수 포인터를 부를 뿐, 그게 정적 핸들러인지 프록시인지 모릅니다. 스텝 7에서 진짜 프록시를 넣을 때, `main.c` · `router.c` · `static_handler.c` 중 어느 파일이 바뀌고 어느 파일은 바뀌지 않을까요? 왜 그럴까요?

## 알려진 한계

- `router_match`가 `const` 라우트를 돌려줘서, 스텝 7 프록시가 Round-Robin 카운터를 올리려면 카운터 위치를 다시 정해야 합니다 (design.md 스텝 4 끝에 적어 둠)
- 설정은 시작할 때 한 번만 읽습니다 (다시 읽기 · SIGHUP 없음)
- `-t` 스레드 수는 읽기만 하고 스텝 5부터 씁니다
