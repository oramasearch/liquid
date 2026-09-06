#include "liquid/frontend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto frontend_error(errc code, std::string message, std::string context) -> error {
    return error{code, std::move(message), std::move(context)};
}

auto status_code(std::uint32_t status) -> errc {
    switch (status) {
    case LIQUID_FRONTEND_STATUS_INVALID_ARGUMENT:
        return errc::invalid_argument;
    case LIQUID_FRONTEND_STATUS_UNSUPPORTED:
        return errc::unsupported;
    case LIQUID_FRONTEND_STATUS_CANCELLED:
        return errc::unavailable;
    default:
        return errc::internal;
    }
}

auto find(const cbor::value& value, std::string_view key) -> const cbor::value* {
    const auto* object = std::get_if<cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    for (const auto& [candidate, child] : *object) {
        const auto* text = std::get_if<std::string>(&candidate.data);
        if (text && *text == key) {
            return &child;
        }
    }
    return nullptr;
}

auto unsigned_field(const cbor::value& value, std::string_view key) -> result<std::uint64_t> {
    const auto* child = find(value, key);
    const auto* number = child ? std::get_if<std::uint64_t>(&child->data) : nullptr;
    if (!number) {
        return std::unexpected(frontend_error(
            errc::corrupt, "frontend unit has an invalid integer field", key.data()));
    }
    return *number;
}

auto canonical_value(std::span<const std::byte> bytes, std::string_view context)
    -> result<cbor::value> {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (encoded->size() != bytes.size() ||
        !std::equal(encoded->begin(), encoded->end(), bytes.begin())) {
        return std::unexpected(frontend_error(
            errc::corrupt, "frontend returned non-canonical CBOR", std::string{context}));
    }
    return std::move(*decoded);
}

struct callback_bridge {
    frontend_callbacks* callbacks{};

    static void
    progress(void* context, const char* operation, std::uint64_t completed, std::uint64_t total) {
        auto* self = static_cast<callback_bridge*>(context);
        if (!self->callbacks->progress) {
            return;
        }
        self->callbacks->progress(operation ? operation : "", completed, total);
    }

    static int cancelled(void* context) {
        auto* self = static_cast<callback_bridge*>(context);
        if (!self->callbacks->cancelled) {
            return 0;
        }
        return self->callbacks->cancelled() ? 1 : 0;
    }
};

auto abi_callbacks(frontend_callbacks& callbacks, callback_bridge& bridge)
    -> liquid_frontend_callbacks_v1 {
    bridge.callbacks = &callbacks;
    return liquid_frontend_callbacks_v1{
        .context = &bridge,
        .progress = callback_bridge::progress,
        .cancelled = callback_bridge::cancelled,
    };
}

class owned_abi_bytes {
public:
    explicit owned_abi_bytes(liquid_frontend_owned_bytes_v1 value) : value_(value) {}

    ~owned_abi_bytes() {
        if (value_.release && value_.data) {
            value_.release(value_.context, value_.data, value_.size);
        }
    }

    owned_abi_bytes(const owned_abi_bytes&) = delete;
    auto operator=(const owned_abi_bytes&) -> owned_abi_bytes& = delete;

    auto copy() const -> std::vector<std::byte> {
        auto output = std::vector<std::byte>(value_.size);
        if (!output.empty()) {
            std::memcpy(output.data(), value_.data, output.size());
        }
        return output;
    }

private:
    liquid_frontend_owned_bytes_v1 value_;
};

auto copy_output(liquid_frontend_owned_bytes_v1 output, std::string_view module_name)
    -> result<std::vector<std::byte>> {
    if (!output.data || !output.release) {
        return std::unexpected(frontend_error(
            errc::corrupt, "frontend returned invalid owned bytes", std::string{module_name}));
    }
    auto owned = owned_abi_bytes{output};
    return owned.copy();
}

