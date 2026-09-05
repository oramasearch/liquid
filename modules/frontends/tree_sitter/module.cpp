#include <span>
#include <string_view>

#include "common/module_support.hpp"
#include "common/syntax.hpp"
#include "liquid/frontend_abi.h"

namespace {

auto units(void*,
           const char* language,
           liquid_frontend_bytes_v1 blob,
           liquid_frontend_bytes_v1,
           const liquid_frontend_callbacks_v1* callbacks,
           liquid_frontend_owned_bytes_v1* output,
           liquid_frontend_error_v1* error) -> int {
    if (!language || !output || (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "invalid units request");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "unit discovery was cancelled");
    }
    const auto name = std::string_view{language};
    auto syntax = liquid_frontend::syntax_language::c;
    if (name == "c++" || name == "cpp") {
        syntax = liquid_frontend::syntax_language::cpp;
    } else if (name != "c") {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_UNSUPPORTED, "language is not supported");
    }
    liquid_frontend::progress(callbacks, "units", 0, 1);
    const auto found = liquid_frontend::parse_units(syntax, {blob.data, blob.size});
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "unit discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "units", 1, 1);
    return liquid_frontend::return_bytes(liquid_frontend::encode_units(found), output);
}

auto compile_unit(void*,
                  liquid_frontend_bytes_v1,
                  liquid_frontend_bytes_v1,
                  const liquid_frontend_callbacks_v1*,
                  liquid_frontend_owned_bytes_v1*,
                  liquid_frontend_error_v1* error) -> int {
    return liquid_frontend::fail(error,
                                 LIQUID_FRONTEND_STATUS_UNSUPPORTED,
                                 "tree-sitter provides syntax units but no compile action");
}

auto assemble(void*,
              liquid_frontend_bytes_v1,
              liquid_frontend_bytes_v1,
              const liquid_frontend_callbacks_v1*,
              liquid_frontend_owned_bytes_v1*,
              liquid_frontend_error_v1* error) -> int {
    return liquid_frontend::fail(error,
                                 LIQUID_FRONTEND_STATUS_UNSUPPORTED,
                                 "tree-sitter provides syntax units but no assembly");
}

auto symbols(void*,
             const char* language,
             liquid_frontend_bytes_v1 blob,
             liquid_frontend_bytes_v1,
             const liquid_frontend_callbacks_v1* callbacks,
             liquid_frontend_owned_bytes_v1* output,
             liquid_frontend_error_v1* error) -> int {
    if (!language || !output || (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "invalid symbols request");
    }
    auto syntax = liquid_frontend::syntax_language::c;
    const auto name = std::string_view{language};
    if (name == "c++" || name == "cpp") {
        syntax = liquid_frontend::syntax_language::cpp;
    } else if (name != "c") {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_UNSUPPORTED, "language is not supported");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "symbol discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "symbols", 0, 1);
    const auto found = liquid_frontend::parse_symbols(syntax, {blob.data, blob.size});
    liquid_frontend::progress(callbacks, "symbols", 1, 1);
    return liquid_frontend::return_bytes(
        liquid_frontend::encode_symbols(syntax, "syntactic", found), output);
}

auto syntax(void*,
            const char* language,
            liquid_frontend_bytes_v1 blob,
            liquid_frontend_bytes_v1,
            const liquid_frontend_callbacks_v1* callbacks,
            liquid_frontend_owned_bytes_v1* output,
            liquid_frontend_error_v1* error) -> int {
    if (!language || !output || (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "invalid syntax request");
    }
    auto syntax_language = liquid_frontend::syntax_language::c;
    const auto name = std::string_view{language};
    if (name == "c++" || name == "cpp") {
        syntax_language = liquid_frontend::syntax_language::cpp;
    } else if (name != "c") {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_UNSUPPORTED, "language is not supported");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "syntax discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "syntax", 0, 1);
    auto encoded = liquid_frontend::encode_syntax_tree(syntax_language, {blob.data, blob.size});
    if (encoded.empty()) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INTERNAL, "tree-sitter could not parse syntax");
    }
    liquid_frontend::progress(callbacks, "syntax", 1, 1);
    return liquid_frontend::return_bytes(std::move(encoded), output);
}

constexpr const char* c_extensions[] = {".c", ".h"};
constexpr const char* cpp_extensions[] = {".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"};
constexpr liquid_frontend_language_v1 languages[] = {
    {"c", c_extensions, 2},
    {"c++", cpp_extensions, 6},
};

const liquid_frontend_module_v1 module = {
    LIQUID_FRONTEND_ABI_VERSION,
    sizeof(liquid_frontend_module_v1),
    "tree-sitter-c-cpp",
    languages,
    2,
    nullptr,
    units,
    compile_unit,
    assemble,
    symbols,
    nullptr,
    nullptr,
    syntax,
};

} // namespace

extern "C" LIQUID_FRONTEND_MODULE_EXPORT const liquid_frontend_module_v1*
liquid_frontend_get_module_v1(void) {
    return &module;
}
