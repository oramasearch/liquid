#include "liquid/format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "encoding/cbor.hpp"
#include "formats/builtin.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto format_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto bytes(std::span<const std::byte> value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&source.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, value] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto digest_value(const digest& value) -> cbor::value {
    return map({{"alg", static_cast<std::uint64_t>(value.alg)}, {"bytes", bytes(value.bytes)}});
}

auto decode_digest(const cbor::value& value) -> result<digest> {
    const auto* algorithm_value = field(value, "alg");
    const auto* bytes_value = field(value, "bytes");
    const auto* algorithm_number =
        algorithm_value ? std::get_if<std::uint64_t>(&algorithm_value->data) : nullptr;
    const auto* raw = bytes_value ? std::get_if<cbor::value::bytes>(&bytes_value->data) : nullptr;
    if (!algorithm_number || *algorithm_number < 1 || *algorithm_number > 3 || !raw ||
        raw->size() != 32) {
        return std::unexpected(format_error(errc::corrupt, "invalid format entry digest"));
    }
    auto output = digest{static_cast<algorithm>(*algorithm_number), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto encode_limits(const format_limits& value) -> cbor::value {
    return map({{"byte_budget", value.byte_budget},
                {"maximum_depth", static_cast<std::uint64_t>(value.maximum_depth)},
                {"maximum_entries", value.maximum_entries}});
}

auto unsigned_field(const cbor::value& source, std::string_view name) -> result<std::uint64_t> {
    const auto* value = field(source, name);
    const auto* number = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
    if (!number) {
        return std::unexpected(
            format_error(errc::corrupt, "format CBOR integer field is absent", std::string{name}));
    }
    return *number;
}

auto text_field(const cbor::value& source, std::string_view name) -> result<std::string> {
    const auto* value = field(source, name);
    const auto* text = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!text) {
        return std::unexpected(
            format_error(errc::corrupt, "format CBOR text field is absent", std::string{name}));
    }
    return *text;
}

auto bytes_field(const cbor::value& source, std::string_view name)
    -> result<std::vector<std::byte>> {
    const auto* value = field(source, name);
    const auto* raw = value ? std::get_if<cbor::value::bytes>(&value->data) : nullptr;
    if (!raw) {
        return std::unexpected(
            format_error(errc::corrupt, "format CBOR byte field is absent", std::string{name}));
    }
    return *raw;
}

auto real_text(double value) -> result<std::string> {
    auto output = std::array<char, 64>{};
    const auto [end, code] = std::to_chars(output.data(), output.data() + output.size(), value);
    if (code != std::errc{}) {
        return std::unexpected(format_error(errc::internal, "cannot encode format tolerance"));
    }
    return std::string{output.data(), end};
}

auto real_field(const cbor::value& source, std::string_view name) -> result<double> {
    auto encoded = text_field(source, name);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto* end = static_cast<char*>(nullptr);
    errno = 0;
    const auto output = std::strtod(encoded->c_str(), &end);
    if (errno == ERANGE || end != encoded->c_str() + encoded->size() || !std::isfinite(output) ||
        output < 0) {
        return std::unexpected(
            format_error(errc::corrupt, "invalid format tolerance", std::string{name}));
    }
    return output;
}

auto decode_limits(const cbor::value& value) -> result<format_limits> {
    auto depth = unsigned_field(value, "maximum_depth");
    auto budget = unsigned_field(value, "byte_budget");
    auto entries = unsigned_field(value, "maximum_entries");
    if (!depth) {
        return std::unexpected(depth.error());
    }
    if (!budget) {
        return std::unexpected(budget.error());
    }
    if (!entries) {
        return std::unexpected(entries.error());
    }
    if (*depth > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(format_error(errc::corrupt, "format depth limit overflows"));
    }
    return format_limits{static_cast<std::uint32_t>(*depth), *budget, *entries};
}

auto encode_tree(const format_tree& tree) -> result<cbor::value>;

auto encode_entry(const format_entry& entry) -> result<cbor::value> {
    auto nested = cbor::value{};
    if (entry.nested) {
        auto encoded = encode_tree(*entry.nested);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        nested = std::move(*encoded);
    }
    auto range = cbor::value{};
    if (entry.source_range) {
        range = map({{"offset", entry.source_range->offset}, {"size", entry.source_range->size}});
    }
    return map({{"attributes", bytes(entry.attributes)},
                {"bytes", bytes(entry.bytes)},
                {"content", digest_value(entry.content)},
                {"kind", static_cast<std::uint64_t>(entry.kind)},
                {"nested", std::move(nested)},
                {"opaque_reason", entry.opaque_reason},
                {"path", entry.path},
                {"size", entry.size},
                {"source_range", std::move(range)}});
}

auto encode_tree(const format_tree& tree) -> result<cbor::value> {
    auto entries = cbor::value::array{};
    entries.reserve(tree.entries.size());
    for (const auto& entry : tree.entries) {
        auto encoded = encode_entry(entry);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        entries.push_back(std::move(*encoded));
    }
    auto limits = cbor::value::array{};
    for (const auto& value : tree.limits) {
        limits.emplace_back(value);
    }
    return map({{"codec", tree.codec},
                {"entries", std::move(entries)},
                {"limits", std::move(limits)},
                {"original", bytes(tree.original)},
                {"tier", static_cast<std::uint64_t>(tree.tier)},
                {"truncated", tree.truncated}});
}

auto decode_tree(const cbor::value& value) -> result<format_tree>;

auto decode_entry(const cbor::value& value) -> result<format_entry> {
    auto path = text_field(value, "path");
    auto kind = unsigned_field(value, "kind");
    auto size = unsigned_field(value, "size");
    auto attributes_value = bytes_field(value, "attributes");
    auto entry_bytes = bytes_field(value, "bytes");
    auto reason = text_field(value, "opaque_reason");
    const auto* content_value = field(value, "content");
    if (!path || !kind || !size || !attributes_value || !entry_bytes || !reason || !content_value ||
        *kind < 1 || *kind > 6) {
        return std::unexpected(format_error(errc::corrupt, "invalid format entry"));
    }
    auto content = decode_digest(*content_value);
    if (!content) {
        return std::unexpected(content.error());
    }
    auto output = format_entry{.path = std::move(*path),
                               .kind = static_cast<format_entry_kind>(*kind),
                               .size = *size,
                               .content = *content,
                               .attributes = std::move(*attributes_value),
                               .bytes = std::move(*entry_bytes),
                               .opaque_reason = std::move(*reason)};
    if (const auto* range_value = field(value, "source_range");
        range_value && !std::holds_alternative<cbor::null>(range_value->data)) {
        auto offset = unsigned_field(*range_value, "offset");
        auto range_size = unsigned_field(*range_value, "size");
        if (!offset || !range_size) {
            return std::unexpected(format_error(errc::corrupt, "invalid format source range"));
        }
        output.source_range = format_byte_range{*offset, *range_size};
    }
    if (const auto* nested_value = field(value, "nested");
        nested_value && !std::holds_alternative<cbor::null>(nested_value->data)) {
        auto nested = decode_tree(*nested_value);
        if (!nested) {
            return std::unexpected(nested.error());
        }
        output.nested = std::make_shared<format_tree>(std::move(*nested));
    }
    return output;
}

auto decode_tree(const cbor::value& value) -> result<format_tree> {
    auto codec = text_field(value, "codec");
    auto tier = unsigned_field(value, "tier");
    auto original = bytes_field(value, "original");
    const auto* entries_value = field(value, "entries");
    const auto* entries =
        entries_value ? std::get_if<cbor::value::array>(&entries_value->data) : nullptr;
    const auto* truncated_value = field(value, "truncated");
    const auto* truncated = truncated_value ? std::get_if<bool>(&truncated_value->data) : nullptr;
    const auto* limits_value = field(value, "limits");
    const auto* limits =
        limits_value ? std::get_if<cbor::value::array>(&limits_value->data) : nullptr;
    if (!codec || !tier || *tier < 1 || *tier > 3 || !original || !entries || !truncated ||
        !limits) {
        return std::unexpected(format_error(errc::corrupt, "invalid format tree"));
    }
    auto output = format_tree{.codec = std::move(*codec),
                              .tier = static_cast<format_tier>(*tier),
                              .original = std::move(*original),
                              .truncated = *truncated};
    output.entries.reserve(entries->size());
    for (const auto& entry_value : *entries) {
        auto entry = decode_entry(entry_value);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        output.entries.push_back(std::move(*entry));
    }
    for (const auto& limit_value : *limits) {
        const auto* text = std::get_if<std::string>(&limit_value.data);
        if (!text) {
            return std::unexpected(format_error(errc::corrupt, "invalid format limit report"));
        }
        output.limits.push_back(*text);
    }
    return output;
}

auto encode_diff(const format_diff& value) -> result<cbor::value>;

auto encode_diff_entry(const format_diff_entry& entry) -> result<cbor::value> {
    auto old_digest = cbor::value{};
    if (entry.old_digest) {
        old_digest = digest_value(*entry.old_digest);
    }
    auto new_digest = cbor::value{};
    if (entry.new_digest) {
        new_digest = digest_value(*entry.new_digest);
    }
    auto nested = cbor::value{};
    if (entry.nested) {
        auto encoded = encode_diff(*entry.nested);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        nested = std::move(*encoded);
    }
    return map({{"kind", static_cast<std::uint64_t>(entry.kind)},
                {"nested", std::move(nested)},
                {"new_digest", std::move(new_digest)},
                {"old_digest", std::move(old_digest)},
                {"path", entry.path}});
}

auto encode_diff(const format_diff& value) -> result<cbor::value> {
    auto entries = cbor::value::array{};
    for (const auto& entry : value.entries) {
        auto encoded = encode_diff_entry(entry);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        entries.push_back(std::move(*encoded));
    }
    return map({{"codec", value.codec},
                {"entries", std::move(entries)},
                {"tier", static_cast<std::uint64_t>(value.tier)},
                {"truncated", value.truncated}});
}

auto decode_diff(const cbor::value& value) -> result<format_diff>;

auto optional_digest(const cbor::value& value, std::string_view name)
    -> result<std::optional<digest>> {
    const auto* candidate = field(value, name);
    if (!candidate || std::holds_alternative<cbor::null>(candidate->data)) {
        return std::optional<digest>{};
    }
    auto decoded = decode_digest(*candidate);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return std::optional<digest>{*decoded};
}

auto decode_diff_entry(const cbor::value& value) -> result<format_diff_entry> {
    auto path = text_field(value, "path");
    auto kind = unsigned_field(value, "kind");
    auto old_digest = optional_digest(value, "old_digest");
    auto new_digest = optional_digest(value, "new_digest");
    if (!path || !kind || *kind < 1 || *kind > 3 || !old_digest || !new_digest) {
        return std::unexpected(format_error(errc::corrupt, "invalid format diff entry"));
    }
    auto output = format_diff_entry{.path = std::move(*path),
                                    .kind = static_cast<format_diff_kind>(*kind),
                                    .old_digest = std::move(*old_digest),
                                    .new_digest = std::move(*new_digest)};
    if (const auto* nested_value = field(value, "nested");
        nested_value && !std::holds_alternative<cbor::null>(nested_value->data)) {
        auto nested = decode_diff(*nested_value);
        if (!nested) {
            return std::unexpected(nested.error());
        }
        output.nested = std::make_shared<format_diff>(std::move(*nested));
    }
    return output;
}

auto decode_diff(const cbor::value& value) -> result<format_diff> {
    auto codec = text_field(value, "codec");
    auto tier = unsigned_field(value, "tier");
    const auto* truncated_value = field(value, "truncated");
    const auto* truncated = truncated_value ? std::get_if<bool>(&truncated_value->data) : nullptr;
    const auto* entries_value = field(value, "entries");
    const auto* entries =
        entries_value ? std::get_if<cbor::value::array>(&entries_value->data) : nullptr;
    if (!codec || !tier || *tier < 1 || *tier > 3 || !truncated || !entries) {
        return std::unexpected(format_error(errc::corrupt, "invalid format diff"));
    }
    auto output = format_diff{.codec = std::move(*codec),
                              .tier = static_cast<format_tier>(*tier),
                              .truncated = *truncated};
    for (const auto& entry_value : *entries) {
        auto entry = decode_diff_entry(entry_value);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        output.entries.push_back(std::move(*entry));
    }
    return output;
}

auto canonical_value(std::span<const std::byte> input, std::string_view context)
    -> result<cbor::value> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (encoded->size() != input.size() || !std::ranges::equal(*encoded, input)) {
        return std::unexpected(format_error(
            errc::corrupt, "format module returned non-canonical CBOR", std::string{context}));
    }
    return std::move(*decoded);
}

