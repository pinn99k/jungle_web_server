#!/bin/bash
# 수강신청 운영 실험 (기획서 docs/proposal-traffic.md). 같은 조건을 REP번 재고 범위를 찍는다.
# 실행: tests/traffic.sh <상황 번호 1~6> [...]   (컨테이너 안, make 뒤, 저장소 루트)
# 환경 변수: REP(반복 수, 3) — 막히는 상황(3 · 4)은 bash 손님을 정해진 속도로 보낸다
set -u
cd "$(dirname "$0")/.." || exit 1
REP=${REP:-3}
PORT=19590
U="http://127.0.0.1:$PORT"
D=$(mktemp -d)
PIDS=""
cp -r www "$D/www"
trap 'stop_all; rm -r "$D"' EXIT

now() { local u; read -r u _ < /proc/uptime; echo $((10#${u/./} * 10)); }
cpu() { awk '/^cpu /{t=0; for(i=2;i<=9;i++)t+=$i; print t, $5+$6}' /proc/stat; }
# 프로세스를 모두 끝낸다 (kill -STOP으로 멈춘 것도 끝나게 KILL)
stop_all() {
    [ -n "$PIDS" ] && kill -KILL $PIDS 2>/dev/null && wait $PIDS 2>/dev/null
    PIDS=""
}
# 백엔드를 띄운다: backend <번호> <스레드> [상황]
backend() {
    ./build/backend $((19500 + $1)) "$2" ${3:-} 2>> "$D/b$1" &
    PIDS="$PIDS $!"
    eval "B$1=\$!"
}
# 서버를 띄운다: server <워커> <큐> "<설정 줄들>" "<라우트 줄들>"
server() {
    : > "$D/b1"; : > "$D/b2"; : > "$D/b3"
    printf 'port %s\nroot %s\nthreads %s\nqueue %s\n%s\n%s\nroute / static\n' \
        $PORT "$D/www" "$1" "$2" "$3" "$4" > "$D/conf"
    ./build/server -c "$D/conf" 2>"$D/server.log" &
    SV=$!; PIDS="$PIDS $SV"
    sleep 0.6
}
R3="route /api/ proxy 127.0.0.1:19501 127.0.0.1:19502 127.0.0.1:19503"
# 조회 손님 1명이 새 연결로 계속 조회하는 동안의 지연과 성공 (초 단위 길이)
lookup() {
    wrk -t1 -c"${2:-1}" -d"$1"s --timeout 20s --latency -H 'Connection: close' "$U/api/courses" |
        awk '/ 50%/{a=$2} / 99%/{b=$2} /Requests\/sec/{q=$2} /Non-2xx/{n=$NF} END{printf "조회 p50 %s p99 %s 초당 %s 실패 %d", a, b, q, n+0}'
}

# 상황 1. 평소: 조회만 드문드문. 서버를 1 · 2 · 3대 켰을 때
s1() {
    local k r list
    for r in $(seq "$REP"); do
        for k in 1 2 3; do
            list="route /api/ proxy"
            for i in $(seq $k); do backend $i 8; list="$list 127.0.0.1:$((19500 + i))"; done
            server 8 128 "" "$list"
            echo "평소 조회, 백엔드 $k대: $(lookup 4 2)"
            stop_all
        done
    done
}
# 상황 2. 준비 기간: 통계(2초)를 계속 보는 학생 4명 + 조회
s2() {
    local r p
    for r in $(seq "$REP"); do
        for p in rr least own; do
            backend 1 2; backend 2 2; backend 3 2
            if [ $p = own ]; then server 16 128 "balance least" "route /api/stats proxy 127.0.0.1:19503
route /api/ proxy 127.0.0.1:19501 127.0.0.1:19502"; else server 16 128 "balance $p" "$R3"; fi
            wrk -t1 -c4 -d9s --timeout 20s -H 'Connection: close' "$U/api/stats" > "$D/st" & PIDS="$PIDS $!"
            sleep 2
            printf '준비 기간 %-5s: %s' $p "$(lookup 6)"
            wait $! 2>/dev/null; awk '/Requests\/sec/{printf " 통계 초당 %s", $2} END{print ""}' "$D/st"
            stop_all
        done
    done
}
# 학생 한 명 (상황 3 · 4). <도착 ms> <종류> <번호>
#   polite 번호표로 1초마다 · press 0.2초마다 · flaky 한 번 오고 4초 뒤 다시 · leave 한 번만 오고 떠남 · multi 번호표 세 장
student() {
    local start t code ok=0 a=0 ticket="" wait_s=1 n m=8
    [ "$2" = leave ] && m=1
    start=$(now)
    case $2 in press) wait_s=0.2 ;; flaky) wait_s=4 ;; esac
    if [ "$2" = look ]; then # 조회 한 번 (상황 4)
        code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "$U/api/courses")
        [ "$code" = 200 ] && ok=1
        echo "$1 $2 $3 $ok 1 $(($(now) - start))" >> "$D/seen"
        return
    fi
    while [ $(($(now) - start)) -lt 12000 ]; do
        a=$((a + 1))
        n=1; [ "$2" = multi ] && n=3
        for k in $(seq $n); do
            local jar="$D/jar_$3_$k"
            code=$(curl -s -m $m -o /dev/null -w '%{http_code}' -c "$jar" -b "$jar" -d "student=$3" "$U/api/apply")
            if [ "$code" = 200 ]; then ok=1; break 2; fi
        done
        [ "$2" = leave ] && break
        [ "$2" = calm ] && break
        sleep $wait_s
    done
    echo "$1 $2 $3 $ok $a $(($(now) - start))" >> "$D/seen"
    rm -f "$D"/jar_"$3"_*
}
# 종류별 결과와 빈 자리 · 두 번 신청을 찍는다
summary() {
    awk '{n[$2]++; k[$2]+=$4; w[$2]+=$6; if($6>m[$2])m[$2]=$6} END{for(t in n) printf "    %-7s %3d명 성공 %3d%% 평균 %.1f초 최대 %.1f초\n", t, n[t], 100*k[t]/n[t], w[t]/n[t]/1000, m[t]/1000}' "$D/seen" | sort
    echo "    두 번 처리된 학생 $(grep -ah '^apply' "$D"/b[123] | awk '{print $2}' | sort | uniq -d | wc -l)명, 성공 $(awk '$4{s++}END{print s+0}' "$D/seen")명, 백엔드 처리 $(grep -ah '^apply' "$D"/b[123] | wc -l)건"
}
# 손님을 초당 rate명으로 len초 동안 보낸다. <rate> <len초> <종류 목록을 돌며 배정>
crowd() {
    local rate=$1 len=$2 t i=0 users="" t0 kind
    shift 2
    : > "$D/seen"
    t0=$(now)
    while :; do
        t=$(($(now) - t0))
        [ $t -ge $((len * 1000)) ] && break
        if [ $t -ge $((i * 1000 / rate)) ]; then
            kind=${*:$((i % $# + 1)):1}
            student $t "$kind" $((i + 1)) & users="$users $!"
            i=$((i + 1))
        else
            sleep 0.01
        fi
    done
    wait $users
}
# 상황 3. 신청 시작: 백엔드는 신청 한 건에 1초(cold), 3대라 초당 3건. 초당 10명이 10초 (약 3.3배)
s3() {
    local r cfg name
    for r in $(seq "$REP"); do
        for name in "기억 없음(지금)" "번호표 유예 1초" "번호표 유예 3초" "번호표 유예 10초" "번호표 유예 3초 자리 6" "번호표 추첨"; do
            case $name in
                기억*) cfg="" ;;
                *"1초"*) cfg=$'gate_path /api/apply\ngate_slots 3\ngate_grace_ms 1000' ;;
                *"자리 6"*) cfg=$'gate_path /api/apply\ngate_slots 6\ngate_grace_ms 3000' ;;
                *"3초"*) cfg=$'gate_path /api/apply\ngate_slots 3\ngate_grace_ms 3000' ;;
                *"10초"*) cfg=$'gate_path /api/apply\ngate_slots 3\ngate_grace_ms 10000' ;;
                *추첨*) cfg=$'gate_path /api/apply\ngate_slots 3\ngate_grace_ms 3000\ngate_lottery 1' ;;
            esac
            backend 1 1 cold=600000; backend 2 1 cold=600000; backend 3 1 cold=600000
            server 8 24 "balance least
