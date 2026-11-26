#!/bin/bash
# 수강신청 중 프록시가 고장 나는 상황. 학생이 본 결과와 백엔드가 처리한 기록을 맞춰 본다.
# 실행: tests/course.sh <none|kill|stop|busy> <보조 0|1>   (컨테이너 안, make 뒤)
#   none 고장 없음 / kill 메인이 죽음 / stop 메인이 멈춤 / busy 메인은 멀쩡한데 느린 요청 16개로 바쁨 (8초)
# 환경 변수: TIMEOUT_MS(보조가 기다리는 시간, 1000) FAILS(연속 실패 수, 3) SERVER(서버 실행 파일)
# 학생은 초당 20명 · 15초. 시도마다 3초 기다리고, 실패하면 0.3초 뒤 같은 학번으로 다시 (10초까지)
set -u
cd "$(dirname "$0")/.." || exit 1
if [ $# -ne 2 ]; then sed -n '2,6p' "$0"; exit 1; fi
FAULT=$1; STANDBY=$2
RATE=20; LEN=15; FAULT_AT=5
PORT=19790
U="http://127.0.0.1:$PORT"
D=$(mktemp -d)
PIDS=""
trap 'kill -KILL $PIDS 2>/dev/null; wait 2>/dev/null; rm -r "$D"' EXIT

# 부팅 뒤 지난 밀리초 (벽시계는 컨테이너에서 튈 수 있어 쓰지 않는다)
now() { local u; read -r u _ < /proc/uptime; echo $((10#${u/./} * 10)); }
# 학생 한 명: "도착 ms · 학번 · 성공 · 시도 수 · 걸린 ms"를 적는다
student() {
    local start ok=0 a=0
    start=$(now)
    while [ $(($(now) - start)) -lt 10000 ]; do
        a=$((a + 1))
        if [ "$(curl -s -m 3 -o /dev/null -w '%{http_code}' -d "student=$2" "$U/api/apply")" = 200 ]; then ok=1; break; fi
        sleep 0.3
    done
    echo "$1 $2 $ok $a $(($(now) - start))" >> "$D/seen"
}

list=""
for k in 1 2 3; do
    ./build/backend $((19700 + k)) 8 2>> "$D/applied" & PIDS="$PIDS $!"
    list="$list 127.0.0.1:$((19700 + k))"
done
cp -r www "$D/www"
printf 'port %s\nroot %s\nstandby_timeout_ms %s\nstandby_fails %s\nlock_file %s\nroute /api/ proxy%s\nroute / static\n' \
    $PORT "$D/www" "${TIMEOUT_MS:-1000}" "${FAILS:-3}" "$D/lock" "$list" > "$D/conf"
SERVER=${SERVER:-./build/server}
$SERVER -c "$D/conf" 2>> "$D/main" & MAIN=$!; PIDS="$PIDS $MAIN"
sleep 0.8
if [ "$STANDBY" = 1 ]; then $SERVER -c "$D/conf" 2>> "$D/standby" & PIDS="$PIDS $!"; sleep 0.5; fi

jobs_=""
t0=$(now)
case $FAULT in
    kill) (sleep $FAULT_AT; kill -KILL $MAIN) & jobs_=$! ;;
    stop) (sleep $FAULT_AT; kill -STOP $MAIN) & jobs_=$! ;;
    busy) (sleep $FAULT_AT; wrk -t1 -c16 -d8s --timeout 20s -H 'Connection: close' "$U/api/work?ms=2500" > /dev/null) & jobs_=$! ;;
esac
i=0
while :; do
    t=$(($(now) - t0))
    [ $t -ge $((LEN * 1000)) ] && break
    if [ $t -ge $((i * 1000 / RATE)) ]; then
        i=$((i + 1))
        student $t $i & jobs_="$jobs_ $!"
    else
        sleep 0.01
    fi
done
wait $jobs_
sleep 1 # 끊긴 손님의 일을 백엔드가 마칠 때까지

grep -a '^apply' "$D/applied" | awk '{print $2}' > "$D/ids"
echo "고장 $FAULT, 보조 $STANDBY (제한 ${TIMEOUT_MS:-1000}ms × ${FAILS:-3}번)"
awk 'NR==FNR {n[$1]++; next}
    {N++; c=n[$2]+0; if($4>1) re++; if($5>mx) mx=$5
     if($3 && c==1) good++; else if($3 && c>1) dup++; else if($3) lost++; else if(c) ghost++; else fail++
     if($4>1){if(a==""||$1<a)a=$1; if($1>b)b=$1}}
    END{printf "  학생 %d: 정상 %d · 두 번 신청됨 %d · 됐는데 실패로 앎 %d · 실패 %d · 성공인데 기록 없음 %d\n", N,good,dup,ghost,fail,lost
        printf "  다시 시도한 학생 %d", re+0; if(re) printf " (%.1f~%.1f초 도착)", a/1000, b/1000; printf ", 가장 오래 걸린 학생 %.1f초\n", mx/1000}' "$D/ids" "$D/seen"
[ "$STANDBY" = 1 ] && grep -a -E '^보조|^포트' "$D/standby" | sed 's/^/  보조 기록: /'
exit 0
