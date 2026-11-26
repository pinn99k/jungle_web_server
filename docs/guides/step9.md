# 스텝 9 읽기 가이드 — 수강신청 시나리오 (백엔드 3대)

## 1. 추가 · 변경 파일
| 파일 | 줄 수 | 내용 |
|---|---|---|
| `backend/main.c` | 291 (136에서) | 처리 스레드 수, `/api/work` · `/api/courses` · `/api/apply`, 경로 표 |
| `conf/course.conf` | 18 | 백엔드 3대 + 메인 · 보조 설정 |
| `www/course/` | 약 60 | 과목 목록과 신청 화면 |
| `tests/course.sh` | 약 85 | 학생들이 신청하는 중에 프록시를 고장 내고 결과를 맞춰 봄 |

## 2. 함수 지도
| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `route_request` | 메서드 · 경로를 `ROUTES` 표에서 찾아 실행 | `handle_conn` |
| `do_apply` | 100ms 뒤 `apply 학번`을 **먼저 기록하고** 응답 | `ROUTES` |
| `do_courses` · `do_work` · `do_time` · `do_echo` · `do_slow` | 경로 하나씩 | `ROUTES` |
| `student_id` | 본문 `student=N`의 N, 아니면 -1 | `do_apply` |
| `serve_loop` | 연결을 하나씩 받아 처리 반복. 스레드 수 = 동시 처리 수 | `main` |

## 3. 신청 하나의 순서
1. 학생이 `POST /api/apply` (본문 `student=7`)
2. 프록시가 백엔드 하나를 고름 (Round-Robin 3대)
3. 백엔드가 100ms 처리 → `apply 7` 기록
4. 백엔드 응답 → 프록시 → 학생
5. 3과 4 사이에 프록시가 끊기면: 처리는 됐는데 학생은 실패로 안다

## 4. 추천 읽기 순서
`ROUTES` 표 → `do_apply` → `student_id` → `serve_loop`

## 5. 실행해 볼 명령
```
./build/backend 8001 8 & ./build/backend 8002 8 & ./build/backend 8003 8 &
./build/server -c conf/course.conf
# 브라우저: http://localhost:8080/course/
tests/course.sh none 1
```

## 6. 확인 질문
`do_apply`가 기록을 응답보다 **나중에** 남기면, 이 시나리오에서 무엇을 셀 수 없게 될까요?