backend_timeout_ms 20000
$cfg" "$R3"
            local c0; c0=$(cpu)
            crowd 10 10 polite polite press flaky leave multi
            echo "신청 시작, $name (CPU 쉰 비율 $(echo "$c0 $(cpu)" | awk '{printf "%d%%", 100*($4-$2)/($3-$1)}'))"
            summary
            stop_all
        done
    done
}
# 상황 4. 신청 도중 고장: (가) 백엔드 1스레드 x 3, 초당 25명(용량 30) 중 하나가 죽어 용량이 20이 됨
# (나) 신청 한 건에 1초, 처리 중인 것이 있는 순간에 하나가 죽음 + 조회도 계속 옴
s4() {
    local r name cfg
    for r in $(seq "$REP"); do
        for name in "차례대로(지금)" "고르게" "빼기+고르게"; do
            case $name in
                차례*) cfg="balance rr" ;;
                고르게) cfg="balance even" ;;
                빼기*) cfg=$'balance even\neject_fails 2\neject_ms 5000' ;;
            esac
            backend 1 1; backend 2 1; backend 3 1
            server 24 24 "$cfg" "$R3"
            (sleep 5; kill -KILL $B1; sleep 10; ./build/backend 19501 1 2>> "$D/b1" & echo $! > "$D/pid"; wait) & PIDS="$PIDS $!"
            crowd 25 20 calm
            echo "고장(가) 몰림, $name"
            summary
            [ -f "$D/pid" ] && PIDS="$PIDS $(cat "$D/pid")"
            stop_all
        done
        for name in "그대로" "조회만 다시"; do
            [ "$name" = "그대로" ] && cfg=$'balance even' || cfg=$'balance even\nretry_safe 1'
            backend 1 8 cold=600000; backend 2 8 cold=600000; backend 3 8 cold=600000
            server 24 128 "$cfg
