#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/grammar_registry.hpp"
#include "common/module_support.hpp"
#include "common/syntax.hpp"
#include "liquid/frontend_abi.h"

namespace {

auto fail_syntax(const liquid_frontend::syntax_error& failure, liquid_frontend_error_v1* error)
    -> int {
    const auto status = failure.code == liquid_frontend::syntax_error_code::unsupported ||
                                failure.code == liquid_frontend::syntax_error_code::incompatible_abi
                            ? LIQUID_FRONTEND_STATUS_UNSUPPORTED
                            : LIQUID_FRONTEND_STATUS_INTERNAL;
    return liquid_frontend::fail(error, status, failure.message);
}

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
    liquid_frontend::progress(callbacks, "units", 0, 1);
    auto found = liquid_frontend::parse_units(language, {blob.data, blob.size});
    if (!found) {
        return fail_syntax(found.error(), error);
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "unit discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "units", 1, 1);
    return liquid_frontend::return_bytes(liquid_frontend::encode_units(*found), output);
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
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "symbol discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "symbols", 0, 1);
    auto found = liquid_frontend::parse_symbols(language, {blob.data, blob.size});
    if (!found) {
        return fail_syntax(found.error(), error);
    }
    liquid_frontend::progress(callbacks, "symbols", 1, 1);
    return liquid_frontend::return_bytes(
        liquid_frontend::encode_symbols(
            language, "syntactic", liquid_frontend::grammar_lock_digest(), *found),
        output);
}

struct module_state {
    std::vector<std::vector<const char*>> extension_pointers;
    std::vector<liquid_frontend_language_v1> languages;
    std::vector<liquid_frontend_language_metadata_v1> metadata;

    module_state() {
        const auto grammars = liquid_frontend::available_grammars();
        extension_pointers.reserve(grammars.size());
        languages.reserve(grammars.size());
        metadata.reserve(grammars.size());
        for (const auto& grammar : grammars) {
            auto extensions = std::vector<const char*>{};
            extensions.reserve(grammar.extensions.size());
            for (const auto extension : grammar.extensions) {
                extensions.push_back(extension.data());
            }
            extension_pointers.push_back(std::move(extensions));
            const auto& stored = extension_pointers.back();
            languages.push_back({grammar.language.data(), stored.data(), stored.size()});
            metadata.push_back({grammar.language.data(),
                                LIQUID_FRONTEND_TIER_GRAMMAR,
                                liquid_frontend::grammar_lock_digest().data()});
        }
    }
};

const auto state = module_state{};

const liquid_frontend_module_v1 module = {
    LIQUID_FRONTEND_ABI_VERSION,
    sizeof(liquid_frontend_module_v1),
    "tree-sitter-grammars",
    state.languages.data(),
    state.languages.size(),
    nullptr,
    units,
    compile_unit,
    assemble,
    symbols,
    nullptr,
    nullptr,
    state.metadata.data(),
    state.metadata.size(),
};

} // namespace

extern "C" LIQUID_FRONTEND_MODULE_EXPORT const liquid_frontend_module_v1*
liquid_frontend_get_module_v1(void) {
    return &module;
}