auto validate_input(std::span<const std::byte> bytes, std::string_view context) -> result<void> {
    if (bytes.empty()) {
        return {};
    }
    auto value = canonical_value(bytes, context);
    if (!value) {
        return std::unexpected(value.error());
    }
    return {};
}

auto parse_position(const cbor::value& value) -> result<frontend_source_position> {
    auto byte = unsigned_field(value, "byte");
    auto line = unsigned_field(value, "line");
    auto column = unsigned_field(value, "column");
    if (!byte) {
        return std::unexpected(byte.error());
    }
    if (!line) {
        return std::unexpected(line.error());
    }
    if (!column) {
        return std::unexpected(column.error());
    }
    return frontend_source_position{*byte, *line, *column};
}

auto parse_unit(const cbor::value& value, std::vector<std::byte> canonical)
    -> result<frontend_unit> {
    const auto* kind_value = find(value, "kind");
    const auto* name_value = find(value, "name");
    const auto* range_value = find(value, "range");
    const auto* source_value = find(value, "source");
    const auto* kind = kind_value ? std::get_if<std::uint64_t>(&kind_value->data) : nullptr;
    const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
    const auto* source =
        source_value ? std::get_if<cbor::value::bytes>(&source_value->data) : nullptr;
    const auto* start_value = range_value ? find(*range_value, "start") : nullptr;
    const auto* end_value = range_value ? find(*range_value, "end") : nullptr;
    if (!kind || *kind < 1 || *kind > 3 || !name || name->empty() || !source || !start_value ||
        !end_value) {
        return std::unexpected(
            frontend_error(errc::corrupt, "frontend returned an invalid unit", "frontend units"));
    }
    auto start = parse_position(*start_value);
    auto end = parse_position(*end_value);
    if (!start) {
        return std::unexpected(start.error());
    }
    if (!end) {
        return std::unexpected(end.error());
    }
    if (end->byte < start->byte) {
        return std::unexpected(
            frontend_error(errc::corrupt, "frontend unit range is reversed", "frontend units"));
    }
    auto identity = cbor::value::map{};
    identity.emplace_back("kind", *kind);
    identity.emplace_back("name", *name);
    identity.emplace_back("source", *source);
    auto identity_bytes = cbor::encode(cbor::value{std::move(identity)});
    if (!identity_bytes) {
        return std::unexpected(identity_bytes.error());
    }
    auto fingerprint = hash(algorithm::blake3, *identity_bytes);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return frontend_unit{
        .kind = static_cast<frontend_unit_kind>(*kind),
        .name = *name,
        .range = frontend_source_range{*start, *end},
        .fingerprint = *fingerprint,
        .canonical_value = std::move(canonical),
    };
}

} // namespace

struct frontend_module::impl {
    const liquid_frontend_module_v1* descriptor{};
    void* library{};
    bool owns_library{};

    ~impl() {
        if (descriptor && descriptor->destroy) {
            descriptor->destroy(descriptor->module_context);
        }
        if (!owns_library || !library) {
            return;
        }
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
    }
};

frontend_module::frontend_module() noexcept = default;
frontend_module::frontend_module(std::unique_ptr<impl> implementation)
    : impl_(std::move(implementation)) {}
frontend_module::~frontend_module() = default;
frontend_module::frontend_module(frontend_module&&) noexcept = default;
auto frontend_module::operator=(frontend_module&&) noexcept -> frontend_module& = default;

