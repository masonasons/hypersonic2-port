# ARM64 cross compiler for schwung/build.sh when no aarch64-linux-gnu-g++ is installed.
# Debian bookworm: GCC 12 (C++20) against glibc 2.36; the module links libstdc++ statically.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++-aarch64-linux-gnu binutils-aarch64-linux-gnu python3 \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
WORKDIR /build