backend_timeout_ms 20000" "$R3"
            (sleep 4; kill -KILL $B1; sleep 20) & PIDS="$PIDS $!"
            crowd 12 9 calm look
            echo "고장(나) 처리 중, $name"
            summary
            stop_all
        done
    done
}
# 상황 5. 업데이트: 3번이 막 켜져 5초 동안 10배 느림. 조회 손님 2명 + 신청 흉내 부하
s5() {
    local r p
    for r in $(seq "$REP"); do
        for p in "balance rr" "balance least" $'balance least\nslow_start_ms 5000\neject_fails 3\neject_ms 1000'; do
            backend 1 8; backend 2 8
            server 24 128 "$p" "$R3"
            (sleep 2; ./build/backend 19503 8 cold=5000 2>> "$D/b3" & echo $! > "$D/pid"; wait) & PIDS="$PIDS $!"
            printf '업데이트 %-30s: ' "$(echo $p | tr '\n' ' ')"
            printf 'wrk.method="POST"; wrk.body="student=1"; wrk.headers["Connection"]="close"' > "$D/a.lua"
            wrk -t1 -c12 -d10s --timeout 20s --latency -s "$D/a.lua" "$U/api/apply" |
                awk '/ 50%/{a=$2} / 99%/{b=$2} /Requests\/sec/{q=$2} /Non-2xx/{n=$NF} END{printf "신청 p50 %s p99 %s 초당 %s 실패 %d\n", a, b, q, n+0}'
            PIDS="$PIDS $(cat "$D/pid" 2>/dev/null)"
            stop_all
        done
    done
}
# 상황 6. 업데이트 실수: 1번이 바로 500
s6() {
    local r p
    for r in $(seq "$REP"); do
        for p in "balance rr" "balance least" $'balance least\neject_fails 3\neject_ms 10000' $'balance rr\neject_fails 3\neject_ms 10000'; do
            backend 1 8 fail; backend 2 8; backend 3 8
            server 24 128 "$p" "$R3"
            printf '업데이트 실수 %-34s: ' "$(echo $p | tr '\n' ' ')"
            wrk -t1 -c8 -d5s --timeout 20s -H 'Connection: close' "$U/api/courses" |
                awk '/requests in/{t=$1} /Non-2xx/{n=$NF} END{printf "요청 %d 실패 %.1f%%\n", t, t ? 100*n/t : 0}'
            stop_all
        done
    done
}

[ $# -eq 0 ] && { sed -n '2,4p' "$0"; exit 1; }
for n in "$@"; do
    echo "== 상황 $n"
    : > "$D/b1"; : > "$D/b2"; : > "$D/b3"
    "s$n"
done