struct callback_bridge {
    format_callbacks* callbacks{};

    static void
    progress(void* context, const char* operation, std::uint64_t completed, std::uint64_t total) {
        auto* self = static_cast<callback_bridge*>(context);
        if (self->callbacks->progress) {
            self->callbacks->progress(operation ? operation : "", completed, total);
        }
    }

    static int cancelled(void* context) {
        auto* self = static_cast<callback_bridge*>(context);
        return self->callbacks->cancelled && self->callbacks->cancelled() ? 1 : 0;
    }
};

auto status_code(std::uint32_t status) -> errc {
    switch (status) {
    case LIQUID_FORMAT_STATUS_INVALID_ARGUMENT:
        return errc::invalid_argument;
    case LIQUID_FORMAT_STATUS_UNSUPPORTED:
        return errc::unsupported;
    case LIQUID_FORMAT_STATUS_CANCELLED:
        return errc::unavailable;
    case LIQUID_FORMAT_STATUS_CORRUPT:
        return errc::corrupt;
    default:
        return errc::internal;
    }
}

auto invoke(const liquid_format_module_v1* module,
            liquid_format_operation_v1 operation,
            const cbor::value& input,
            format_callbacks callbacks,
            std::string_view name) -> result<cbor::value> {
    if (!module || !operation) {
        return std::unexpected(
            format_error(errc::unsupported, "format operation is unsupported", std::string{name}));
    }
    auto input_bytes = cbor::encode(input);
    if (!input_bytes) {
        return std::unexpected(input_bytes.error());
    }
    auto bridge = callback_bridge{&callbacks};
    auto abi_callbacks = liquid_format_callbacks_v1{.context = &bridge,
                                                    .progress = callback_bridge::progress,
                                                    .cancelled = callback_bridge::cancelled};
    auto output = liquid_format_owned_bytes_v1{};
    auto abi_error = liquid_format_error_v1{};
    const auto status =
        operation(module->module_context,
                  {reinterpret_cast<const std::uint8_t*>(input_bytes->data()), input_bytes->size()},
                  &abi_callbacks,
                  &output,
                  &abi_error);
    if (status != LIQUID_FORMAT_STATUS_OK) {
        return std::unexpected(
            format_error(status_code(static_cast<std::uint32_t>(status)),
                         abi_error.message ? abi_error.message : "format module failed",
                         module->descriptor.name));
    }
    if (!output.data || !output.release) {
        return std::unexpected(format_error(
            errc::corrupt, "format module returned invalid owned bytes", module->descriptor.name));
    }
    auto copied = std::vector<std::byte>(output.size);
    if (!copied.empty()) {
        std::memcpy(copied.data(), output.data, output.size);
    }
    output.release(output.context, output.data, output.size);
    return canonical_value(copied, name);
}

