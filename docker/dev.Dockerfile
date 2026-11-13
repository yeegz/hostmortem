# Development and CI image. Pinned base; packages from the Ubuntu 24.04 archive.
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++ clang clang-tidy libclang-rt-18-dev shellcheck cmake ninja-build pkg-config git ca-certificates \
      libsqlite3-dev sqlite3 libgtest-dev libgmock-dev nlohmann-json3-dev \
      iproute2 iputils-ping dnsmasq-base iptables procps psmisc python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
