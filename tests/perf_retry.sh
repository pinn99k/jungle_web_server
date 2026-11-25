#!/bin/bash
# 잠깐 멈춘 백엔드가 돌아온 뒤에도 서버가 계속 실패하는지 잰다 (손님은 2초 기다리고, 실패하면 다시 시도).
# 실행: tests/perf_retry.sh <워커> <재시도> <정지 초> <길이 초> <백엔드 수>   (컨테이너 안, make 뒤)
# 예: tests/perf_retry.sh 8 2 4 45 2
# 손님은 초당 14명, 백엔드 한 대는 100ms짜리를 한 번에 1건 (용량 초당 10건). 정지는 10초 지점에 시작
set -u
cd "$(dirname "$0")/.." || exit 1
if [ $# -ne 5 ]; then sed -n '2,5p' "$0"; exit 1; fi
WORKERS=$1; RETRIES=$2; STOP=$3; LEN=$4; NB=$5
RATE=14
PORT=19290
URL="http://127.0.0.1:$PORT/api/work?ms=100"
D=$(mktemp -d)
BS=""
trap 'kill -KILL $SV $BS 2>/dev/null; rm -r "$D"' EXIT

# 부팅 뒤 지난 밀리초. 벽시계는 컨테이너에서 튈 수 있어 쓰지 않는다
now() { local u; read -r u _ < /proc/uptime; echo $((10#${u/./} * 10)); }
# 손님 한 명: 시도마다 2초 기다리고 실패하면 바로 다시. "도착 ms · 성공 · 시도 수"를 적는다
user() {
    local a ok=0
    for ((a = 1; a <= RETRIES + 1; a++)); do
        if [ "$(curl -s -m 2 -o /dev/null -w '%{http_code}' "$URL")" = 200 ]; then ok=1; break; fi
    done
    [ $ok = 0 ] && a=$((RETRIES + 1))
    echo "$1 $ok $a" >> "$D/out"
}

list=""
for k in $(seq "$NB"); do
    ./build/backend $((19200 + k)) 1 2>> "$D/backend" & BS="$BS $!"
    list="$list 127.0.0.1:$((19200 + k))"
done
printf 'port %s\nthreads %s\nroute /api/ proxy%s\nroute / static\n' $PORT "$WORKERS" "$list" > "$D/conf"
./build/server -c "$D/conf" 2>/dev/null & SV=$!
sleep 0.6

users=""
if [ "$STOP" -gt 0 ]; then
    (sleep 10; kill -STOP $BS; sleep "$STOP"; kill -CONT $BS) & users="$!"
fi
t0=$(now)
i=0
while :; do
    t=$(($(now) - t0))
    [ $t -ge $((LEN * 1000)) ] && break
    if [ $t -ge $((i * 1000 / RATE)) ]; then
        user $t & users="$users $!"
        i=$((i + 1))
    else
        sleep 0.01
    fi
done
wait $users

echo "워커 $WORKERS, 재시도 $RETRIES, 정지 ${STOP}초, 백엔드 ${NB}대 (용량 초당 $((NB * 10))건), 손님 초당 ${RATE}명"
sort -n "$D/out" | awk '{b=int($1/5000); n[b]++; ok[b]+=$2; at[b]+=$3; if(b>m)m=b}
    END{for(b=0;b<=m;b++) printf "  %2d~%2d초 손님 %3d 성공 %3d%% 평균 시도 %.2f\n", b*5,b*5+5,n[b],100*ok[b]/n[b],at[b]/n[b]}'
echo "  백엔드가 처리한 일 $(grep -c GET "$D/backend")건, 성공한 손님 $(awk '{s+=$2} END{print s}' "$D/out")명"
