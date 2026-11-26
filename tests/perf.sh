#!/bin/bash
# 상황별 성능 측정. 같은 명령으로 전 · 후를 비교한다 (합격 · 불합격은 없음).
# 실행: make && tests/perf.sh [묶음...]   (컨테이너 안, 저장소 루트)   묶음: base conn worker lb block rr down deploy idle slow stop
# 정적 파일은 /tmp에 복사해서 잰다. /work는 Windows 폴더 마운트(9p)라 파일 여는 시간이 결과를 덮는다 (base에서 비교)
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=19090
U="http://127.0.0.1:$PORT"
DIR=$(mktemp -d)
SEC=${SEC:-4}                           # wrk 한 번 길이(초)
REP=${REP:-3}                           # 반복 수
PIDS=""
cp -r www "$DIR/www"
head -c 5000000 /dev/zero > "$DIR/www/big.bin"

# 띄운 프로세스를 모두 끝낸다 (kill -STOP으로 멈춘 것도 끝나게 KILL)
stop_all() {
    [ -n "$PIDS" ] && kill -KILL $PIDS 2>/dev/null && wait $PIDS 2>/dev/null
    PIDS=""
}
trap 'stop_all; rm -r "$DIR"' EXIT

# 서버를 띄운다: start <워커 수> <백엔드 수> <백엔드 스레드 수> [루트]
start() {
    local i list=""
    BE=()
    for i in $(seq "$2"); do
        ./build/backend $((19000 + i)) "$3" 2>/dev/null &
        BE+=($!); PIDS="$PIDS $!"
        list="$list 127.0.0.1:$((19000 + i))"
    done
    printf 'port %s\nroot %s\nthreads %s\nroute / static\n' "$PORT" "${4:-$DIR/www}" "$1" > "$DIR/conf"
    [ "$2" -gt 0 ] && printf 'route /api/ proxy%s\n' "$list" >> "$DIR/conf"
    ./build/server -c "$DIR/conf" 2>/dev/null &
    SV=$!; PIDS="$PIDS $SV"
    sleep 0.5
}

# wrk 결과를 한 줄로 줄인다: rps · p50 · p99 · 최대 · 소켓 오류(timeout 포함) · 2xx 아님
brief() {
    awk '/ 50%/{p50=$2} / 99%/{p99=$2} /Latency  /{mx=$4} /Requests\/sec/{r=$2}
         /Socket errors/{e=$0; sub(/.*errors: /,"",e)} /Non-2xx/{n=$NF}
         END{printf "rps=%-9s p50=%-9s p99=%-9s max=%-9s 오류=[%s] 2xx아님=%s\n", r,p50,p99,mx,e,n+0}'
}
# wrk 실행: load <연결 수> <경로> [추가 옵션...]
load() {
    local c=$1 path=$2 t=2
    shift 2
    [ "$c" -lt 2 ] && t=1
    wrk -t$t -c"$c" -d"${SEC}s" --timeout 10s --latency "$@" "$U$path" | brief
}
CLOSE=(-H "Connection: close")          # 새 손님처럼 요청마다 새 연결

