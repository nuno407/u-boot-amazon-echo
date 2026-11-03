# Build and test environment: U-Boot's build dependencies, QEMU for the arm64 test runs, and the
# tools the host tests use. For the host's own architecture: native on arm64, and with Debian's
# aarch64 cross compiler elsewhere (build.sh sets CROSS_COMPILE). Nothing runs under emulation.
# The base image is pinned by digest (debian:bookworm-slim, multi-arch index, 2026-10-29), so the
# build environment only changes when this line does. The Debian packages below come from the
# archive at build time.
FROM debian:bookworm-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential bc bison flex libssl-dev libgnutls28-dev uuid-dev zlib1g-dev \
    python3 python3-setuptools python3-pyelftools python3-dev swig \
    git ca-certificates device-tree-compiler gzip xz-utils file \
    qemu-system-arm qemu-efi-aarch64- e2fsprogs gdisk busybox gawk \
    $([ "$TARGETARCH" = arm64 ] || echo gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu) \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
