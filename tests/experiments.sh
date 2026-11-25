#!/bin/bash
# 사다리 · 공정성(X1) · 잠금 경합(X3) · 초과예약(X2) · 한 대 죽음을 차례로 잰다 (컨테이너 안, make 뒤, 약 20분)
# 중요한 것부터 돌리므로 아무 때나 끊어도 앞의 결과는 쓸 수 있다. 한 번에 90초를 넘으면 그 측정만 끊는다
# 실행: tests/experiments.sh > /tmp/x.txt 2>&1
cd "$(dirname "$0")/.." || exit 1
R="timeout 90 tests/students.sh"

echo "#### 사다리"
for s in first wide slots gate; do $R $s; done
echo "#### X1 공정성: 다음 과목 줄 끝부터(0) · 이어서(1) x 창 1개 · 3개, 인기 과목 1개 · 2개"
for hot in 1 2; do for keep in 0 1; do for tabn in 1 3; do
    EXTRA="gate_keep $keep\n" $R gate tabn=$tabn hot=$hot
done; done; done
echo "#### X3 잠금 경합: 잠금 10 · 20ms x 과목별 자리 끔 · 16"
for lock in 10 20; do for cs in 0 16; do
    LOCK=$lock EXTRA="gate_course_slots $cs\n" $R gate
done; done
echo "#### X2 초과예약: 안 오는 학생 5 · 30% x 부르는 속도 100 · 150 · 200%"
for leave in 5 30; do for ob in 100 150 200; do
    EXTRA="gate_overbook $ob\n" $R gate leave=$leave
done; done
echo "#### 한 대 죽음"
FAULT=kill $R gate
echo "#### 끝"
