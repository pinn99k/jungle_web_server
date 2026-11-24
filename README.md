# jungle_web_server

## 핑구 구현 실행 방법 (`pinggu` 브랜치)

C 웹 서버 + 리버스 프록시. 빌드와 실행은 Docker 컨테이너(Ubuntu 24.04) 안에서 합니다. Docker Desktop이 켜져 있어야 합니다.

```
git checkout pinggu
docker compose up -d --build     # 처음 한 번 (이미지 빌드 + 컨테이너 시작)
docker compose exec dev bash     # 터미널마다 하나씩 (서버 / curl / 백엔드)
docker compose down              # 끝낼 때
```

컨테이너 안 `/work`가 이 저장소입니다. 호스트(Windows · macOS)에서는 `localhost:8080`으로 접속할 수 있습니다.

컨테이너 안에서 빌드 · 실행:

```
make                    # build/server
make test               # 단위 테스트 (tests/test_*.c)
make asan               # ASan + UBSan으로 서버 · 테스트 빌드 후 테스트 실행
./build/server -c conf/server.conf      # 지금은 스텝 7: / 는 정적 파일, /api/ 는 백엔드 8001 · 8002로 번갈아 (리버스 프록시)
./build/server -p 9090 -r ./www         # 인자가 설정 파일 값을 덮어씀 (-c 없으면 정적 서버만)
./build/backend 8001 & ./build/backend 8002 &    # 테스트용 백엔드 (/api/time · /api/echo · /api/slow)
tests/functional.sh                     # 기능 테스트 (서버 · 백엔드를 스스로 띄움, 종료 코드 = 실패 수)
# 종료: Ctrl+C 또는 kill -TERM → 새 연결을 막고 진행 중인 요청을 마무리한 뒤 끝남
curl -v localhost:8080/
```

빌드 · 실행 명령은 스텝이 진행되면서 여기에 추가합니다.
