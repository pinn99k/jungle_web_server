# 웹 서버 만들기 요구사항

Krafton Jungle 과제 원문을 마크다운으로 옮긴 것입니다. 내용은 수정하지 않습니다.

## 과제 개요

- 이번 단계 과제는 'C 언어로 동작하는 웹서버' 구현입니다.
- 외부 HTTP 라이브러리·프레임워크 없이, POSIX 소켓 API만 사용하여 처음부터 직접 작성합니다.
- 단순 HTTP 서버에서 끝나지 않고, Proxy 또는 Reverse Proxy 기능을 함께 구현합니다.

## 필수 구현 항목

- TCP 소켓 서버
  - socket() → bind() → listen() → accept()의 표준 흐름을 직접 구현합니다.
  - 클라이언트 연결을 수락하고 요청 / 응답을 송수신합니다.
- HTTP 요청 파싱
  - 요청 라인 (메서드, URI, 버전) 파싱
  - 헤더 파싱 (Host, Content-Length, Connection 등)
  - 본문 (body) 처리 (POST 요청)
  - 최소 지원 메서드: GET, POST
- HTTP 응답 생성
  - 상태 라인 (HTTP/1.1 200 OK 등)
  - 표준 헤더 (Content-Type, Content-Length, Connection 등)
  - 본문 송신
- 정적 파일 서빙
  - 지정된 루트 디렉토리에서 파일을 읽어 응답합니다.
  - MIME 타입 매핑 (HTML, CSS, JS, PNG, JPG 등)
  - 404 / 403 / 500 등 기본 오류 응답 처리
- 동적 요청 처리
  - 요청에 따라 애플리케이션 로직을 실행하고 응답을 생성합니다.
  - 예: CGI 프로그램 실행, 또는 백엔드 서버로 요청 전달
- 동시 요청 처리
  - 다음 방식 중 하나 이상을 선택해 구현합니다.
    - 멀티프로세스: fork()
    - 멀티스레드: pthread (요청 처리 스레드 풀 권장)
    - I/O 멀티플렉싱: select() / poll() / epoll() / kqueue()
  - I/O 멀티플렉싱 방식을 선택하는 경우, 이벤트 루프를 통해 여러 연결의 I/O를 관리합니다.

## Proxy 또는 Reverse Proxy 구현 (필수)

- 두 가지 중 최소 한 가지를 구현합니다 (둘 다 구현하면 더 좋습니다).
- Forward Proxy 방식
  - 클라이언트의 HTTP 요청을 받아 외부 서버에 대신 전달하고 응답을 그대로 돌려줍니다.
  - 흐름: 클라이언트 → Proxy → 원 서버
  - 요청 URL의 호스트를 파악하여 새로운 TCP 연결을 만들고 데이터를 중계합니다.
  - 선택적으로 캐싱 기능을 개발합니다.
- Reverse Proxy 방식
  - 외부 요청을 받아 내부에 있는 여러 백엔드 서버로 라우팅합니다.
  - 흐름: 클라이언트 → Reverse Proxy → 백엔드 (1 .. N)
  - 가능하면 단순 로드 밸런싱 (Round-Robin 등)까지 포함합니다.
  - 라우팅 규칙은 경로 (/api/* → 8001, /static/* → 8002 …) 또는 호스트 헤더 기반으로 설계합니다.
  - 정적 요청을 프록시가 직접 서빙할지, 백엔드로 전달할지는 구현자가 선택합니다.

## 기술 스택 / 제한

- 언어: C
- 라이브러리 제한
  - 금지: libevent, libuv, nginx, Apache, mongoose 같은 완성된 HTTP / 이벤트 루프 라이브러리
  - 허용: 표준 C 라이브러리, POSIX API (`<sys/socket.h>`, `<netinet/in.h>`, `<unistd.h>`, `<pthread.h>` 등)
- 빌드: Makefile 또는 CMakeLists.txt 제공
- 실행 방법
  - 포트 번호와 정적 파일 루트 (그리고 Reverse Proxy 라우팅 규칙)를 CLI 인자 또는 설정 파일로 받습니다.

## 개발 환경

- 본 과제는 POSIX 소켓 API를 사용하므로 macOS / Linux 환경에서 그대로 개발 가능합니다.
- Windows 학생은 반드시 WSL2 (Windows Subsystem for Linux) + Ubuntu, Docker 혹은 클라우드에서 개발해야 합니다.
- 한 소스 코드를 여러 환경 (macOS / WSL / Linux)에서 빌드·실행하려는 경우, 사용하는 라이브러리나 함수가 OS별로 차이가 있을 수 있으므로 이식성까지 고려해서 개발을 진행합니다. (POSIX 표준 범위 안에 머무르거나, #ifdef로 플랫폼별 분기 처리)

## 중점 포인트

- 소켓 프로그래밍의 기본 흐름
  - 3-way handshake, SO_REUSEADDR, blocking / non-blocking 차이
  - bind 실패 / accept 블로킹 등의 동작 원리
- HTTP 프로토콜 명세 (RFC 7230 ~ 7235)의 최소 구현 범위
  - 요청 라인 / 헤더 / 본문의 경계를 어떻게 식별할 것인가
  - Content-Length 기반 본문 길이 처리
  - Connection: keep-alive의 의미와 처리
- 동시성 모델 선택의 트레이드오프
  - process-per-request vs thread pool vs event loop
  - C10K 문제와 관련된 설계 고려사항
- Proxy vs Reverse Proxy의 차이
  - 누구의 입장에서 누구를 대신하는가
  - 보안 / 로드 밸런싱 / 캐싱 측면에서 각각의 활용
- 메모리 관리
  - malloc / free 짝 맞추기, 버퍼 오버플로우 방지
  - 클라이언트 끊김 · 비정상 종료 시 자원 회수
- 시그널 / 종료 처리
  - SIGINT / SIGTERM 안전한 셧다운
  - SIGPIPE 무시 (쓰기 도중 클라이언트 끊김 대응)

## 품질

- 단위 테스트를 통해 각 모듈 (HTTP 파서, MIME 매핑, 라우팅 / 프록시 전달 로직 등)을 검증합니다.
- 기능 테스트
  - curl, 브라우저 등 실제 HTTP 클라이언트로 정적 파일 응답 / 프록시 동작 확인
  - 동시 다중 클라이언트 접속 시 정상 동작 확인
- 부하 테스트
  - ab (Apache Bench), wrk, hey 등으로 동시 요청 수에 따른 처리량을 측정합니다.
- 메모리 누수 검사
  - valgrind 또는 컴파일러 sanitizer (fsanitize=address,leak) 등으로 누수 / 잘못된 메모리 접근이 없는지 확인합니다.
- 엣지 케이스를 최대한 고려합니다.
  - 잘못된 HTTP 요청 (불완전한 헤더, 비정상 메서드, 잘못된 버전)
  - 매우 큰 요청 본문 / 매우 긴 URI
  - 백엔드 서버 응답 없음 / 타임아웃 (Reverse Proxy 케이스)
  - 클라이언트의 갑작스러운 연결 종료
- 이력서와 포트폴리오에 포함할 수 있을 만큼 완성도 높은 수준으로 구현합니다.