auto lower(std::string value) -> std::string {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

} // namespace

struct format_codec::impl {
    const liquid_format_module_v1* module{};
    format_descriptor public_descriptor;
    void* library{};
    bool owns_library{};

    ~impl() {
        if (module && module->destroy) {
            module->destroy(module->module_context);
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

format_codec::format_codec() noexcept = default;
format_codec::format_codec(std::unique_ptr<impl> implementation)
    : impl_(std::move(implementation)) {}
format_codec::~format_codec() = default;
format_codec::format_codec(format_codec&&) noexcept = default;
auto format_codec::operator=(format_codec&&) noexcept -> format_codec& = default;

auto format_codec::linked(const liquid_format_module_v1* module) -> result<format_codec> {
    if (!module || module->abi_version != LIQUID_FORMAT_ABI_VERSION ||
        module->struct_size < sizeof(liquid_format_module_v1) || !module->descriptor.name ||
        module->descriptor.name[0] == '\0' || module->descriptor.tier < 1 ||
        module->descriptor.tier > 3 || !module->recognise || !module->unpack ||
        !module->canonicalise || !module->diff ||
        (module->descriptor.tier == 1 && !module->repack)) {
        return std::unexpected(format_error(
            errc::invalid_argument, "format descriptor does not implement ABI v1", "format"));
    }
    if (module->descriptor.extension_count != 0 && !module->descriptor.extensions) {
        return std::unexpected(format_error(
            errc::invalid_argument, "format extensions are absent", module->descriptor.name));
    }
    if (module->descriptor.magic_count != 0 && !module->descriptor.magic) {
        return std::unexpected(format_error(
            errc::invalid_argument, "format magic is absent", module->descriptor.name));
    }
    if (module->descriptor.volatile_field_count != 0 && !module->descriptor.volatile_fields) {
        return std::unexpected(format_error(
            errc::invalid_argument, "format volatile fields are absent", module->descriptor.name));
    }
    auto implementation = std::make_unique<impl>();
    implementation->module = module;
    implementation->public_descriptor.name = module->descriptor.name;
    implementation->public_descriptor.tier = static_cast<format_tier>(module->descriptor.tier);
    implementation->public_descriptor.limits = {module->descriptor.limits.maximum_depth,
                                                module->descriptor.limits.byte_budget,
                                                module->descriptor.limits.maximum_entries};
    for (std::size_t index = 0; index < module->descriptor.extension_count; ++index) {
        const auto* extension = module->descriptor.extensions[index];
        if (!extension || extension[0] == '\0') {
            return std::unexpected(format_error(errc::invalid_argument,
                                                "format has an invalid extension",
                                                module->descriptor.name));
        }
        implementation->public_descriptor.extensions.emplace_back(extension);
    }
    for (std::size_t index = 0; index < module->descriptor.magic_count; ++index) {
        const auto& magic = module->descriptor.magic[index];
        if (!magic.data || magic.size == 0) {
            return std::unexpected(format_error(
                errc::invalid_argument, "format has invalid magic", module->descriptor.name));
        }
        const auto* begin = reinterpret_cast<const std::byte*>(magic.data);
        implementation->public_descriptor.magic.emplace_back(begin, begin + magic.size);
    }
    for (std::size_t index = 0; index < module->descriptor.volatile_field_count; ++index) {
        const auto* volatile_field = module->descriptor.volatile_fields[index];
        if (!volatile_field || volatile_field[0] == '\0') {
            return std::unexpected(format_error(errc::invalid_argument,
                                                "format has an invalid volatile field",
                                                module->descriptor.name));
        }
        implementation->public_descriptor.volatile_fields.emplace_back(volatile_field);
    }
    return format_codec{std::move(implementation)};
}

auto format_codec::load(std::filesystem::path shared_object) -> result<format_codec> {
    void* library{};
#if defined(_WIN32)
    library = LoadLibraryW(shared_object.wstring().c_str());
    auto entrypoint = library ? reinterpret_cast<liquid_format_entrypoint_v1>(GetProcAddress(
                                    static_cast<HMODULE>(library), LIQUID_FORMAT_ENTRYPOINT_V1))
                              : nullptr;
#else
    library = dlopen(shared_object.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto entrypoint = library ? reinterpret_cast<liquid_format_entrypoint_v1>(
                                    dlsym(library, LIQUID_FORMAT_ENTRYPOINT_V1))
                              : nullptr;
#endif
    if (!library || !entrypoint) {
        if (library) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(library));
#else
            dlclose(library);
#endif
        }
        return std::unexpected(
            format_error(errc::not_found, "cannot load format module", shared_object.string()));
    }
    auto loaded = linked(entrypoint());
    if (!loaded) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
        return std::unexpected(loaded.error());
    }
    loaded->impl_->library = library;
    loaded->impl_->owns_library = true;
    return loaded;
}

auto format_codec::descriptor() const -> const format_descriptor& {
    static const auto empty = format_descriptor{};
    return impl_ ? impl_->public_descriptor : empty;
}

auto format_codec::recognise(std::span<const std::byte> blob,
                             std::string_view path,
                             format_callbacks callbacks) const
    -> result<std::optional<format_recognition>> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    auto output = invoke(impl_->module,
                         impl_->module->recognise,
                         map({{"blob", bytes(blob)}, {"path", std::string{path}}}),
                         std::move(callbacks),
                         "format recognise");
    if (!output) {
        return std::unexpected(output.error());
    }
    const auto* recognised_value = field(*output, "recognised");
    const auto* recognised =
        recognised_value ? std::get_if<bool>(&recognised_value->data) : nullptr;
    if (!recognised) {
        return std::unexpected(format_error(errc::corrupt, "invalid format recognition result"));
    }
    if (!*recognised) {
        return std::optional<format_recognition>{};
    }
    auto tier = unsigned_field(*output, "tier");
    auto reason = text_field(*output, "reason");
    if (!tier || *tier < 1 || *tier > 3 || !reason) {
        return std::unexpected(format_error(errc::corrupt, "invalid format recognition tier"));
    }
    return std::optional<format_recognition>{
        format_recognition{static_cast<format_tier>(*tier), std::move(*reason)}};
}

