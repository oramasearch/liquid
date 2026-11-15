// SPDX-License-Identifier: MIT

#ifndef LIQUID_FORMAT_ABI_H
#define LIQUID_FORMAT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIQUID_FORMAT_ABI_VERSION 1u
#define LIQUID_FORMAT_ENTRYPOINT_V1 "liquid_format_get_module_v1"

typedef enum liquid_format_status_v1 {
    LIQUID_FORMAT_STATUS_OK = 0,
    LIQUID_FORMAT_STATUS_INVALID_ARGUMENT = 1,
    LIQUID_FORMAT_STATUS_UNSUPPORTED = 2,
    LIQUID_FORMAT_STATUS_CANCELLED = 3,
    LIQUID_FORMAT_STATUS_CORRUPT = 4,
    LIQUID_FORMAT_STATUS_INTERNAL = 5
} liquid_format_status_v1;

#if defined(_WIN32)
#define LIQUID_FORMAT_MODULE_EXPORT __declspec(dllexport)
#else
#define LIQUID_FORMAT_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct liquid_format_bytes_v1 {
    const uint8_t* data;
    size_t size;
} liquid_format_bytes_v1;

typedef void (*liquid_format_release_v1)(void* context, uint8_t* data, size_t size);

typedef struct liquid_format_owned_bytes_v1 {
    uint8_t* data;
    size_t size;
    liquid_format_release_v1 release;
    void* context;
} liquid_format_owned_bytes_v1;

typedef struct liquid_format_error_v1 {
    uint32_t code;
    const char* message;
} liquid_format_error_v1;

typedef void (*liquid_format_progress_v1)(void* context,
                                          const char* operation,
                                          uint64_t completed,
                                          uint64_t total);
typedef int (*liquid_format_cancelled_v1)(void* context);

typedef struct liquid_format_callbacks_v1 {
    void* context;
    liquid_format_progress_v1 progress;
    liquid_format_cancelled_v1 cancelled;
} liquid_format_callbacks_v1;

typedef struct liquid_format_magic_v1 {
    const uint8_t* data;
    size_t size;
} liquid_format_magic_v1;

typedef struct liquid_format_limits_v1 {
    uint32_t maximum_depth;
    uint64_t byte_budget;
    uint64_t maximum_entries;
} liquid_format_limits_v1;

typedef struct liquid_format_descriptor_v1 {
    const char* name;
    uint32_t tier;
    const char* const* extensions;
    size_t extension_count;
    const liquid_format_magic_v1* magic;
    size_t magic_count;
    const char* const* volatile_fields;
    size_t volatile_field_count;
    liquid_format_limits_v1 limits;
} liquid_format_descriptor_v1;

typedef int (*liquid_format_operation_v1)(void* module_context,
                                          liquid_format_bytes_v1 input_cbor,
                                          const liquid_format_callbacks_v1* callbacks,
                                          liquid_format_owned_bytes_v1* output_cbor,
                                          liquid_format_error_v1* error);

typedef void (*liquid_format_destroy_v1)(void* module_context);

typedef struct liquid_format_module_v1 {
    uint32_t abi_version;
    size_t struct_size;
    liquid_format_descriptor_v1 descriptor;
    void* module_context;
    liquid_format_operation_v1 recognise;
    liquid_format_operation_v1 unpack;
    liquid_format_operation_v1 canonicalise;
    liquid_format_operation_v1 diff;
    liquid_format_operation_v1 merge;
    liquid_format_operation_v1 repack;
    liquid_format_destroy_v1 destroy;
} liquid_format_module_v1;

typedef const liquid_format_module_v1* (*liquid_format_entrypoint_v1)(void);

#ifdef __cplusplus
}
#endif

#endif
