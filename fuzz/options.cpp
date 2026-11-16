// SPDX-License-Identifier: MIT

// Default sanitizer options for the harnesses, linked into each one.
//
// On Darwin, libFuzzer's own runtime uses the system libc++ and shares std::vector<std::string>
// code with the instrumented harness, so AddressSanitizer's container-overflow check reports
// libFuzzer's corpus loading as an overflow before the first input runs. Linux builds of libFuzzer
// use a private libc++ and keep the check.

// Default visibility: the build hides symbols, and the runtime looks this one up by name.
extern "C" __attribute__((visibility("default"), used)) const char* __asan_default_options() {
#if defined(__APPLE__)
    return "detect_container_overflow=0";
#else
    return "";
#endif
}