auto frontend_module::linked(const liquid_frontend_module_v1* descriptor)
    -> result<frontend_module> {
    constexpr auto base_descriptor_size = offsetof(liquid_frontend_module_v1, language_metadata);
    if (!descriptor || descriptor->abi_version != LIQUID_FRONTEND_ABI_VERSION ||
        descriptor->struct_size < base_descriptor_size || !descriptor->name ||
        descriptor->language_count == 0 || !descriptor->languages || !descriptor->units ||
        !descriptor->compile_unit || !descriptor->assemble || !descriptor->symbols) {
        return std::unexpected(frontend_error(
            errc::invalid_argument, "frontend descriptor does not implement ABI v1", "frontend"));
    }
    for (std::size_t index = 0; index < descriptor->language_count; ++index) {
        const auto& language = descriptor->languages[index];
        if (!language.name || language.name[0] == '\0' ||
            (language.extension_count != 0 && !language.extensions)) {
            return std::unexpected(frontend_error(
                errc::invalid_argument, "frontend has an invalid language", descriptor->name));
        }
    }
    constexpr auto metadata_size = offsetof(liquid_frontend_module_v1, syntax);
    if (descriptor->struct_size >= metadata_size) {
        if (descriptor->language_metadata_count != 0 && !descriptor->language_metadata) {
            return std::unexpected(frontend_error(errc::invalid_argument,
                                                  "frontend has invalid language metadata",
                                                  descriptor->name));
        }
        for (std::size_t index = 0; index < descriptor->language_metadata_count; ++index) {
            const auto& metadata = descriptor->language_metadata[index];
            if (!metadata.language ||
                (metadata.tier_mechanism != LIQUID_FRONTEND_TIER_FRONTEND &&
                 metadata.tier_mechanism != LIQUID_FRONTEND_TIER_GRAMMAR) ||
                (metadata.tier_mechanism == LIQUID_FRONTEND_TIER_GRAMMAR &&
                 (!metadata.grammar_digest || metadata.grammar_digest[0] == '\0'))) {
                return std::unexpected(frontend_error(errc::invalid_argument,
                                                      "frontend has invalid language metadata",
                                                      descriptor->name));
            }
        }
    }
    auto implementation = std::make_unique<impl>();
    implementation->descriptor = descriptor;
    return frontend_module{std::move(implementation)};
}

auto frontend_module::syntax(std::string_view language,
                             std::span<const std::byte> blob,
                             std::span<const std::byte> context_cbor,
                             frontend_callbacks callbacks) const -> result<syntax_tree> {
    constexpr auto syntax_size =
        offsetof(liquid_frontend_module_v1, syntax) + sizeof(liquid_frontend_module_v1::syntax);
    if (!impl_ || !supports(language) || impl_->descriptor->struct_size < syntax_size ||
        !impl_->descriptor->syntax) {
        return std::unexpected(frontend_error(
            errc::unsupported, "frontend does not expose concrete syntax", std::string{language}));
    }
    auto valid_context = validate_input(context_cbor, "frontend syntax context");
    if (!valid_context) {
        return std::unexpected(valid_context.error());
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto language_text = std::string{language};
    const auto status = impl_->descriptor->syntax(
        impl_->descriptor->module_context,
        language_text.c_str(),
        {reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()},
        {reinterpret_cast<const std::uint8_t*>(context_cbor.data()), context_cbor.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "frontend syntax failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    return decode_syntax_tree(*copied);
}

auto frontend_module::load(std::filesystem::path shared_object) -> result<frontend_module> {
    void* library{};
#if defined(_WIN32)
    library = LoadLibraryW(shared_object.wstring().c_str());
    if (!library) {
        return std::unexpected(frontend_error(
            errc::not_found, "cannot load frontend shared library", shared_object.string()));
    }
    auto entrypoint = reinterpret_cast<liquid_frontend_entrypoint_v1>(
        GetProcAddress(static_cast<HMODULE>(library), LIQUID_FRONTEND_ENTRYPOINT_V1));
#else
    library = dlopen(shared_object.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        const auto* message = dlerror();
        return std::unexpected(frontend_error(
            errc::not_found, message ? message : "cannot load frontend", shared_object.string()));
    }
    auto entrypoint = reinterpret_cast<liquid_frontend_entrypoint_v1>(
        dlsym(library, LIQUID_FRONTEND_ENTRYPOINT_V1));
#endif
    if (!entrypoint) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return std::unexpected(frontend_error(
            errc::invalid_argument, "frontend entry point is missing", shared_object.string()));
    }
    auto linked_module = linked(entrypoint());
    if (!linked_module) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return std::unexpected(linked_module.error());
    }
    linked_module->impl_->library = library;
    linked_module->impl_->owns_library = true;
    return linked_module;
}

auto frontend_module::name() const -> std::string_view {
    return impl_ ? impl_->descriptor->name : "";
}

auto frontend_module::languages() const -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    if (!impl_) {
        return output;
    }
    output.reserve(impl_->descriptor->language_count);
    for (std::size_t index = 0; index < impl_->descriptor->language_count; ++index) {
        output.emplace_back(impl_->descriptor->languages[index].name);
    }
    return output;
}

