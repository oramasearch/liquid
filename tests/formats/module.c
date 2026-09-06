#include "liquid/format_abi.h"

static int unsupported(void* module_context,
                       liquid_format_bytes_v1 input,
                       const liquid_format_callbacks_v1* callbacks,
                       liquid_format_owned_bytes_v1* output,
                       liquid_format_error_v1* error) {
    (void)module_context;
    (void)input;
    (void)callbacks;
    (void)output;
    error->code = LIQUID_FORMAT_STATUS_UNSUPPORTED;
    error->message = "test module operation is unsupported";
    return LIQUID_FORMAT_STATUS_UNSUPPORTED;
}

static const char* extensions[] = {".fixture"};

static const liquid_format_module_v1 module = {
    .abi_version = LIQUID_FORMAT_ABI_VERSION,
    .struct_size = sizeof(liquid_format_module_v1),
    .descriptor =
        {
            .name = "test-module",
            .tier = 2,
            .extensions = extensions,
            .extension_count = 1,
            .limits =
                {
                    .maximum_depth = 2,
                    .byte_budget = 4096,
                    .maximum_entries = 16,
                },
        },
    .recognise = unsupported,
    .unpack = unsupported,
    .canonicalise = unsupported,
    .diff = unsupported,
};

LIQUID_FORMAT_MODULE_EXPORT const liquid_format_module_v1* liquid_format_get_module_v1(void) {
    return &module;
}