auto format_codec::unpack(std::span<const std::byte> blob,
                          format_limits limits,
                          format_callbacks callbacks) const -> result<format_tree> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    auto output = invoke(impl_->module,
                         impl_->module->unpack,
                         map({{"blob", bytes(blob)}, {"limits", encode_limits(limits)}}),
                         std::move(callbacks),
                         "format unpack");
    if (!output) {
        return std::unexpected(output.error());
    }
    return decode_tree(*output);
}

auto format_codec::canonicalise(const format_tree& tree, format_callbacks callbacks) const
    -> result<format_tree> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    auto encoded = encode_tree(tree);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto output = invoke(impl_->module,
                         impl_->module->canonicalise,
                         map({{"tree", std::move(*encoded)}}),
                         std::move(callbacks),
                         "format canonicalise");
    if (!output) {
        return std::unexpected(output.error());
    }
    return decode_tree(*output);
}

auto format_codec::diff(const format_tree& old_tree,
                        const format_tree& new_tree,
                        format_callbacks callbacks) const -> result<format_diff> {
    return diff(old_tree, new_tree, format_diff_options{}, std::move(callbacks));
}

auto format_codec::diff(const format_tree& old_tree,
                        const format_tree& new_tree,
                        format_diff_options options,
                        format_callbacks callbacks) const -> result<format_diff> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    if (!std::isfinite(options.absolute_tolerance) || options.absolute_tolerance < 0 ||
        !std::isfinite(options.relative_tolerance) || options.relative_tolerance < 0) {
        return std::unexpected(format_error(errc::invalid_argument,
                                            "format tolerances must be finite and nonnegative"));
    }
    auto old_value = encode_tree(old_tree);
    auto new_value = encode_tree(new_tree);
    auto absolute = real_text(options.absolute_tolerance);
    auto relative = real_text(options.relative_tolerance);
    if (!old_value) {
        return std::unexpected(old_value.error());
    }
    if (!new_value) {
        return std::unexpected(new_value.error());
    }
    if (!absolute || !relative) {
        return std::unexpected(format_error(errc::internal, "cannot encode format tolerances"));
    }
    auto output = invoke(impl_->module,
                         impl_->module->diff,
                         map({{"absolute_tolerance", std::move(*absolute)},
                              {"new", std::move(*new_value)},
                              {"old", std::move(*old_value)},
                              {"relative_tolerance", std::move(*relative)}}),
                         std::move(callbacks),
                         "format diff");
    if (!output) {
        return std::unexpected(output.error());
    }
    return decode_diff(*output);
}

