# 빌드 · 실행 환경. 소스는 이미지에 넣지 않고 compose.yaml이 /work로 마운트한다.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential curl netcat-openbsd apache2-utils wrk valgrind \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
