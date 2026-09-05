#ifndef LIQUID_FRONTEND_ABI_H
#define LIQUID_FRONTEND_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIQUID_FRONTEND_ABI_VERSION 1u
#define LIQUID_FRONTEND_ENTRYPOINT_V1 "liquid_frontend_get_module_v1"

typedef enum liquid_frontend_status_v1 {
    LIQUID_FRONTEND_STATUS_OK = 0,
    LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT = 1,
    LIQUID_FRONTEND_STATUS_UNSUPPORTED = 2,
    LIQUID_FRONTEND_STATUS_CANCELLED = 3,
    LIQUID_FRONTEND_STATUS_INTERNAL = 4
} liquid_frontend_status_v1;

#if defined(_WIN32)
#define LIQUID_FRONTEND_MODULE_EXPORT __declspec(dllexport)
#else
#define LIQUID_FRONTEND_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct liquid_frontend_bytes_v1 {
    const uint8_t* data;
    size_t size;
} liquid_frontend_bytes_v1;

typedef void (*liquid_frontend_release_v1)(void* context, uint8_t* data, size_t size);

typedef struct liquid_frontend_owned_bytes_v1 {
    uint8_t* data;
    size_t size;
    liquid_frontend_release_v1 release;
    void* context;
} liquid_frontend_owned_bytes_v1;

typedef struct liquid_frontend_error_v1 {
    uint32_t code;
    const char* message;
} liquid_frontend_error_v1;

typedef void (*liquid_frontend_progress_v1)(void* context,
                                            const char* operation,
                                            uint64_t completed,
                                            uint64_t total);
typedef int (*liquid_frontend_cancelled_v1)(void* context);

typedef struct liquid_frontend_callbacks_v1 {
    void* context;
    liquid_frontend_progress_v1 progress;
    liquid_frontend_cancelled_v1 cancelled;
} liquid_frontend_callbacks_v1;

typedef struct liquid_frontend_language_v1 {
    const char* name;
    const char* const* extensions;
    size_t extension_count;
} liquid_frontend_language_v1;

typedef int (*liquid_frontend_units_v1)(void* module_context,
                                        const char* language,
                                        liquid_frontend_bytes_v1 blob,
                                        liquid_frontend_bytes_v1 context_cbor,
                                        const liquid_frontend_callbacks_v1* callbacks,
                                        liquid_frontend_owned_bytes_v1* units_cbor,
                                        liquid_frontend_error_v1* error);

typedef int (*liquid_frontend_compile_unit_v1)(void* module_context,
                                               liquid_frontend_bytes_v1 unit_cbor,
                                               liquid_frontend_bytes_v1 context_cbor,
                                               const liquid_frontend_callbacks_v1* callbacks,
                                               liquid_frontend_owned_bytes_v1* action_cbor,
                                               liquid_frontend_error_v1* error);

typedef int (*liquid_frontend_assemble_v1)(void* module_context,
                                           liquid_frontend_bytes_v1 unit_outputs_cbor,
                                           liquid_frontend_bytes_v1 context_cbor,
                                           const liquid_frontend_callbacks_v1* callbacks,
                                           liquid_frontend_owned_bytes_v1* output_cbor,
                                           liquid_frontend_error_v1* error);

typedef int (*liquid_frontend_symbols_v1)(void* module_context,
                                          const char* language,
                                          liquid_frontend_bytes_v1 blob,
                                          liquid_frontend_bytes_v1 context_cbor,
                                          const liquid_frontend_callbacks_v1* callbacks,
                                          liquid_frontend_owned_bytes_v1* symbols_cbor,
                                          liquid_frontend_error_v1* error);

typedef int (*liquid_frontend_symbols_from_output_v1)(void* module_context,
                                                      const char* language,
                                                      liquid_frontend_bytes_v1 blob,
                                                      liquid_frontend_bytes_v1 action_output,
                                                      const liquid_frontend_callbacks_v1* callbacks,
                                                      liquid_frontend_owned_bytes_v1* symbols_cbor,
                                                      liquid_frontend_error_v1* error);

typedef void (*liquid_frontend_destroy_v1)(void* module_context);

typedef struct liquid_frontend_module_v1 {
    uint32_t abi_version;
    size_t struct_size;
    const char* name;
    const liquid_frontend_language_v1* languages;
    size_t language_count;
    void* module_context;
    liquid_frontend_units_v1 units;
    liquid_frontend_compile_unit_v1 compile_unit;
    liquid_frontend_assemble_v1 assemble;
    liquid_frontend_symbols_v1 symbols;
    liquid_frontend_symbols_from_output_v1 symbols_from_output;
    liquid_frontend_destroy_v1 destroy;
} liquid_frontend_module_v1;

typedef const liquid_frontend_module_v1* (*liquid_frontend_entrypoint_v1)(void);

#ifdef __cplusplus
}
#endif

#endif