auto format_codec::merge(const format_tree& base,
                         const format_tree& left,
                         const format_tree& right,
                         format_callbacks callbacks) const -> result<format_tree> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    auto base_value = encode_tree(base);
    auto left_value = encode_tree(left);
    auto right_value = encode_tree(right);
    if (!base_value || !left_value || !right_value) {
        return std::unexpected(format_error(errc::internal, "cannot encode format merge input"));
    }
    auto output = invoke(impl_->module,
                         impl_->module->merge,
                         map({{"base", std::move(*base_value)},
                              {"left", std::move(*left_value)},
                              {"right", std::move(*right_value)}}),
                         std::move(callbacks),
                         "format merge");
    if (!output) {
        return std::unexpected(output.error());
    }
    return decode_tree(*output);
}

auto format_codec::repack(const format_tree& tree, format_callbacks callbacks) const
    -> result<std::vector<std::byte>> {
    if (!impl_) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    auto tree_value = encode_tree(tree);
    if (!tree_value) {
        return std::unexpected(tree_value.error());
    }
    auto output = invoke(impl_->module,
                         impl_->module->repack,
                         map({{"tree", std::move(*tree_value)}}),
                         std::move(callbacks),
                         "format repack");
    if (!output) {
        return std::unexpected(output.error());
    }
    return bytes_field(*output, "bytes");
}

struct format_registry::impl {
    std::vector<format_codec> codecs;
};

format_registry::format_registry() : impl_(std::make_unique<impl>()) {}
format_registry::~format_registry() = default;
format_registry::format_registry(format_registry&&) noexcept = default;
auto format_registry::operator=(format_registry&&) noexcept -> format_registry& = default;

auto format_registry::global() -> format_registry& {
    static auto value = [] {
        auto registry = format_registry{};
        const auto modules = std::array{formats::detail::zip_module(),
                                        formats::detail::tar_module(),
                                        formats::detail::deb_module(),
                                        formats::detail::rpm_module(),
                                        formats::detail::apk_module(),
                                        formats::detail::hdf5_module(),
                                        formats::detail::parquet_module(),
                                        formats::detail::sqlite_module()};
        for (const auto* module : modules) {
            auto codec = format_codec::linked(module);
            if (codec) {
                static_cast<void>(registry.add(std::move(*codec)));
            }
        }
        return registry;
    }();
    return value;
}

auto format_registry::add(format_codec codec) -> result<void> {
    if (!impl_ || codec.descriptor().name.empty()) {
        return std::unexpected(format_error(errc::invalid_argument, "format codec is empty"));
    }
    const auto duplicate = std::ranges::find_if(impl_->codecs, [&](const format_codec& current) {
        return current.descriptor().name == codec.descriptor().name;
    });
    if (duplicate != impl_->codecs.end()) {
        return std::unexpected(format_error(
            errc::already_exists, "format codec is already registered", codec.descriptor().name));
    }
    impl_->codecs.push_back(std::move(codec));
    return {};
}

auto format_registry::load(std::filesystem::path shared_object) -> result<void> {
    auto codec = format_codec::load(std::move(shared_object));
    if (!codec) {
        return std::unexpected(codec.error());
    }
    return add(std::move(*codec));
}

auto format_registry::find_by_magic(std::span<const std::byte> blob) const
    -> std::vector<const format_codec*> {
    auto output = std::vector<const format_codec*>{};
    if (!impl_) {
        return output;
    }
    for (const auto& codec : impl_->codecs) {
        const auto& magic_values = codec.descriptor().magic;
        if (std::ranges::any_of(magic_values, [&](const auto& magic) {
                return blob.size() >= magic.size() &&
                       std::ranges::equal(magic, blob.first(magic.size()));
            })) {
            output.push_back(&codec);
        }
    }
    return output;
}

auto format_registry::find_by_extension(std::string_view path) const
    -> std::vector<const format_codec*> {
    auto output = std::vector<const format_codec*>{};
    if (!impl_) {
        return output;
    }
    const auto name = lower(std::filesystem::path{path}.filename().string());
    for (const auto& codec : impl_->codecs) {
        if (std::ranges::any_of(codec.descriptor().extensions, [&](const auto& extension) {
                const auto wanted = lower(extension);
                return name.size() >= wanted.size() && name.ends_with(wanted);
            })) {
            output.push_back(&codec);
        }
    }
    return output;
}

