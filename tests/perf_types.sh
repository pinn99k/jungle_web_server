#!/bin/bash
# 손님 유형을 섞어 유형별 성공률을 잰다 (용량 초당 20건에 손님 초당 30명, 각자 2초까지만 참음).
# 실행: tests/perf_types.sh <다수 유형> <소수 유형> <소수 %> [워커 8] [길이 20초]   (컨테이너 안, make 뒤)
# calm  한 번만 요청하고 기다린다. 거절되면 포기
# retry 거절(503 · 연결 끊김)되면 0.2초 쉬고 다시. 2초가 다 될 때까지
# press 0.7초 안에 답이 없으면 끊고 새로 요청 (새로고침). 거절돼도 다시. 2초가 다 될 때까지
set -u
cd "$(dirname "$0")/.." || exit 1
if [ $# -lt 3 ]; then sed -n '2,6p' "$0"; exit 1; fi
MAJOR=$1; MINOR=$2; PCT=$3; WORKERS=${4:-8}; LEN=${5:-20}
RATE=30
PORT=19390
URL="http://127.0.0.1:$PORT/api/work?ms=100"
D=$(mktemp -d)
BS=""
trap 'kill -KILL $SV $BS 2>/dev/null; rm -r "$D"' EXIT

# 부팅 뒤 지난 밀리초 (벽시계는 컨테이너에서 튈 수 있어 쓰지 않는다)
now() { local u; read -r u _ < /proc/uptime; echo $((10#${u/./} * 10)); }
# 밀리초를 curl -m에 줄 초로 바꾼다
sec() { printf '%d.%03d' $(($1 / 1000)) $(($1 % 1000)); }
# 손님 한 명: "도착 ms · 유형 · 성공 · 시도 수"를 적는다
user() {
    local start left m ok=0 a=0
    start=$(now)
    while :; do
        left=$((2000 - ($(now) - start)))
        [ $left -le 50 ] && break
        a=$((a + 1))
        m=$left
        [ "$2" = press ] && [ $m -gt 700 ] && m=700
        if [ "$(curl -s -m "$(sec $m)" -o /dev/null -w '%{http_code}' "$URL")" = 200 ]; then ok=1; break; fi
        [ "$2" = calm ] && break
        sleep 0.2
    done
    echo "$1 $2 $ok $a" >> "$D/out"
}

./build/backend 19301 1 2>> "$D/backend" & BS="$BS $!"
./build/backend 19302 1 2>> "$D/backend" & BS="$BS $!"
printf 'port %s\nthreads %s\nroute /api/ proxy 127.0.0.1:19301 127.0.0.1:19302\nroute / static\n' \
    $PORT "$WORKERS" > "$D/conf"
./build/server -c "$D/conf" 2>/dev/null & SV=$!
sleep 0.6

users=""
t0=$(now)
i=0
while :; do
    t=$(($(now) - t0))
    [ $t -ge $((LEN * 1000)) ] && break
    if [ $t -ge $((i * 1000 / RATE)) ]; then
        # 7은 100과 서로소라 소수 유형이 시간에 고르게 퍼진다
        type=$MAJOR
        [ $((i * 7 % 100)) -lt "$PCT" ] && type=$MINOR
        user $t "$type" & users="$users $!"
        i=$((i + 1))
    else
        sleep 0.01
    fi
done
wait $users

echo "다수 $MAJOR, 소수 $MINOR $PCT%, 워커 $WORKERS ($(grep -h '^#define QUEUE_CAP' server/main.c | awk '{print "큐", $3}'))"
# 줄이 차기 전인 처음 5초는 뺀다
awk '$1>=5000 {n[$2]++; ok[$2]+=$3; a[$2]+=$4; N++; K+=$3; A+=$4}
    END{for(k in n) printf "  %-5s 손님 %3d 성공 %3d%% 평균 시도 %.2f\n", k, n[k], 100*ok[k]/n[k], a[k]/n[k];
        printf "  전체  손님 %3d 성공 %3d%% 평균 시도 %.2f\n", N, 100*K/N, A/N}' "$D/out"
echo "  백엔드가 처리한 일 $(grep -c GET "$D/backend")건, 성공한 손님 $(awk '{s+=$3} END{print s}' "$D/out")명 (전 구간)"
