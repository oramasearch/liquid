# SPDX-License-Identifier: MIT

# Inputs are pinned (lake#1003): the base by its multi-platform index digest, the CMake installer by
# the SHA-256 Kitware publishes (cmake-<v>-SHA-256.txt), checked before it runs, and the LLVM apt
# channel by its signing key's fingerprint: no script from the network runs, and apt checks every
# clang package against that key. docs/build/ci.md says how to move each pin.
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends autoconf automake ca-certificates wget gnupg lsb-release software-properties-common build-essential ninja-build git libstdc++-14-dev zlib1g-dev libssl-dev libfuse3-dev fuse3 libpcsclite-dev pkg-config postgresql postgresql-client python3 swtpm time >/dev/null \
 && LLVM_KEY_FINGERPRINT=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421 \
 && wget -qO /etc/apt/keyrings/llvm.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && gpg --show-keys --with-colons /etc/apt/keyrings/llvm.asc | grep -q "^fpr:::::::::${LLVM_KEY_FINGERPRINT}:" \
 && echo "deb [signed-by=/etc/apt/keyrings/llvm.asc] http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update -qq && apt-get install -y -qq --no-install-recommends clang-19 lldb-19 lld-19 clangd-19 clang-format-19 libc++-19-dev libc++abi-19-dev >/dev/null \
 && ARCH=$(uname -m | sed 's/arm64/aarch64/') \
 && case "$ARCH" in \
      x86_64) CMAKE_SHA256=69118f4aafaf5f73e9897f2d39cf667a01d6fe8ef66f9a874e38996d7025e966 ;; \
      aarch64) CMAKE_SHA256=7d60f2d688cfc3b1b6797f9f192297d1d429771fffe8725cbc153bf0f36007c5 ;; \
      *) echo "no pinned CMake installer for $ARCH" >&2; exit 1 ;; \
    esac \
 && wget -qO /tmp/cmake.sh https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-linux-$ARCH.sh \
 && echo "$CMAKE_SHA256  /tmp/cmake.sh" | sha256sum -c - >/dev/null \
 && sh /tmp/cmake.sh --skip-license --prefix=/usr/local >/dev/null \
 && rm -rf /var/lib/apt/lists/* /tmp/*.sh
ENV CC=clang-19 CXX=clang++-19