auto format_registry::codecs() const -> std::vector<const format_codec*> {
    auto output = std::vector<const format_codec*>{};
    if (!impl_) {
        return output;
    }
    for (const auto& codec : impl_->codecs) {
        output.push_back(&codec);
    }
    return output;
}

auto format_registry::recognise(std::span<const std::byte> blob, std::string_view path) const
    -> result<std::pair<const format_codec*, format_recognition>> {
    auto candidates = find_by_magic(blob);
    for (const auto* codec : find_by_extension(path)) {
        if (std::ranges::find(candidates, codec) == candidates.end()) {
            candidates.push_back(codec);
        }
    }
    for (const auto* codec : codecs()) {
        if (std::ranges::find(candidates, codec) == candidates.end()) {
            candidates.push_back(codec);
        }
    }
    for (const auto* codec : candidates) {
        auto recognised = codec->recognise(blob, path);
        if (!recognised) {
            return std::unexpected(recognised.error());
        }
        if (*recognised) {
            return std::pair{codec, **recognised};
        }
    }
    return std::unexpected(
        format_error(errc::not_found, "blob format is not recognised", std::string{path}));
}

auto format_registry::unpack(std::span<const std::byte> blob,
                             std::string_view path,
                             format_limits limits) const -> result<format_tree> {
    auto recognised = recognise(blob, path);
    if (!recognised) {
        return std::unexpected(recognised.error());
    }
    auto root = recognised->first->unpack(blob, limits);
    if (!root) {
        return std::unexpected(root.error());
    }
    root->codec = recognised->first->descriptor().name;
    root->tier = recognised->second.tier;
    auto budget = limits.byte_budget;
    auto entry_count = std::uint64_t{root->entries.size()};
    auto expand = [&](auto&& self, format_tree& tree, std::uint32_t depth) -> result<void> {
        for (auto& entry : tree.entries) {
            if (entry.bytes.size() > budget) {
                tree.truncated = true;
                tree.limits.push_back("byte budget reached at " + entry.path);
                entry.opaque_reason = "byte budget reached";
                entry.bytes.clear();
                continue;
            }
            budget -= entry.bytes.size();
            if (entry.bytes.empty()) {
                continue;
            }
            auto nested_recognition = recognise(entry.bytes, entry.path);
            if (!nested_recognition) {
                if (nested_recognition.error().code == errc::not_found) {
                    continue;
                }
                return std::unexpected(nested_recognition.error());
            }
            if (depth >= limits.maximum_depth) {
                tree.truncated = true;
                tree.limits.push_back("depth limit reached at " + entry.path);
                entry.opaque_reason = "depth limit reached";
                continue;
            }
            auto nested = nested_recognition->first->unpack(entry.bytes, limits);
            if (!nested) {
                return std::unexpected(nested.error());
            }
            nested->codec = nested_recognition->first->descriptor().name;
            nested->tier = nested_recognition->second.tier;
            if (nested->entries.size() >
                limits.maximum_entries - std::min(limits.maximum_entries, entry_count)) {
                tree.truncated = true;
                tree.limits.push_back("entry limit reached at " + entry.path);
                entry.opaque_reason = "entry limit reached";
                continue;
            }
            entry_count += nested->entries.size();
            auto expanded = self(self, *nested, depth + 1);
            if (!expanded) {
                return expanded;
            }
            entry.nested = std::make_shared<format_tree>(std::move(*nested));
        }
        return {};
    };
    auto expanded = expand(expand, *root, 1);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    return root;
}

} // namespace liquid

