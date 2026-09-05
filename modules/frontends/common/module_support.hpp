#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "liquid/frontend_abi.h"

namespace liquid_frontend {

inline void release_bytes(void*, std::uint8_t* data, std::size_t) {
    std::free(data);
}

inline auto return_bytes(std::vector<std::uint8_t> bytes, liquid_frontend_owned_bytes_v1* output)
    -> int {
    auto* data = static_cast<std::uint8_t*>(std::malloc(bytes.size() == 0 ? 1 : bytes.size()));
    if (!data) {
        return LIQUID_FRONTEND_STATUS_INTERNAL;
    }
    if (!bytes.empty()) {
        std::memcpy(data, bytes.data(), bytes.size());
    }
    *output = liquid_frontend_owned_bytes_v1{
        .data = data,
        .size = bytes.size(),
        .release = release_bytes,
        .context = nullptr,
    };
    return LIQUID_FRONTEND_STATUS_OK;
}

inline auto fail(liquid_frontend_error_v1* error, std::uint32_t code, std::string message) -> int {
    static thread_local auto storage = std::string{};
    storage = std::move(message);
    if (error) {
        error->code = code;
        error->message = storage.c_str();
    }
    return static_cast<int>(code);
}

inline auto cancelled(const liquid_frontend_callbacks_v1* callbacks) -> bool {
    return callbacks && callbacks->cancelled && callbacks->cancelled(callbacks->context) != 0;
}

inline void progress(const liquid_frontend_callbacks_v1* callbacks,
                     const char* operation,
                     std::uint64_t completed,
                     std::uint64_t total) {
    if (callbacks && callbacks->progress) {
        callbacks->progress(callbacks->context, operation, completed, total);
    }
}

} // namespace liquid_frontend