auto frontend_module::language_details() const -> std::vector<frontend_language> {
    auto output = std::vector<frontend_language>{};
    if (!impl_) {
        return output;
    }
    const auto* descriptor = impl_->descriptor;
    constexpr auto metadata_size = offsetof(liquid_frontend_module_v1, syntax);
    const auto has_metadata = descriptor->struct_size >= metadata_size;
    output.reserve(descriptor->language_count);
    for (std::size_t index = 0; index < descriptor->language_count; ++index) {
        const auto& registered = descriptor->languages[index];
        auto value = frontend_language{.language = registered.name, .module = descriptor->name};
        value.extensions.reserve(registered.extension_count);
        for (std::size_t extension = 0; extension < registered.extension_count; ++extension) {
            value.extensions.emplace_back(registered.extensions[extension]);
        }
        if (has_metadata) {
            for (std::size_t metadata_index = 0;
                 metadata_index < descriptor->language_metadata_count;
                 ++metadata_index) {
                const auto& metadata = descriptor->language_metadata[metadata_index];
                if (value.language != metadata.language) {
                    continue;
                }
                value.tier_mechanism = metadata.tier_mechanism == LIQUID_FRONTEND_TIER_GRAMMAR
                                           ? frontend_tier_mechanism::grammar
                                           : frontend_tier_mechanism::frontend;
                if (metadata.grammar_digest) {
                    value.grammar_digest = metadata.grammar_digest;
                }
                break;
            }
        }
        output.push_back(std::move(value));
    }
    return output;
}

auto frontend_module::supports(std::string_view language) const -> bool {
    const auto supported = languages();
    return std::ranges::find(supported, language) != supported.end();
}

