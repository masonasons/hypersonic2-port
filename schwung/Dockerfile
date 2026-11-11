# ARM64 cross compiler for schwung/build.sh when no aarch64-linux-gnu-g++ is installed.
# Ubuntu 22.04: GCC 12 (C++20) against glibc 2.35. libstdc++ is linked statically and takes the build system's glibc
# symbols, so the image's glibc decides what dsp.so needs: Debian bookworm's (2.36) pulled in arc4random, newer than
# Move's. With 22.04's and libgcc linked dynamically (build.sh) dsp.so needs glibc 2.34 at most.
FROM ubuntu:22.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++-12-aarch64-linux-gnu binutils-aarch64-linux-gnu python3 \
    && ln -s aarch64-linux-gnu-g++-12 /usr/bin/aarch64-linux-gnu-g++ \
    && ln -s aarch64-linux-gnu-gcc-12 /usr/bin/aarch64-linux-gnu-gcc \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
WORKDIR /build