namespace liquid::formats::detail {
namespace {

namespace cbor = encoding::cbor;

thread_local auto abi_error_message = std::string{};

void release_bytes(void*, std::uint8_t* data, std::size_t) {
    delete[] data;
}

auto abi_status(const error& value) -> int {
    switch (value.code) {
    case errc::invalid_argument:
        return LIQUID_FORMAT_STATUS_INVALID_ARGUMENT;
    case errc::unsupported:
        return LIQUID_FORMAT_STATUS_UNSUPPORTED;
    case errc::unavailable:
        return LIQUID_FORMAT_STATUS_CANCELLED;
    case errc::corrupt:
        return LIQUID_FORMAT_STATUS_CORRUPT;
    default:
        return LIQUID_FORMAT_STATUS_INTERNAL;
    }
}

auto fail_abi(const error& value, liquid_format_error_v1* output) -> int {
    abi_error_message = value.message;
    if (!value.context.empty()) {
        abi_error_message += " (" + value.context + ")";
    }
    if (output) {
        output->code = static_cast<std::uint32_t>(value.code);
        output->message = abi_error_message.c_str();
    }
    return abi_status(value);
}

auto return_value(result<cbor::value> value,
                  liquid_format_owned_bytes_v1* output,
                  liquid_format_error_v1* error_output) -> int {
    if (!value) {
        return fail_abi(value.error(), error_output);
    }
    auto encoded = cbor::encode(*value);
    if (!encoded) {
        return fail_abi(encoded.error(), error_output);
    }
    auto* data = new std::uint8_t[encoded->size()];
    if (!encoded->empty()) {
        std::memcpy(data, encoded->data(), encoded->size());
    }
    *output = {.data = data, .size = encoded->size(), .release = release_bytes, .context = nullptr};
    return LIQUID_FORMAT_STATUS_OK;
}

auto input_value(liquid_format_bytes_v1 input) -> result<cbor::value> {
    if (!input.data && input.size != 0) {
        return std::unexpected(
            format_error(errc::invalid_argument, "format input bytes are invalid"));
    }
    const auto* begin = reinterpret_cast<const std::byte*>(input.data);
    return cbor::decode(std::span<const std::byte>{begin, input.size});
}

auto cancelled(const liquid_format_callbacks_v1* callbacks) -> bool {
    return callbacks && callbacks->cancelled && callbacks->cancelled(callbacks->context) != 0;
}

auto require_tree(const cbor::value& input, std::string_view name) -> result<format_tree> {
    const auto* value = field(input, name);
    if (!value) {
        return std::unexpected(
            format_error(errc::invalid_argument, "format tree input is absent", std::string{name}));
    }
    return decode_tree(*value);
}

auto strip_attributes(std::vector<std::byte> input,
                      std::span<const std::string_view> volatile_fields)
    -> result<std::vector<std::byte>> {
    if (input.empty()) {
        return empty_attributes();
    }
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto* fields = std::get_if<cbor::value::map>(&decoded->data);
    if (!fields) {
        return std::unexpected(format_error(errc::corrupt, "format attributes are not a CBOR map"));
    }
    std::erase_if(*fields, [&](const auto& item) {
        const auto* key = std::get_if<std::string>(&item.first.data);
        return key && std::ranges::find(volatile_fields, *key) != volatile_fields.end();
    });
    return cbor::encode(*decoded);
}

auto copy_tree(const format_tree& input) -> format_tree {
    auto output = input;
    for (auto& entry : output.entries) {
        if (entry.nested) {
            entry.nested = std::make_shared<format_tree>(copy_tree(*entry.nested));
        }
    }
    return output;
}

} // namespace

auto empty_attributes() -> result<std::vector<std::byte>> {
    return cbor::encode(cbor::value{cbor::value::map{}});
}

auto attributes(std::initializer_list<std::pair<std::string, std::string>> fields)
    -> result<std::vector<std::byte>> {
    auto values = cbor::value::map{};
    for (const auto& [key, value] : fields) {
        values.emplace_back(key, value);
    }
    return cbor::encode(cbor::value{std::move(values)});
}

auto make_entry(std::string path,
                format_entry_kind kind,
                std::vector<std::byte> entry_bytes,
                std::vector<std::byte> entry_attributes,
                std::optional<format_byte_range> range) -> result<format_entry> {
    if (entry_attributes.empty()) {
        auto encoded = empty_attributes();
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        entry_attributes = std::move(*encoded);
    }
    auto content = hash(algorithm::blake3, entry_bytes);
    if (!content) {
        return std::unexpected(content.error());
    }
    return format_entry{.path = std::move(path),
                        .kind = kind,
                        .size = entry_bytes.size(),
                        .content = *content,
                        .attributes = std::move(entry_attributes),
                        .bytes = std::move(entry_bytes),
                        .source_range = range};
}

auto codec_backend::canonicalise(const format_tree& tree) -> result<format_tree> {
    auto output = copy_tree(tree);
    auto canonicalise_entries = [&](auto&& self, format_tree& value) -> result<void> {
        for (auto& entry : value.entries) {
            auto stripped = strip_attributes(std::move(entry.attributes), volatile_fields());
            if (!stripped) {
                return std::unexpected(stripped.error());
            }
            entry.attributes = std::move(*stripped);
            if (entry.nested) {
                auto nested_result = self(self, *entry.nested);
                if (!nested_result) {
                    return nested_result;
                }
            }
        }
        std::ranges::sort(value.entries, [](const format_entry& left, const format_entry& right) {
            if (left.path != right.path) {
                return left.path < right.path;
            }
            return left.kind < right.kind;
        });
        return {};
    };
    auto canonicalised = canonicalise_entries(canonicalise_entries, output);
    if (!canonicalised) {
        return std::unexpected(canonicalised.error());
    }
    return output;
}

auto codec_backend::diff(const format_tree& old_tree,
                         const format_tree& new_tree,
                         format_diff_options options) -> result<format_diff> {
    auto old_entries = std::map<std::pair<std::string, format_entry_kind>, const format_entry*>{};
    auto new_entries = std::map<std::pair<std::string, format_entry_kind>, const format_entry*>{};
    for (const auto& entry : old_tree.entries) {
        old_entries[{entry.path, entry.kind}] = &entry;
    }
    for (const auto& entry : new_tree.entries) {
        new_entries[{entry.path, entry.kind}] = &entry;
    }
    auto output = format_diff{.codec = new_tree.codec.empty() ? old_tree.codec : new_tree.codec,
                              .tier = new_tree.tier,
                              .truncated = old_tree.truncated || new_tree.truncated};
    for (const auto& [key, old_entry] : old_entries) {
        const auto found = new_entries.find(key);
        if (found == new_entries.end()) {
            output.entries.push_back({.path = old_entry->path,
                                      .kind = format_diff_kind::removed,
                                      .old_digest = old_entry->content});
            continue;
        }
        const auto* new_entry = found->second;
        auto nested = std::shared_ptr<format_diff>{};
        if (old_entry->nested && new_entry->nested &&
            old_entry->nested->codec == new_entry->nested->codec) {
            auto nested_result = diff(*old_entry->nested, *new_entry->nested, options);
            if (!nested_result) {
                return std::unexpected(nested_result.error());
            }
            if (!nested_result->entries.empty() || nested_result->truncated) {
                nested = std::make_shared<format_diff>(std::move(*nested_result));
            }
        }
        if (!same_digest(old_entry->content, new_entry->content) ||
            old_entry->attributes != new_entry->attributes || nested) {
            output.entries.push_back({.path = old_entry->path,
                                      .kind = format_diff_kind::modified,
                                      .old_digest = old_entry->content,
                                      .new_digest = new_entry->content,
                                      .nested = std::move(nested)});
        }
    }
    for (const auto& [key, new_entry] : new_entries) {
        if (!old_entries.contains(key)) {
            output.entries.push_back({.path = new_entry->path,
                                      .kind = format_diff_kind::added,
                                      .new_digest = new_entry->content});
        }
    }
    std::ranges::sort(output.entries, {}, &format_diff_entry::path);
    return output;
}

auto codec_backend::merge(const format_tree&, const format_tree&, const format_tree&)
    -> result<format_tree> {
    return std::unexpected(format_error(errc::unsupported, "format merge is unsupported"));
}

auto codec_backend::repack(const format_tree&) -> result<std::vector<std::byte>> {
    return std::unexpected(format_error(errc::unsupported, "format repack is unsupported"));
}

int recognise_operation(void* module_context,
                        liquid_format_bytes_v1 input_cbor,
                        const liquid_format_callbacks_v1* callbacks,
                        liquid_format_owned_bytes_v1* output_cbor,
                        liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto blob = bytes_field(*input, "blob");
    auto path = text_field(*input, "path");
    if (!blob || !path) {
        return fail_abi(format_error(errc::invalid_argument, "invalid recognise input"),
                        error_output);
    }
    auto recognised = static_cast<codec_backend*>(module_context)->recognise(*blob, *path);
    if (!recognised) {
        return fail_abi(recognised.error(), error_output);
    }
    if (!*recognised) {
        return return_value(
            map({{"reason", ""}, {"recognised", false}, {"tier", std::uint64_t{3}}}),
            output_cbor,
            error_output);
    }
    return return_value(map({{"reason", (**recognised).reason},
                             {"recognised", true},
                             {"tier", static_cast<std::uint64_t>((**recognised).tier)}}),
                        output_cbor,
                        error_output);
}

int unpack_operation(void* module_context,
                     liquid_format_bytes_v1 input_cbor,
                     const liquid_format_callbacks_v1* callbacks,
                     liquid_format_owned_bytes_v1* output_cbor,
                     liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto blob = bytes_field(*input, "blob");
    const auto* limits_value = field(*input, "limits");
    auto limits = limits_value ? decode_limits(*limits_value)
                               : result<format_limits>{std::unexpected(format_error(
                                     errc::invalid_argument, "format limits are absent"))};
    if (!blob || !limits) {
        return fail_abi(format_error(errc::invalid_argument, "invalid unpack input"), error_output);
    }
    auto tree = static_cast<codec_backend*>(module_context)->unpack(*blob, *limits);
    if (!tree) {
        return fail_abi(tree.error(), error_output);
    }
    return return_value(encode_tree(*tree), output_cbor, error_output);
}

int canonicalise_operation(void* module_context,
                           liquid_format_bytes_v1 input_cbor,
                           const liquid_format_callbacks_v1* callbacks,
                           liquid_format_owned_bytes_v1* output_cbor,
                           liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto tree = require_tree(*input, "tree");
    if (!tree) {
        return fail_abi(tree.error(), error_output);
    }
    auto output = static_cast<codec_backend*>(module_context)->canonicalise(*tree);
    if (!output) {
        return fail_abi(output.error(), error_output);
    }
    return return_value(encode_tree(*output), output_cbor, error_output);
}

int diff_operation(void* module_context,
                   liquid_format_bytes_v1 input_cbor,
                   const liquid_format_callbacks_v1* callbacks,
                   liquid_format_owned_bytes_v1* output_cbor,
                   liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto old_tree = require_tree(*input, "old");
    auto new_tree = require_tree(*input, "new");
    auto absolute = real_field(*input, "absolute_tolerance");
    auto relative = real_field(*input, "relative_tolerance");
    if (!old_tree || !new_tree || !absolute || !relative) {
        return fail_abi(format_error(errc::invalid_argument, "invalid format diff input"),
                        error_output);
    }
    auto output = static_cast<codec_backend*>(module_context)
                      ->diff(*old_tree,
                             *new_tree,
                             format_diff_options{.absolute_tolerance = *absolute,
                                                 .relative_tolerance = *relative});
    if (!output) {
        return fail_abi(output.error(), error_output);
    }
    return return_value(encode_diff(*output), output_cbor, error_output);
}

int merge_operation(void* module_context,
                    liquid_format_bytes_v1 input_cbor,
                    const liquid_format_callbacks_v1* callbacks,
                    liquid_format_owned_bytes_v1* output_cbor,
                    liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto base = require_tree(*input, "base");
    auto left = require_tree(*input, "left");
    auto right = require_tree(*input, "right");
    if (!base || !left || !right) {
        return fail_abi(format_error(errc::invalid_argument, "invalid format merge input"),
                        error_output);
    }
    auto output = static_cast<codec_backend*>(module_context)->merge(*base, *left, *right);
    if (!output) {
        return fail_abi(output.error(), error_output);
    }
    return return_value(encode_tree(*output), output_cbor, error_output);
}

int repack_operation(void* module_context,
                     liquid_format_bytes_v1 input_cbor,
                     const liquid_format_callbacks_v1* callbacks,
                     liquid_format_owned_bytes_v1* output_cbor,
                     liquid_format_error_v1* error_output) {
    if (cancelled(callbacks)) {
        return fail_abi(format_error(errc::unavailable, "format operation cancelled"),
                        error_output);
    }
    auto input = input_value(input_cbor);
    if (!input) {
        return fail_abi(input.error(), error_output);
    }
    auto tree = require_tree(*input, "tree");
    if (!tree) {
        return fail_abi(tree.error(), error_output);
    }
    auto output = static_cast<codec_backend*>(module_context)->repack(*tree);
    if (!output) {
        return fail_abi(output.error(), error_output);
    }
    return return_value(map({{"bytes", bytes(*output)}}), output_cbor, error_output);
}

} // namespace liquid::formats::detail
