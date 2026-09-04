FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends ca-certificates wget gnupg lsb-release software-properties-common build-essential ninja-build git libstdc++-14-dev zlib1g-dev libssl-dev libfuse3-dev fuse3 pkg-config python3 >/dev/null \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 19 >/dev/null 2>&1 \
 && apt-get install -y -qq --no-install-recommends libc++-19-dev libc++abi-19-dev >/dev/null \
 && ARCH=$(uname -m | sed 's/arm64/aarch64/') && wget -qO /tmp/cmake.sh https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-linux-$ARCH.sh && sh /tmp/cmake.sh --skip-license --prefix=/usr/local >/dev/null \
 && rm -rf /var/lib/apt/lists/* /tmp/*.sh
ENV CC=clang-19 CXX=clang++-19