# 루트가 9p 마운트일 때와 컨테이너 디스크일 때, 프록시일 때의 기준선
g_base() {
    local i
    for i in $(seq "$REP"); do
        start 8 0 1 ./www; echo "정적 /work(9p) c=8: $(load 8 /index.html)"; stop_all
        start 8 0 1;       echo "정적 /tmp c=8:      $(load 8 /index.html)"; stop_all
        start 8 2 8;       echo "프록시 /api/time c=8: $(load 8 /api/time)"; stop_all
    done
}
# 워커 8에서 동시 연결 수를 늘릴 때 (keep-alive)
g_conn() {
    local c i
    for c in 1 4 8 9 16 64; do
        for i in $(seq "$REP"); do start 8 0 1; echo "워커8 연결$c: $(load "$c" /index.html)"; stop_all; done
    done
}
# 연결 64에서 워커 수를 늘릴 때 (메모리 포함)
g_worker() {
    local t i
    for t in 2 8 16 32 64 128; do
        for i in $(seq "$REP"); do
            start "$t" 0 1
            echo "워커$t 연결64: $(load 64 /index.html) RSS=$(awk '/VmRSS/{print $2$3}' "/proc/$SV/status")"
            stop_all
        done
    done
}
# 백엔드가 병목일 때(50ms, 한 대가 한 번에 1건) 대수를 늘리면 처리량이 느나
g_lb() {
    local b i
    for b in 1 2 4 8; do
        for i in $(seq "$REP"); do start 8 "$b" 1; echo "백엔드$b대 50ms: $(load 8 '/api/work?ms=50')"; stop_all; done
    done
}
# 1초 걸리는 API를 k명이 계속 부를 때, 새로 온 정적 손님이 겪는 지연
g_block() {
    local k i
    for k in 0 4 6 7 8 12; do
        for i in $(seq "$REP"); do
            start 8 2 16
            if [ "$k" -gt 0 ]; then
                wrk -t1 -c"$k" -d$((SEC + 2))s --timeout 10s "${CLOSE[@]}" "$U/api/work?ms=1000" >/dev/null &
                PIDS="$PIDS $!"; sleep 1
            fi
            echo "느린API $k명 중 정적: $(load 1 /index.html "${CLOSE[@]}")"
            stop_all
        done
    done
}
# 한 번에 1건인 백엔드 2대에서, 2초 요청 1명이 빠른 요청을 얼마나 늦추나
g_rr() {
    local i
    for i in $(seq "$REP"); do
        start 8 2 1; echo "빠른 API만:            $(load 2 /api/time "${CLOSE[@]}")"; stop_all
        start 8 2 1
        wrk -t1 -c1 -d$((SEC + 4))s --timeout 10s "${CLOSE[@]}" "$U/api/work?ms=2000" >/dev/null &
        PIDS="$PIDS $!"; sleep 0.5
        echo "빠른 API + 2초 요청 1명: $(load 2 /api/time "${CLOSE[@]}")"
        stop_all
    done
}
# 백엔드 한 대가 멈췄을 때(kill -STOP) 요청 20번의 상태 코드와 평균 시간
g_down() {
    start 8 2 8
    kill -STOP "${BE[0]}"
    for _ in $(seq 20); do curl -s -o /dev/null -w '%{http_code} %{time_total}\n' "$U/api/time"; done |
        awk '{c[$1]++; s+=$2} END{for(k in c) printf "%s=%d ", k, c[k]; printf "평균 %.3f초\n", s/NR}'
    stop_all
}
# 부하 중에 백엔드 한 대를 2초 동안 껐다 켤 때 실패 수
g_deploy() {
    local i
    for i in $(seq "$REP"); do
        start 8 2 8
        (sleep 2; kill "${BE[0]}"; sleep 2; exec ./build/backend 19001 8 2>/dev/null) &
        PIDS="$PIDS $!"
        echo "배포(한 대 2초 꺼짐): $(SEC=6 load 4 /api/time "${CLOSE[@]}")"
        stop_all
    done
}
# 요청 하나 보내고 쉬는 keep-alive 연결 k개가 있을 때 새 손님의 첫 응답 시간
g_idle() {
    local k i j
    for k in 0 7 8 16; do
        for i in $(seq "$REP"); do
            start 8 0 1
            for j in $(seq "$k"); do
                (printf 'GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n'; sleep 15) | timeout 16 nc 127.0.0.1 "$PORT" >/dev/null 2>&1 &
                PIDS="$PIDS $!"
            done
            sleep 0.5
            echo "쉬는 연결 $k개: $(curl -s -m 20 -o /dev/null -w '%{http_code} %{time_total}초' "$U/index.html")"
            stop_all
        done
    done
}
# 5MB를 초당 200KB로 받는 느린 손님 k명이 있을 때 새 손님의 응답 시간
g_slow() {
    local k i j
    for k in 0 7 8; do
        for i in $(seq "$REP"); do
            start 8 0 1
            for j in $(seq "$k"); do
                curl -s -o /dev/null --limit-rate 200k "$U/big.bin" & PIDS="$PIDS $!"
            done
            sleep 1
            echo "느린 손님 $k명: $(curl -s -m 30 -o /dev/null -w '%{http_code} %{time_total}초' "$U/index.html")"
            stop_all
        done
    done
}
# 2초 걸리는 요청을 처리하는 중에 SIGTERM: 진행 중 응답을 받는지, 몇 초 뒤 꺼지는지
g_stop() {
    local i s
    for i in $(seq "$REP"); do
        start 8 2 8
        curl -s -o /dev/null -w '%{http_code}' "$U/api/work?ms=2000" > "$DIR/code" & local c=$!
        sleep 0.5
        s=$(date +%s%N); kill -TERM "$SV"; wait "$SV" 2>/dev/null
        echo "종료: 진행 중 응답 $(wait $c; cat "$DIR/code"), 꺼지는 데 $((($(date +%s%N) - s) / 1000000))ms"
        stop_all
    done
}

[ $# -eq 0 ] && set -- base conn worker lb block rr down deploy idle slow stop
for g in "$@"; do
    echo "== $g"
    "g_$g"
done
