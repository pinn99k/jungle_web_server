#!/bin/sh
# 기능 테스트: 서버 + 백엔드 2개를 띄우고 design.md 8장 대응표와 주요 기능을 curl · nc로 확인한다.
# 실행: make && tests/functional.sh   (컨테이너 안, 저장소 루트에서)   종료 코드 = 실패 수
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=18080
U="http://127.0.0.1:$PORT"
CONF=$(mktemp)
printf 'port %s\nroute /api/ proxy 127.0.0.1:18001 127.0.0.1:18002\nroute / static\n' "$PORT" > "$CONF"

./build/backend 18001 2>/dev/null & B1=$!
./build/backend 18002 2>/dev/null & B2=$!
./build/server -c "$CONF" 2>/tmp/functional_server.log & S=$!
trap 'kill $S $B1 $B2 2>/dev/null; rm -f "$CONF"' EXIT
sleep 0.5

pass=0
fail=0
# 이름 · 기대값 · 실제값을 비교해 통과 / 실패를 센다
check() {
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1)); echo "통과 $1"
    else
        fail=$((fail + 1)); echo "실패 $1: 기대 [$2] 결과 [$3]"
    fi
}
# curl로 상태 코드만 받는다 (경로를 정리하지 않고 그대로 보냄)
code() { curl -s -o /dev/null -w '%{http_code}' --path-as-is "$@"; }
# 날것의 요청 바이트를 보내고 응답 상태 코드만 받는다 (\r\n은 printf %b로 풀림)
raw() { printf '%b' "$1" | nc -w 8 127.0.0.1 "$PORT" | head -1 | cut -d' ' -f2; }
# 백엔드가 응답에 넣은 포트 번호만 뽑는다
port_of() { curl -s "$U/api/time" | sed 's/.*"port":\([0-9]*\).*/\1/'; }
long() { head -c "$1" /dev/zero | tr '\0' a; }

echo "== 정적 파일"
check "정적 200" 200 "$(code "$U/")"
check "없는 파일 404" 404 "$(code "$U/nope.html")"
check "경로 탐색 403" 403 "$(code "$U/../etc/passwd")"
check "점 파일 404" 404 "$(code "$U/.env")"
check "정적 POST 405" 405 "$(code -X POST -d a "$U/")"
check "끝 / 없는 디렉토리 301" 301 "$(code "$U/docs")"

echo "== 잘못된 요청"
check "Host 없음 400" 400 "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "헤더 이름 뒤 공백 400" 400 "$(raw 'GET / HTTP/1.1\r\nHost : x\r\n\r\n')"
check "\\\\n만 쓴 줄 400" 400 "$(raw 'GET / HTTP/1.1\nHost: x\n\n')"
check "메서드 형식 오류 400" 400 "$(raw 'get / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "17자 메서드 501" 501 "$(raw 'ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\nHost: x\r\n\r\n')"
check "모르는 버전 505" 505 "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
check "CL + TE 400" 400 "$(raw 'POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\na')"
check "POST 길이 없음 411" 411 "$(raw 'POST /api/echo HTTP/1.1\r\nHost: x\r\n\r\n')"
check "불완전한 헤더 후 무응답 408" 408 "$( (printf 'GET / HTTP/1.1\r\nHo'; sleep 6) | nc -w 8 127.0.0.1 "$PORT" | head -1 | cut -d' ' -f2)"

echo "== 매우 큰 요청"
check "URI 8KB 초과 414" 414 "$(code "$U/$(long 9000)")"
check "헤더 8KB 초과 431" 431 "$(code -H "X: $(long 9000)" "$U/")"
check "본문 1MB 초과 413" 413 "$(long 1100000 | curl -s -o /dev/null -w '%{http_code}' --data-binary @- "$U/api/echo")"

echo "== 리버스 프록시"
check "Round-Robin 교대" "18001 18002" "$(port_of) $(port_of)"
check "본문 전달" hello "$(curl -s -d hello "$U/api/echo")"
check "처리하지 않는 메서드도 백엔드로 전달 (백엔드 404)" 404 "$(code -X DELETE "$U/api/x")"
check "응답 지연 504" 504 "$(code "$U/api/slow")"
sleep 3 # /api/slow를 받은 백엔드가 5초 sleep을 마칠 때까지

echo "== 손님 비정상 종료"
curl -s -m 0.3 "$U/api/slow" >/dev/null 2>&1 # 응답 전에 손님이 끊음 → 서버는 EPIPE로 정리
sleep 3
check "손님이 끊은 뒤에도 서버 살아 있음" 200 "$(code "$U/")"

echo "== 백엔드 문제"
kill "$B1" 2>/dev/null; wait "$B1" 2>/dev/null
check "백엔드 하나 꺼짐 → 나머지로" "18002 18002" "$(port_of) $(port_of)"
kill "$B2" 2>/dev/null; wait "$B2" 2>/dev/null
check "백엔드 모두 꺼짐 502" 502 "$(code "$U/api/time")"

echo "== 결과: 통과 $pass, 실패 $fail"
exit "$fail"
