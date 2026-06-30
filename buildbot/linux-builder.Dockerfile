FROM debian:11-slim

RUN dpkg --add-architecture i386 \
    && apt-get -o Acquire::Retries=5 update -y \
    && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 install -y --fix-missing --no-install-recommends \
        ca-certificates \
        git \
        gcc \
        g++ \
        gcc-multilib \
        g++-multilib \
        clang \
        cmake \
        make \
        python3 \
        python3-pip \
        procps \
        lib32stdc++-10-dev \
        lib32z1-dev \
        libc6-dev-i386 \
        linux-libc-dev:i386 \
        zlib1g-dev \
        zlib1g-dev:i386 \
    && python3 -m pip install --no-cache-dir git+https://github.com/alliedmodders/ambuild.git \
    && rm -rf /var/lib/apt/lists/*
