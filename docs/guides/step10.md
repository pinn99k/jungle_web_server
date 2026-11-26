# 스텝 10 읽기 가이드 — 메인 · 보조

## 1. 추가 · 변경 파일
| 파일 | 줄 수 | 내용 |
|---|---|---|
| `server/standby.c` · `.h` | 93 · 9 | 잠금, 멈춤 판단, 이어받기 |
| `server/main.c` | +11 | 리스닝 전에 `standby_acquire` |
| `server/config.c` · `.h` | +9 · +3 | `lock_file` · `standby_timeout_ms` · `standby_fails` |
| `tests/test_router.c` | +16 | 설정 키 테스트 |

## 2. 함수 지도
| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `standby_acquire` | 메인이 될 때까지 기다림. 메인이 되면 잠금 fd | `main` |
| `try_lock` | 잠금 시도. 얻으면 1 (= 메인이 없다) | `standby_acquire` |
| `main_answers` | `GET /`에 제한 시간 안에 1바이트라도 오면 1 | `standby_acquire` |
| `fence_main` | 잠금을 쥔 프로세스를 SIGKILL | `standby_acquire` |

## 3. 순서
```
서버 시작 → 잠금 시도
 ├ 얻음 → net_listen → 스레드 풀 → accept (메인)
 └ 못 얻음 (보조) → 반복
     100ms마다  잠금 시도 ─ 얻음 → 메인이 됨              메인이 죽은 경우
     1초마다    GET / ─ 3번 연속 답 없음 → SIGKILL → 잠금 얻음 → 메인이 됨   메인이 멈춘 경우
```

## 4. 추천 읽기 순서
`standby_acquire` → `try_lock` → `fence_main` → `main_answers`

## 5. 실행해 볼 명령
```
tests/course.sh kill 0 ; tests/course.sh kill 1     # 죽음: 보조 없음 / 있음
tests/course.sh stop 0 ; tests/course.sh stop 1     # 멈춤
tests/course.sh busy 0 ; tests/course.sh busy 1     # 바쁠 뿐
TIMEOUT_MS=3000 tests/course.sh stop 1              # 느긋한 보조
```

## 6. 확인 질문
메인이 죽었는지는 잠금으로, 멈췄는지는 응답으로 봅니다. 멈춤도 잠금으로 알 수는 없을까요?
