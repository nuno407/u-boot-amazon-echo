# Build and test environment: U-Boot's build dependencies, QEMU for the arm64 test runs, and the
# tools the host tests use. Native arm64 (no cross compiler needed on an arm64 host); on an x86-64
# host Docker runs it under emulation, which is slower but works.
FROM --platform=linux/arm64 debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential bc bison flex libssl-dev libgnutls28-dev uuid-dev zlib1g-dev \
    python3 python3-setuptools python3-pyelftools python3-dev swig \
    git ca-certificates device-tree-compiler gzip xz-utils file \
    qemu-system-arm qemu-efi-aarch64- e2fsprogs gdisk busybox gawk \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
