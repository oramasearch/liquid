# Linux sanitizer, fuzzing and hardening runs on the cluster BuildKit lane (liquid #407).
# Nothing is pushed: the build runs, prints its results and is discarded. See
# docs/build/hardening.md for the commands; run.sh drives it.
#
#   MODE=ctest  configure with CMAKE_OPTIONS, build everything, run the whole CTest suite, then
#               print the ELF protections of the liquid binary and the programs it ships
#   MODE=fuzz   configure with -DLIQUID_FUZZ=ON, build the harnesses, run each for FUZZ_SECONDS
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      ca-certificates wget gnupg lsb-release software-properties-common build-essential \
      ninja-build git pkg-config libfuse3-dev fuse3 libstdc++-14-dev zlib1g-dev libssl-dev \
      python3 binutils >/dev/null \
 && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh \
 && /tmp/llvm.sh 19 >/dev/null 2>&1 \
 && wget -qO /tmp/cmake.sh https://github.com/Kitware/CMake/releases/download/v3.31.5/cmake-3.31.5-linux-x86_64.sh \
 && sh /tmp/cmake.sh --skip-license --prefix=/usr/local >/dev/null \
 && rm -rf /var/lib/apt/lists/* /tmp/*.sh
WORKDIR /src
COPY . /src
ARG MODE=ctest
ARG BUILD_TYPE=Release
ARG CMAKE_OPTIONS=
ARG CTEST_JOBS=4
ARG FUZZ_SECONDS=60
ARG FUZZ_HARNESSES=
ARG LABEL=unknown
RUN echo "label ${LABEL}; mode ${MODE}; options ${CMAKE_OPTIONS}; kernel $(uname -r)"; \
    if [ "$MODE" = fuzz ]; then OPTIONS="-DLIQUID_FUZZ=ON -DLIQUID_BUILD_TESTS=OFF ${CMAKE_OPTIONS}"; \
    else OPTIONS="-DLIQUID_BUILD_TESTS=ON ${CMAKE_OPTIONS}"; fi; \
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
      -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 $OPTIONS > /tmp/configure.log 2>&1; \
    echo "CONFIGURE_EXIT=$?"; grep "liquid:" /tmp/configure.log; \
    if [ "$MODE" = fuzz ]; then TARGET=liquid-fuzz; else TARGET=all; fi; \
    (cmake --build build --target $TARGET -j 4 2>&1; echo "BUILD_EXIT=$?") \
      | grep -E -i " error|FAILED|killed|BUILD_EXIT"; \
    if [ "$MODE" = ctest ]; then \
      timeout 5400 ctest --test-dir build -j ${CTEST_JOBS} --timeout 600 > /tmp/ctest.log 2>&1; \
      echo "CTEST_EXIT=$?"; grep -E "tests passed|tests failed|Total Test" /tmp/ctest.log; \
      grep -E "^\s+[0-9]+ - .*\((Failed|Timeout|SEGFAULT|Subprocess|Exception|ILLEGAL|BUS|OTHER)" /tmp/ctest.log \
        | sed 's/^/FAILED_TEST /'; \
      echo "SKIPPED_COUNT=$(grep -cE '\(Skipped\)' /tmp/ctest.log)"; \
      grep -lE "ERROR: AddressSanitizer|runtime error:" build/Testing/Temporary/LastTest.log >/dev/null 2>&1 \
        && grep -E -A30 "ERROR: AddressSanitizer|runtime error:" build/Testing/Temporary/LastTest.log | head -400; \
      for binary in build/liquid build/libliquid-wrap-hook.so build/liquid-read-guard; do \
        [ -f "$binary" ] || continue; \
        echo "--- $binary"; readelf -hW "$binary" | grep -E "Type:"; \
        readelf -lW "$binary" | grep -E "GNU_RELRO|GNU_STACK"; readelf -dW "$binary" | grep -E "BIND_NOW|FLAGS"; \
        echo "stack_chk_fail $(nm "$binary" 2>/dev/null | grep -c __stack_chk_fail) fortified $(nm "$binary" 2>/dev/null | grep -cE '__[a-z_]+_chk\b')"; \
      done; \
    elif [ "$MODE" = fuzz ]; then \
      for harness in ${FUZZ_HARNESSES:-$(ls build/fuzz/corpus)}; do \
        build/fuzz/liquid-fuzz-$harness build/fuzz/corpus/$harness -max_total_time=${FUZZ_SECONDS} \
          -rss_limit_mb=4096 -artifact_prefix=/tmp/$harness- > /tmp/fuzz-$harness.log 2>&1; \
        echo "FUZZ $harness exit=$? $(grep -E '^(#[0-9]+\s+DONE|Done [0-9]+ runs)' /tmp/fuzz-$harness.log | tail -1)"; \
        grep -E -A40 "ERROR: (AddressSanitizer|libFuzzer)|runtime error:|deadly signal" /tmp/fuzz-$harness.log | head -80; \
        for crash in /tmp/$harness-*; do [ -f "$crash" ] && echo "CRASH_INPUT $crash $(od -An -tx1 -v "$crash" | tr -d ' \n' | head -c 2048)"; done; \
      done; \
    fi
