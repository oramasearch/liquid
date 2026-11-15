// SPDX-License-Identifier: MIT

#if defined(_WIN32)
#define DEMO_EXPORT __declspec(dllexport)
#else
#define DEMO_EXPORT __attribute__((visibility("default")))
#endif

DEMO_EXPORT int demo_value(void) {
    return 2;
}

DEMO_EXPORT int demo_added(void) {
    return 3;
}
