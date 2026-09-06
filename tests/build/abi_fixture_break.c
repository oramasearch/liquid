#if defined(_WIN32)
#define LIQUID_ABI_EXPORT __declspec(dllexport)
#else
#define LIQUID_ABI_EXPORT __attribute__((visibility("default")))
#endif

LIQUID_ABI_EXPORT int liquid_abi_retained(double value) {
    return (int)value + 1;
}

LIQUID_ABI_EXPORT long long liquid_abi_object = 7;
