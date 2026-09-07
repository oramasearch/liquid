#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/cbor.hpp"
#include "common/grammar_registry.hpp"
#include "common/module_support.hpp"
#include "common/syntax.hpp"
#include "liquid/frontend_abi.h"

namespace {

auto parse_from_context(liquid_frontend_bytes_v1 context)
    -> std::optional<std::vector<std::uint8_t>> {
    if (context.size == 0) {
        return std::nullopt;
    }
    const auto decoded = liquid_frontend::cbor_decode({context.data, context.size});
    const auto* value = decoded ? liquid_frontend::cbor_find(*decoded, "parse") : nullptr;
    const auto* bytes =
        value ? std::get_if<liquid_frontend::cbor_value::bytes>(&value->data) : nullptr;
    return bytes ? std::optional<std::vector<std::uint8_t>>{*bytes} : std::nullopt;
}

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
           liquid_frontend_bytes_v1 context,
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
    auto parsed = parse_from_context(context);
    if (!parsed) {
        auto encoded = liquid_frontend::encode_parse_tree(language, {blob.data, blob.size});
        if (!encoded) {
            return fail_syntax(encoded.error(), error);
        }
        parsed = std::move(*encoded);
    }
    auto found = liquid_frontend::parse_units_bytes_from_value(*parsed);
    if (!found) {
        return fail_syntax(found.error(), error);
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "unit discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "units", 1, 1);
    return liquid_frontend::return_bytes(std::move(*found), output);
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
             liquid_frontend_bytes_v1 context,
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
    auto parsed = parse_from_context(context);
    if (!parsed) {
        auto encoded = liquid_frontend::encode_parse_tree(language, {blob.data, blob.size});
        if (!encoded) {
            return fail_syntax(encoded.error(), error);
        }
        parsed = std::move(*encoded);
    }
    auto found = liquid_frontend::parse_symbols_bytes_from_value(*parsed);
    if (!found) {
        return fail_syntax(found.error(), error);
    }
    liquid_frontend::progress(callbacks, "symbols", 1, 1);
    return liquid_frontend::return_bytes(std::move(*found), output);
}

auto parse(void*,
           const char* language,
           liquid_frontend_bytes_v1 blob,
           liquid_frontend_bytes_v1,
           const liquid_frontend_callbacks_v1* callbacks,
           liquid_frontend_owned_bytes_v1* output,
           liquid_frontend_error_v1* error) -> int {
    if (!language || !output || (!blob.data && blob.size != 0)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT, "invalid parse request");
    }
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "parse was cancelled");
    }
    liquid_frontend::progress(callbacks, "parse", 0, 1);
    auto encoded = liquid_frontend::encode_parse_tree(language, {blob.data, blob.size});
    if (!encoded) {
        return fail_syntax(encoded.error(), error);
    }
    liquid_frontend::progress(callbacks, "parse", 1, 1);
    return liquid_frontend::return_bytes(std::move(*encoded), output);
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
    if (liquid_frontend::cancelled(callbacks)) {
        return liquid_frontend::fail(
            error, LIQUID_FRONTEND_STATUS_CANCELLED, "syntax discovery was cancelled");
    }
    liquid_frontend::progress(callbacks, "syntax", 0, 1);
    auto encoded = liquid_frontend::encode_syntax_tree(language, {blob.data, blob.size});
    if (!encoded) {
        return fail_syntax(encoded.error(), error);
    }
    liquid_frontend::progress(callbacks, "syntax", 1, 1);
    return liquid_frontend::return_bytes(std::move(*encoded), output);
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
    syntax,
    parse,
};

} // namespace

#if defined(LIQUID_FRONTEND_STATIC)
extern "C" const liquid_frontend_module_v1* liquid_builtin_tree_sitter_module_v1(void) {
#else
extern "C" LIQUID_FRONTEND_MODULE_EXPORT const liquid_frontend_module_v1*
liquid_frontend_get_module_v1(void) {
#endif
    return &module;
}
