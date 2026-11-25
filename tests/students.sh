#!/bin/bash
# 학생 5,000명 수강신청 한 번. 사다리 단계를 골라 띄우고, 장부와 학생 결과로 판정 기준을 낸다 (Q10)
# 실행: tests/students.sh <first|wide|slots|gate|course|soldout> [학생 옵션...]   (컨테이너 안, make 뒤)
#   first 처음 설정 그대로 · wide 워커와 줄을 늘림 · slots 백엔드마다 16자리 · gate 번호판과 의자
#   course 과목별 자리 16 · soldout 마감 1초 기억 (뒤 단계는 앞 단계를 모두 포함)
# 환경 변수: N(5000) LOCK(5) FAULT(none|kill|bad) EXTRA(설정 줄 추가) HOT(인기 정원 100)
set -u
cd "$(dirname "$0")/.." || exit 1
STAGE=${1:?단계}; shift
N=${N:-5000}; LOCK=${LOCK:-5}; FAULT=${FAULT:-none}; HOT=${HOT:-100}; PORT=19880
D=$(mktemp -d)
PIDS=""
trap 'kill -KILL $PIDS 2>/dev/null; wait 2>/dev/null; rm -r "$D"' EXIT
cp -r www "$D/www"
cp build/server build/backend build/students build/ledger "$D/"

has() { case " first wide slots gate course soldout " in *" $1 "*" $STAGE "*|*" $STAGE "*) ;; esac
    local order="first wide slots gate course soldout" i=0 a=0 b=0 s
    for s in $order; do i=$((i + 1)); [ "$s" = "$1" ] && a=$i; [ "$s" = "$STAGE" ] && b=$i; done
    [ $b -ge $a ]; }
{
    printf 'port %s\nroot %s\n' $PORT "$D/www"
    has wide && printf 'threads 128\nqueue 16384\nbacklog 4096\nbalance least\neject_fails 3\neject_ms 10000\nretry_safe 1\n'
    has gate && printf 'gate_path /api/apply/\ngate_chairs 48\n'
    has course && printf 'gate_course_slots 16\n'
    has soldout && printf 'gate_soldout_ms 1000\n'
    B="127.0.0.1:19881 127.0.0.1:19882 127.0.0.1:19883"
    printf 'route /api/apply/ proxy %s%s\nroute /api/ proxy %s\nroute / static\n' "$B" "$(has slots && echo ' slots=16')" "$B"
    printf '%b' "${EXTRA:-}"
} > "$D/conf"
OPEN=$("$D/ledger" init "$D/led" "$HOT" "$HOT" 1000 5000)
for k in 1 2 3; do
    mode=""; [ "$FAULT" = bad ] && [ $k = 3 ] && mode=fail
    "$D/backend" $((19880 + k)) 64 ledger="$D/led" lock="$LOCK" db=16 $mode 2>/dev/null & PIDS="$PIDS $!"; LASTB=$!
done
"$D/server" -c "$D/conf" 2> "$D/log" & SRV=$!; PIDS="$PIDS $SRV"
sleep 0.3; kill -0 $SRV || { cat "$D/log"; exit 1; }
[ "$FAULT" = kill ] && (sleep 7; kill -KILL $LASTB) &
idle0=$(awk '/^cpu /{print $5, $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
"$D/students" $PORT "$OPEN" "$N" "$D/csv" "$@" > "$D/out"
idle1=$(awk '/^cpu /{print $5, $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
echo "== $STAGE N=$N lock=$LOCK fault=$FAULT $* ${EXTRA:-}"
"$D/ledger" dump "$D/led" > "$D/dump" 2> "$D/g2"
awk -f tests/summary.awk "$D/dump" "$D/csv"
echo "G2 $(cat "$D/g2")  G5 $(head -2 "$D/out" | tr '\n' ' ')"
tail -1 "$D/out"
awk '/POST \/api\/apply/ && / 127.0.0.1:/ {v[++n] = $(NF - 1) + 0} END {
    for (gap = int(n / 2); gap > 0; gap = int(gap / 2)) for (i = gap + 1; i <= n; i++) {
        x = v[i]; for (j = i; j > gap && v[j - gap] > x; j -= gap) v[j] = v[j - gap]; v[j] = x }
    printf "들어간 신청 %d건, 의자+처리 p50 %dms p99 %dms\n", n, v[int(n / 2) + 1], v[int(n * 0.99) + 1]}' "$D/log"
echo "$idle0 $idle1" | awk '{printf "CPU 쉰 비율 %.0f%%\n", 100 * ($3 - $1) / ($4 - $2)}'
[ -n "${SAVE:-}" ] && cp "$D/csv" "$SAVE"
exit 0
