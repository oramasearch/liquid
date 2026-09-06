#ifndef LIQUID_HOT_ABI_H
#define LIQUID_HOT_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(LIQUID_HOT_BUILDING)
#define LIQUID_HOT_API __declspec(dllexport)
#else
#define LIQUID_HOT_API __declspec(dllimport)
#endif
#else
#define LIQUID_HOT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LIQUID_HOT_ABI_VERSION 1u

typedef enum liquid_hot_status {
    LIQUID_HOT_OK = 0,
    LIQUID_HOT_INVALID = 1,
    LIQUID_HOT_UNSUPPORTED = 2,
    LIQUID_HOT_IO = 3,
    LIQUID_HOT_SYMBOL = 4
} liquid_hot_status;

LIQUID_HOT_API uint32_t liquid_hot_abi_version(void);
LIQUID_HOT_API liquid_hot_status liquid_hot_register(const char* symbol, void** entry_slot);
LIQUID_HOT_API liquid_hot_status liquid_hot_listen(const char* endpoint);
LIQUID_HOT_API liquid_hot_status liquid_hot_apply(const void* patch_bytes, size_t patch_size);
LIQUID_HOT_API liquid_hot_status liquid_hot_quiescent(void);
LIQUID_HOT_API void liquid_hot_close(void);
LIQUID_HOT_API const char* liquid_hot_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