auto frontend_module::units(std::string_view language,
                            std::span<const std::byte> blob,
                            std::span<const std::byte> context_cbor,
                            frontend_callbacks callbacks) const
    -> result<std::vector<frontend_unit>> {
    if (!impl_ || !supports(language)) {
        return std::unexpected(frontend_error(errc::unsupported,
                                              "frontend does not support the requested language",
                                              std::string{language}));
    }
    auto valid_context = validate_input(context_cbor, "frontend units context");
    if (!valid_context) {
        return std::unexpected(valid_context.error());
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto language_text = std::string{language};
    const auto status = impl_->descriptor->units(
        impl_->descriptor->module_context,
        language_text.c_str(),
        {reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()},
        {reinterpret_cast<const std::uint8_t*>(context_cbor.data()), context_cbor.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "frontend units failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto bytes = std::move(*copied);
    auto decoded = canonical_value(bytes, "frontend units");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* array = std::get_if<cbor::value::array>(&decoded->data);
    if (!array) {
        return std::unexpected(frontend_error(
            errc::corrupt, "frontend unit list is not an array", std::string{name()}));
    }
    auto result_units = std::vector<frontend_unit>{};
    result_units.reserve(array->size());
    for (const auto& item : *array) {
        auto item_bytes = cbor::encode(item);
        if (!item_bytes) {
            return std::unexpected(item_bytes.error());
        }
        auto unit = parse_unit(item, std::move(*item_bytes));
        if (!unit) {
            return std::unexpected(unit.error());
        }
        result_units.push_back(std::move(*unit));
    }
    return result_units;
}

auto frontend_module::compile_unit(const frontend_unit& unit,
                                   std::span<const std::byte> context_cbor,
                                   frontend_callbacks callbacks) const
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend module is empty", "frontend"));
    }
    auto valid_unit = validate_input(unit.canonical_value, "frontend unit");
    if (!valid_unit) {
        return std::unexpected(valid_unit.error());
    }
    auto valid_context = validate_input(context_cbor, "frontend compile context");
    if (!valid_context) {
        return std::unexpected(valid_context.error());
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto status = impl_->descriptor->compile_unit(
        impl_->descriptor->module_context,
        {reinterpret_cast<const std::uint8_t*>(unit.canonical_value.data()),
         unit.canonical_value.size()},
        {reinterpret_cast<const std::uint8_t*>(context_cbor.data()), context_cbor.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "compile_unit failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto bytes = std::move(*copied);
    if (auto canonical = canonical_value(bytes, "frontend action"); !canonical) {
        return std::unexpected(canonical.error());
    }
    return bytes;
}

auto frontend_module::assemble(std::span<const std::byte> unit_outputs_cbor,
                               std::span<const std::byte> context_cbor,
                               frontend_callbacks callbacks) const
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected(
            frontend_error(errc::invalid_argument, "frontend module is empty", "frontend"));
    }
    auto valid_outputs = validate_input(unit_outputs_cbor, "frontend unit outputs");
    if (!valid_outputs) {
        return std::unexpected(valid_outputs.error());
    }
    auto valid_context = validate_input(context_cbor, "frontend assembly context");
    if (!valid_context) {
        return std::unexpected(valid_context.error());
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto status = impl_->descriptor->assemble(
        impl_->descriptor->module_context,
        {reinterpret_cast<const std::uint8_t*>(unit_outputs_cbor.data()), unit_outputs_cbor.size()},
        {reinterpret_cast<const std::uint8_t*>(context_cbor.data()), context_cbor.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "assemble failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto bytes = std::move(*copied);
    if (auto canonical = canonical_value(bytes, "frontend translation-unit output"); !canonical) {
        return std::unexpected(canonical.error());
    }
    return bytes;
}

auto frontend_module::symbols(std::string_view language,
                              std::span<const std::byte> blob,
                              std::span<const std::byte> context_cbor,
                              frontend_callbacks callbacks) const
    -> result<std::vector<std::byte>> {
    if (!impl_ || !supports(language)) {
        return std::unexpected(frontend_error(errc::unsupported,
                                              "frontend does not support the requested language",
                                              std::string{language}));
    }
    auto valid_context = validate_input(context_cbor, "frontend symbols context");
    if (!valid_context) {
        return std::unexpected(valid_context.error());
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto language_text = std::string{language};
    const auto status = impl_->descriptor->symbols(
        impl_->descriptor->module_context,
        language_text.c_str(),
        {reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()},
        {reinterpret_cast<const std::uint8_t*>(context_cbor.data()), context_cbor.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "frontend symbols failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto bytes = std::move(*copied);
    if (auto canonical = canonical_value(bytes, "frontend symbols"); !canonical) {
        return std::unexpected(canonical.error());
    }
    return bytes;
}

auto frontend_module::symbols_from_output(std::string_view language,
                                          std::span<const std::byte> blob,
                                          std::span<const std::byte> action_output,
                                          frontend_callbacks callbacks) const
    -> result<std::vector<std::byte>> {
    if (!impl_ || !supports(language) || !impl_->descriptor->symbols_from_output) {
        return std::unexpected(frontend_error(errc::unsupported,
                                              "frontend cannot decode symbol action output",
                                              std::string{language}));
    }
    auto bridge = callback_bridge{};
    auto abi = abi_callbacks(callbacks, bridge);
    auto output = liquid_frontend_owned_bytes_v1{};
    auto abi_error = liquid_frontend_error_v1{};
    const auto language_text = std::string{language};
    const auto status = impl_->descriptor->symbols_from_output(
        impl_->descriptor->module_context,
        language_text.c_str(),
        {reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()},
        {reinterpret_cast<const std::uint8_t*>(action_output.data()), action_output.size()},
        &abi,
        &output,
        &abi_error);
    if (status != LIQUID_FRONTEND_STATUS_OK) {
        return std::unexpected(
            frontend_error(status_code(status),
                           abi_error.message ? abi_error.message : "symbol action decoding failed",
                           std::string{name()}));
    }
    auto copied = copy_output(output, name());
    if (!copied) {
        return std::unexpected(copied.error());
    }
    auto bytes = std::move(*copied);
    if (auto canonical = canonical_value(bytes, "frontend symbols"); !canonical) {
        return std::unexpected(canonical.error());
    }
    return bytes;
}

struct frontend_registry::impl {
    std::vector<frontend_module> modules;
};

frontend_registry::frontend_registry() : impl_(std::make_unique<impl>()) {}
frontend_registry::~frontend_registry() = default;
frontend_registry::frontend_registry(frontend_registry&&) noexcept = default;
auto frontend_registry::operator=(frontend_registry&&) noexcept -> frontend_registry& = default;

auto frontend_registry::global() -> frontend_registry& {
    static auto value = frontend_registry{};
    return value;
}

auto frontend_registry::add(frontend_module module) -> result<void> {
    if (module.name().empty()) {
        return std::unexpected(frontend_error(
            errc::invalid_argument, "frontend module is empty", "frontend registry"));
    }
    const auto duplicate = std::ranges::find_if(
        impl_->modules, [&](const auto& registered) { return registered.name() == module.name(); });
    if (duplicate != impl_->modules.end()) {
        return std::unexpected(frontend_error(errc::already_exists,
                                              "frontend module is already registered",
                                              std::string{module.name()}));
    }
    impl_->modules.push_back(std::move(module));
    return {};
}

auto frontend_registry::load(std::filesystem::path shared_object) -> result<void> {
    auto module = frontend_module::load(std::move(shared_object));
    if (!module) {
        return std::unexpected(module.error());
    }
    return add(std::move(*module));
}

auto frontend_registry::find(std::string_view language) const -> result<const frontend_module*> {
    const auto* found = static_cast<const frontend_module*>(nullptr);
    auto found_tier = frontend_tier_mechanism::grammar;
    for (const auto& module : impl_->modules) {
        const auto details = module.language_details();
        const auto detail = std::ranges::find(details, language, &frontend_language::language);
        if (detail == details.end()) {
            continue;
        }
        if (!found || detail->tier_mechanism == frontend_tier_mechanism::frontend) {
            found = &module;
            found_tier = detail->tier_mechanism;
        }
        if (found_tier == frontend_tier_mechanism::frontend) {
            break;
        }
    }
    if (!found) {
        return std::unexpected(frontend_error(
            errc::not_found, "no frontend is registered for the language", std::string{language}));
    }
    return found;
}

auto frontend_registry::languages() const -> std::vector<frontend_language> {
    auto output = std::vector<frontend_language>{};
    for (const auto& module : impl_->modules) {
        auto registered = module.language_details();
        for (auto& language : registered) {
            auto current =
                std::ranges::find(output, language.language, &frontend_language::language);
            if (current == output.end()) {
                output.push_back(std::move(language));
                continue;
            }
            current->extensions.insert(
                current->extensions.end(), language.extensions.begin(), language.extensions.end());
            std::ranges::sort(current->extensions);
            current->extensions.erase(
                std::unique(current->extensions.begin(), current->extensions.end()),
                current->extensions.end());
            if (language.tier_mechanism == frontend_tier_mechanism::frontend) {
                current->tier_mechanism = language.tier_mechanism;
                current->grammar_digest = std::move(language.grammar_digest);
                current->module = std::move(language.module);
            }
        }
    }
    std::ranges::sort(output, {}, &frontend_language::language);
    return output;
}

} // namespace liquid
