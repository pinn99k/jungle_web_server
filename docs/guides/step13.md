# 스텝 13 읽기 가이드 — 고르기 (`balance`)

## 1. 추가 · 변경 파일
| 파일 | 줄 수 | 내용 |
|---|---|---|
| `server/balance.c` · `.h` | 127 · 18 | 백엔드 고르기 |
| `server/proxy.c` | `attempt` · `forward` · `safe_to_retry` | 고르기와 보내기를 나눔 |
| `server/config.c` · `.h` | 설정 키 | `balance` · `eject_fails` · `eject_ms` · `slow_start_ms` · `retry_safe` · `queue` |
| `tests/test_balance.c` | 5개 테스트 | 나누기 · 몰기 · 덜 바쁜 쪽 · 빼기 · 천천히 |

## 2. 함수 지도
| 함수 | 한 줄 설명 | 호출하는 곳 |
|---|---|---|
| `balance_pick` | 이번 요청을 보낼 백엔드 번호 | `forward` |
| `balance_begin` · `balance_end` | 처리 중 개수와 이어진 실패를 셈 | `forward` |
| `least_busy` | 후보 중 처리 중이 가장 적은 것 | `balance_pick` |
| `attempt` | 백엔드 하나에 한 번 보내기. `sent`로 "보냈나"를 구분 | `forward` |
| `safe_to_retry` | 다시 보내도 되는가 (조회만, 또는 아직 안 보냈을 때) | `forward` |

## 3. 순서
1. `balance_pick`이 멀쩡하고 데워진 서버 → 멀쩡한 서버 → 남은 서버 순으로 후보를 모은다
2. 정책에 따라 하나를 고른다
3. `attempt`가 보낸다. 실패하면 `safe_to_retry`로 다른 서버에 한 번 더
4. `balance_end`가 성공 · 실패를 센다. 이어서 실패하면 잠깐 뺀다

## 4. 추천 읽기 순서
`forward` → `attempt` → `balance_pick` → `collect` → `least_busy`

## 5. 실행해 볼 명령
```
tests/traffic.sh 6      # 업데이트 실수: rr · least · 빼기
tests/traffic.sh 2      # 무거운 요청
```

## 6. 확인 질문
`balance_end`가 실패를 셀 때, 백엔드가 준 500과 연결 실패를 같은 실패로 봅니다. 손님이 잘못된 요청을 보내 생긴 500도 같이 셀 때의 문제는 무엇일까요?
