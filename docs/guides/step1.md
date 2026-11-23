# 스텝 1 읽기 가이드 — 버퍼, 소켓 헬퍼, 최소 서버

페이즈 1 (연결) · 이슈 #1 · 2026-11-23

## 1. 추가된 파일과 줄 수

빈 줄 · 주석을 뺀 줄 수입니다.

| 파일 | 줄 | 역할 |
|---|---|---|
| `common/buf.h` | 13 | 늘어나는 바이트 버퍼 선언 |
| `common/buf.c` | 46 | 버퍼 구현 |
| `common/net.h` | 10 | 소켓 헬퍼 선언 |
| `common/net.c` | 69 | listen · accept · read · write 감싸기 |
| `server/main.c` | 101 | 임시 서버 (스텝 2~5에서 교체) |
| `Makefile` | 26 | `make`, `make asan`, `make test`, `make clean` |
| 합계 | 265 | C 소스만 239 |

## 2. 함수 지도

| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `buf_init` | 빈 버퍼로 초기화 | main.c `handle_client`, `buf_free` |
| `buf_append` | 뒤에 n바이트를 붙이고, 모자라면 2배로 늘림 | main.c `handle_client` |
| `buf_consume` | 앞에서 n바이트를 지우고 당김 | 아직 없음 (스텝 5 `conn`) |
| `buf_free` | 해제하고 빈 상태로 | main.c `handle_client` |
| `net_listen` | socket → `SO_REUSEADDR` → bind → listen | main.c `main` |
| `net_accept` | 연결 하나를 받고 손님 IP를 적음. EINTR이면 다시 | main.c `main` |
| `net_read` | read + EINTR 다시 | main.c `handle_client` |
| `net_write_all` | 부분 쓰기가 나도 다 보낼 때까지 반복 | main.c `handle_client` |
| `find_header_end` (static) | `\r\n\r\n` 위치를 찾아 헤더 길이 반환 | main.c `handle_client` |
| `handle_client` (static) | 헤더 끝까지 읽고 hello 또는 431 | main.c `main` |
| `main` | 포트 읽기 → 리스닝 → accept 반복 | — |

## 3. 요청 하나 기준 호출 순서

1. `main`이 포트를 읽고 `net_listen(8080, 128)`로 리스닝 소켓을 엽니다.
2. `net_accept`에서 멈춰 기다립니다. 커널이 3-way handshake를 끝낸 연결이 생기면 fd를 받습니다.
3. `handle_client(fd)`가 `net_read`로 4096바이트씩 읽어 `buf_append`로 쌓습니다.
4. 읽을 때마다 `find_header_end`로 빈 줄(`\r\n\r\n`)을 찾습니다.
   - 아직 없고 8KB 이하 → 3번으로 (더 읽기)
   - 8KB 초과 → `net_write_all`로 431 응답
   - 찾음 → `net_write_all`로 `hello` 응답
5. `buf_free`로 버퍼를 해제하고, `main`이 `close(fd)`로 연결을 닫습니다.
6. 2번으로 돌아가 다음 손님을 기다립니다.

흐름 지도로는 ① → ④ → ⑨입니다. ② ③(큐 · 워커)과 ⑤ ~ ⑧(파싱 · 라우팅 · 처리)은 아직 없습니다.

## 4. 추천 읽기 순서

1. `server/main.c` `main` — 전체 뼈대. accept 루프와 실패 처리(Q3)
2. `server/main.c` `handle_client` — 읽기 → 경계 찾기 → 응답. 8KB 상한(Q2)
3. `common/net.c` `net_listen` → `net_accept` — 소켓 준비 순서와 `SO_REUSEADDR`
4. `common/net.c` `net_read`, `net_write_all` — EINTR와 부분 쓰기
5. `common/buf.c` `buf_append` — 2배로 늘리기, 넘침 검사, realloc 실패 처리
6. `server/main.c` `find_header_end` — 왜 `strstr`을 안 쓰나

## 5. 직접 실행해 볼 명령

호스트(PowerShell)에서:
```
docker compose up -d
docker compose exec dev bash
```

컨테이너 안에서:
```
make && make asan
./build/server 8080                 # 터미널 1 (ASan으로 보려면 ./build/asan/server 8080)
curl -v localhost:8080/             # 터미널 2: hello
(printf 'GET / HTTP/1.1\r\nHo'; sleep 1; printf 'st: x\r\n\r\n') | nc localhost 8080
{ printf 'GET / HTTP/1.1\r\nX: '; head -c 9000 /dev/zero | tr '\0' a; } | nc localhost 8080   # 431
```

드러나는 문제 보기 (스레드가 하나라서 생기는 일):
```
nc localhost 8080                   # 터미널 2: 연결만 열고 아무것도 안 보냄
curl -m 5 localhost:8080/           # 터미널 3: 멈춤 → 터미널 2를 Ctrl+C 하면 그때 hello
```

Windows 브라우저에서 `http://localhost:8080/`을 열어도 `hello`가 보입니다. 서버 로그의 손님 IP가 `127.0.0.1`이 아니라 `172.x.x.1`로 찍히는데, Docker 네트워크의 관문(gateway) 주소를 거쳐 들어오기 때문입니다.

## 6. 확인 질문

`find_header_end`가 빈 줄을 찾았는데 `buf`에 그 뒤로 바이트가 더 남아 있을 수 있습니다 (POST 본문, 또는 붙어 온 다음 요청). 지금 서버는 그 바이트를 어떻게 처리하고 있고, 스텝 2 · 5에서 그 바이트를 제대로 다루려면 무엇이 더 필요할까요?

## 알려진 한계 (다음 스텝에서 해결)

- 손님이 응답을 받기 전에 끊으면 `write`에서 SIGPIPE를 받아 서버가 죽을 수 있습니다 → 스텝 8 SIGPIPE 무시
- 8KB보다 훨씬 많이 보내는 손님에게 431을 보내고 바로 닫으면, 읽지 않은 바이트 때문에 커널이 RST를 보내 손님이 431을 못 볼 수 있습니다
- `find_header_end`는 읽을 때마다 처음부터 다시 찾습니다 (최대 8KB라 문제 없음) → 스텝 2 증분 파서로 교체
