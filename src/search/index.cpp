#include "search/index.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <map>
#include <ranges>
#include <regex>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include <oramacore/bm25f.hpp>
#include <oramacore/tokenize.hpp>

#include "encoding/cbor.hpp"

namespace liquid::search_detail {
namespace {

namespace cbor = encoding::cbor;

constexpr auto working_slot = std::string_view{"working"};

struct slot_record {
    std::string generation;
    std::vector<std::string> segments;
};

struct manifest_record {
    std::map<std::string, slot_record> slots;
};

auto failure(errc code, std::string message, const std::filesystem::path& path = {}) -> error {
    return {code, std::move(message), path.string()};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto bytes(std::span<const std::byte> value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}

auto digest_value(const digest& value) -> cbor::value {
    return map({{"alg", cbor::value{static_cast<std::uint64_t>(value.alg)}},
                {"bytes", bytes(value.bytes)}});
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

auto decode_digest(const cbor::value& value) -> result<digest> {
    const auto* algorithm_value = field(value, "alg");
    const auto* bytes_value = field(value, "bytes");
    const auto* algorithm_number =
        algorithm_value ? std::get_if<std::uint64_t>(&algorithm_value->data) : nullptr;
    const auto* raw = bytes_value ? std::get_if<cbor::value::bytes>(&bytes_value->data) : nullptr;
    if (!algorithm_number || *algorithm_number < 1 || *algorithm_number > 3 || !raw ||
        raw->size() != 32) {
        return std::unexpected(error{errc::corrupt, "invalid search digest", "search index"});
    }
    auto output = digest{static_cast<algorithm>(*algorithm_number), {}};
    std::ranges::copy(*raw, output.bytes.begin());
    return output;
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::not_found, "search index file is absent", path));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(failure(errc::io, "cannot size search index file", path));
    }
    input.seekg(0, std::ios::beg);
    auto output = std::vector<std::byte>(static_cast<std::size_t>(end));
    if (!output.empty()) {
        input.read(reinterpret_cast<char*>(output.data()),
                   static_cast<std::streamsize>(output.size()));
    }
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read search index file", path));
    }
    return output;
}

auto replace_file(const std::filesystem::path& temporary, const std::filesystem::path& path)
    -> result<void> {
#ifdef _WIN32
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return std::unexpected(failure(
            errc::io, "cannot publish search index file: " + std::to_string(GetLastError()), path));
    }
#else
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path));
    }
#endif
    return {};
}

auto write_atomic(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.parent_path()));
    }
    static std::atomic<std::uint64_t> sequence{};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return std::unexpected(failure(errc::io, "cannot write search index file", temporary));
        }
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            return std::unexpected(failure(errc::io, "cannot flush search index file", temporary));
        }
    }
    auto replaced = replace_file(temporary, path);
    if (!replaced) {
        std::filesystem::remove(temporary, code);
        return replaced;
    }
    return {};
}

auto write_once(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> result<void> {
    if (std::filesystem::exists(path)) {
        return {};
    }
    auto written = write_atomic(path, contents);
    if (!written && std::filesystem::exists(path)) {
        return {};
    }
    return written;
}

auto manifest_path(const std::filesystem::path& directory) -> std::filesystem::path {
    return directory / "manifest.cbor";
}

auto blob_path(const std::filesystem::path& directory, const digest& id) -> std::filesystem::path {
    auto name = to_string(id);
    std::ranges::replace(name, ':', '-');
    return directory / "blobs" / (name + ".blob");
}

auto trigram_path(const std::filesystem::path& directory, const digest& id)
    -> std::filesystem::path {
    auto name = to_string(id);
    std::ranges::replace(name, ':', '-');
    return directory / "trigrams" / (name + ".tri");
}

auto term_path(const std::filesystem::path& directory, const digest& id) -> std::filesystem::path {
    auto name = to_string(id);
    std::ranges::replace(name, ':', '-');
    return directory / "terms" / (name + ".term");
}

auto append_u32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    for (auto shift = 0U; shift != 32U; shift += 8U) {
        output.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

auto encode_trigram_positions(std::span<const std::byte> contents)
    -> result<std::vector<std::byte>> {
    if (contents.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error{errc::unsupported,
                                     "search trigram positions require blobs below 4 GiB",
                                     "search index"});
    }
    auto positions = std::map<std::array<std::byte, 3>, std::vector<std::uint32_t>>{};
    for (std::size_t offset = 0; offset + 3 <= contents.size(); ++offset) {
        positions[{contents[offset], contents[offset + 1], contents[offset + 2]}].push_back(
            static_cast<std::uint32_t>(offset));
    }
    constexpr auto magic = std::array<std::byte, 8>{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'T'},
                                                    std::byte{'R'},
                                                    std::byte{'I'},
                                                    std::byte{'0'},
                                                    std::byte{'1'},
                                                    std::byte{0}};
    auto output = std::vector<std::byte>{magic.begin(), magic.end()};
    append_u32(output, static_cast<std::uint32_t>(positions.size()));
    for (const auto& [key, offsets] : positions) {
        output.insert(output.end(), key.begin(), key.end());
        append_u32(output, static_cast<std::uint32_t>(offsets.size()));
        for (const auto offset : offsets) {
            append_u32(output, offset);
        }
    }
    return output;
}

auto encode_terms(std::span<const std::byte> contents) -> result<std::vector<std::byte>> {
    const auto text =
        std::string_view{reinterpret_cast<const char*>(contents.data()), contents.size()};
    const auto tokens = oramacore::tokenize::tokenize(text);
    if (tokens.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            error{errc::unsupported, "search token count exceeds index format", "search index"});
    }
    auto frequencies = std::map<std::string, std::uint32_t>{};
    for (const auto& token : tokens) {
        ++frequencies[token.text];
    }
    constexpr auto magic = std::array<std::byte, 8>{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'T'},
                                                    std::byte{'E'},
                                                    std::byte{'R'},
                                                    std::byte{'M'},
                                                    std::byte{'1'},
                                                    std::byte{0}};
    auto output = std::vector<std::byte>{magic.begin(), magic.end()};
    append_u32(output, static_cast<std::uint32_t>(tokens.size()));
    append_u32(output, static_cast<std::uint32_t>(frequencies.size()));
    for (const auto& [term, frequency] : frequencies) {
        if (term.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(error{
                errc::unsupported, "search term length exceeds index format", "search index"});
        }
        append_u32(output, static_cast<std::uint32_t>(term.size()));
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(term.data()),
                      reinterpret_cast<const std::byte*>(term.data() + term.size()));
        append_u32(output, frequency);
    }
    return output;
}

struct catalog_record {
    std::array<std::byte, 3> key;
    digest blob;
};

auto shard_suffix(std::byte value) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    const auto number = std::to_integer<std::uint8_t>(value);
    auto output = std::string{"-00.cat"};
    output[1] = digits[number >> 4U];
    output[2] = digits[number & 0x0fU];
    return output;
}

auto write_catalog_segment(const std::filesystem::path& directory,
                           std::string file_name,
                           const std::vector<document>& documents) -> result<void> {
    auto shards = std::array<std::vector<catalog_record>, 256>{};
    for (const auto& value : documents) {
        auto keys = std::set<std::array<std::byte, 3>>{};
        for (std::size_t offset = 0; offset + 3 <= value.contents.size(); ++offset) {
            keys.insert(
                {value.contents[offset], value.contents[offset + 1], value.contents[offset + 2]});
        }
        for (const auto& key : keys) {
            shards[std::to_integer<std::uint8_t>(key.front())].push_back({key, value.blob});
        }
    }
    if (file_name.ends_with(".cbor")) {
        file_name.resize(file_name.size() - 5);
    }
    constexpr auto magic = std::array<std::byte, 8>{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'T'},
                                                    std::byte{'C'},
                                                    std::byte{'A'},
                                                    std::byte{'T'},
                                                    std::byte{'1'},
                                                    std::byte{0}};
    for (auto shard = std::size_t{}; shard != shards.size(); ++shard) {
        auto& records = shards[shard];
        if (records.empty()) {
            continue;
        }
        std::ranges::sort(records, [](const auto& left, const auto& right) {
            if (left.key != right.key) {
                return left.key < right.key;
            }
            if (left.blob.alg != right.blob.alg) {
                return left.blob.alg < right.blob.alg;
            }
            return left.blob.bytes < right.blob.bytes;
        });
        const auto unique = std::ranges::unique(records, {}, [](const auto& record) {
            return std::pair{record.key, to_string(record.blob)};
        });
        records.erase(unique.begin(), unique.end());
        auto encoded = std::vector<std::byte>{magic.begin(), magic.end()};
        append_u32(encoded, static_cast<std::uint32_t>(records.size()));
        for (const auto& record : records) {
            encoded.insert(encoded.end(), record.key.begin(), record.key.end());
            encoded.push_back(std::byte{static_cast<std::uint8_t>(record.blob.alg)});
            encoded.insert(encoded.end(), record.blob.bytes.begin(), record.blob.bytes.end());
        }
        const auto path = directory / "catalog" /
                          (file_name + shard_suffix(std::byte{static_cast<std::uint8_t>(shard)}));
        auto written = write_once(path, encoded);
        if (!written) {
            return written;
        }
    }
    return {};
}

auto encode_manifest(const manifest_record& manifest) -> result<std::vector<std::byte>> {
    auto slots = cbor::value::array{};
    for (const auto& [name, slot] : manifest.slots) {
        auto segments = cbor::value::array{};
        for (const auto& segment : slot.segments) {
            segments.emplace_back(segment);
        }
        slots.push_back(map({{"name", cbor::value{name}},
                             {"generation", cbor::value{slot.generation}},
                             {"segments", cbor::value{std::move(segments)}}}));
    }
    return cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}}, {"slots", cbor::value{std::move(slots)}}}));
}

auto decode_manifest(std::span<const std::byte> encoded) -> result<manifest_record> {
    auto decoded = cbor::decode(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = field(*decoded, "v");
    const auto* slots_value = field(*decoded, "slots");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* slots = slots_value ? std::get_if<cbor::value::array>(&slots_value->data) : nullptr;
    if (!version || *version != 1 || !slots) {
        return std::unexpected(error{errc::corrupt, "invalid search manifest", "search index"});
    }
    auto output = manifest_record{};
    for (const auto& value : *slots) {
        const auto* name_value = field(value, "name");
        const auto* generation_value = field(value, "generation");
        const auto* segments_value = field(value, "segments");
        const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
        const auto* generation =
            generation_value ? std::get_if<std::string>(&generation_value->data) : nullptr;
        const auto* segments =
            segments_value ? std::get_if<cbor::value::array>(&segments_value->data) : nullptr;
        if (!name || name->empty() || !generation || !segments || output.slots.contains(*name)) {
            return std::unexpected(
                error{errc::corrupt, "invalid search manifest slot", "search index"});
        }
        auto slot = slot_record{.generation = *generation};
        for (const auto& item : *segments) {
            const auto* segment = std::get_if<std::string>(&item.data);
            if (!segment || segment->empty()) {
                return std::unexpected(
                    error{errc::corrupt, "invalid search segment name", "search index"});
            }
            slot.segments.push_back(*segment);
        }
        output.slots.emplace(*name, std::move(slot));
    }
    return output;
}

auto load_manifest(const std::filesystem::path& directory) -> result<manifest_record> {
    if (!std::filesystem::exists(manifest_path(directory))) {
        return manifest_record{};
    }
    auto encoded = read_file(manifest_path(directory));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return decode_manifest(*encoded);
}

auto publish_manifest(const std::filesystem::path& directory, const manifest_record& manifest)
    -> result<void> {
    auto encoded = encode_manifest(manifest);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return write_atomic(manifest_path(directory), *encoded);
}

auto segment_file_name(std::string_view slot, std::string_view generation) -> result<std::string> {
    auto identity = std::string{slot};
    identity.push_back('\n');
    identity.append(generation);
    auto id = hash(algorithm::blake3,
                   std::span{reinterpret_cast<const std::byte*>(identity.data()), identity.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    const auto text = to_string(*id);
    return "segment-" + text.substr(text.find(':') + 1) + ".cbor";
}

auto encode_segment(const std::vector<document_update>& updates) -> result<std::vector<std::byte>> {
    auto values = cbor::value::array{};
    for (const auto& update : updates) {
        values.push_back(
            map({{"path", cbor::value{update.path}},
                 {"blob",
                  update.value ? digest_value(update.value->blob) : cbor::value{cbor::null{}}}}));
    }
    return cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}}, {"updates", cbor::value{std::move(values)}}}));
}

auto decode_segment(std::span<const std::byte> encoded)
    -> result<std::vector<std::pair<std::string, std::optional<digest>>>> {
    auto decoded = cbor::decode(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = field(*decoded, "v");
    const auto* updates_value = field(*decoded, "updates");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* updates =
        updates_value ? std::get_if<cbor::value::array>(&updates_value->data) : nullptr;
    if (!version || *version != 1 || !updates) {
        return std::unexpected(error{errc::corrupt, "invalid search segment", "search index"});
    }
    auto output = std::vector<std::pair<std::string, std::optional<digest>>>{};
    output.reserve(updates->size());
    for (const auto& value : *updates) {
        const auto* path_value = field(value, "path");
        const auto* blob_value = field(value, "blob");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        if (!path || path->empty() || !blob_value) {
            return std::unexpected(
                error{errc::corrupt, "invalid search segment update", "search index"});
        }
        if (std::holds_alternative<cbor::null>(blob_value->data)) {
            output.emplace_back(*path, std::nullopt);
            continue;
        }
        auto id = decode_digest(*blob_value);
        if (!id) {
            return std::unexpected(id.error());
        }
        output.emplace_back(*path, *id);
    }
    return output;
}

auto store_documents(const std::filesystem::path& directory,
                     const std::vector<document_update>& updates) -> result<void> {
    for (const auto& update : updates) {
        if (!update.value) {
            continue;
        }
        auto actual = hash(algorithm::blake3, update.value->contents);
        if (!actual) {
            return std::unexpected(actual.error());
        }
        if (actual->alg != update.value->blob.alg || actual->bytes != update.value->blob.bytes) {
            return std::unexpected(
                error{errc::corrupt, "search blob identity does not match contents", update.path});
        }
        auto written = write_once(blob_path(directory, update.value->blob), update.value->contents);
        if (!written) {
            return written;
        }
        if (!std::filesystem::exists(trigram_path(directory, update.value->blob))) {
            auto trigram = encode_trigram_positions(update.value->contents);
            if (!trigram) {
                return std::unexpected(trigram.error());
            }
            auto trigram_written =
                write_once(trigram_path(directory, update.value->blob), *trigram);
            if (!trigram_written) {
                return trigram_written;
            }
        }
        if (!std::filesystem::exists(term_path(directory, update.value->blob))) {
            auto terms = encode_terms(update.value->contents);
            if (!terms) {
                return std::unexpected(terms.error());
            }
            auto terms_written = write_once(term_path(directory, update.value->blob), *terms);
            if (!terms_written) {
                return terms_written;
            }
        }
    }
    return {};
}

auto publish_updates(const std::filesystem::path& directory,
                     const std::string& slot,
                     const std::string& generation,
                     const std::vector<document_update>& updates,
                     bool replace) -> result<void> {
    if (slot.empty() || generation.empty()) {
        return std::unexpected(
            error{errc::invalid_argument, "search slot and generation are required", slot});
    }
    auto manifest = load_manifest(directory);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    const auto existing = manifest->slots.find(slot);
    if (existing != manifest->slots.end() && existing->second.generation == generation) {
        return {};
    }
    auto new_documents = std::vector<document>{};
    for (const auto& update : updates) {
        if (update.value && !std::filesystem::exists(blob_path(directory, update.value->blob))) {
            new_documents.push_back(*update.value);
        }
    }
    auto stored = store_documents(directory, updates);
    if (!stored) {
        return stored;
    }
    auto file_name = segment_file_name(slot, generation);
    if (!file_name) {
        return std::unexpected(file_name.error());
    }
    auto encoded = encode_segment(updates);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto segment_written = write_once(directory / "segments" / *file_name, *encoded);
    if (!segment_written) {
        return segment_written;
    }
    auto catalog_written = write_catalog_segment(directory, *file_name, new_documents);
    if (!catalog_written) {
        return catalog_written;
    }
    auto& selected = manifest->slots[slot];
    if (replace) {
        selected.segments.clear();
    }
    selected.generation = generation;
    selected.segments.push_back(*file_name);
    return publish_manifest(directory, *manifest);
}

auto active_paths(const std::filesystem::path& directory, const std::string& slot)
    -> result<std::map<std::string, digest>> {
    auto manifest = load_manifest(directory);
    if (!manifest) {
        return std::unexpected(manifest.error());
    }
    const auto selected = manifest->slots.find(slot);
    if (selected == manifest->slots.end()) {
        return std::unexpected(error{errc::not_found, "search index slot is absent", slot});
    }
    auto output = std::map<std::string, digest>{};
    for (const auto& name : selected->second.segments) {
        auto encoded = read_file(directory / "segments" / name);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto updates = decode_segment(*encoded);
        if (!updates) {
            return std::unexpected(updates.error());
        }
        for (const auto& [path, blob] : *updates) {
            if (blob) {
                output[path] = *blob;
            } else {
                output.erase(path);
            }
        }
    }
    return output;
}

auto load_blob(const std::filesystem::path& directory, const digest& id) -> result<std::string> {
    auto encoded = read_file(blob_path(directory, id));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto actual = hash(algorithm::blake3, *encoded);
    if (!actual) {
        return std::unexpected(actual.error());
    }
    if (actual->alg != id.alg || actual->bytes != id.bytes) {
        return std::unexpected(
            failure(errc::corrupt, "search blob failed verification", blob_path(directory, id)));
    }
    return std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()};
}

auto trigrams(const std::string& text) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    if (text.size() < 3) {
        return output;
    }
    output.reserve(text.size() - 2);
    for (std::size_t index = 0; index + 3 <= text.size(); ++index) {
        output.push_back(text.substr(index, 3));
    }
    return output;
}

auto read_u32(std::istream& input) -> result<std::uint32_t> {
    auto raw = std::array<unsigned char, 4>{};
    input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!input) {
        return std::unexpected(error{errc::corrupt, "truncated trigram index", "search index"});
    }
    auto output = std::uint32_t{};
    for (auto index = 0U; index != raw.size(); ++index) {
        output |= static_cast<std::uint32_t>(raw[index]) << (index * 8U);
    }
    return output;
}

struct term_record {
    std::uint32_t length{};
    std::unordered_map<std::string, std::uint32_t> frequencies;
};

auto read_term_record(const std::filesystem::path& path,
                      const std::unordered_set<std::string>& wanted) -> result<term_record> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::not_found, "term index is absent", path));
    }
    constexpr auto expected = std::array<char, 8>{'L', 'Q', 'T', 'E', 'R', 'M', '1', '\0'};
    auto magic = std::array<char, 8>{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != expected) {
        return std::unexpected(failure(errc::corrupt, "invalid term index", path));
    }
    auto length = read_u32(input);
    auto count = read_u32(input);
    if (!length || !count) {
        return std::unexpected(failure(errc::corrupt, "truncated term index", path));
    }
    auto output = term_record{.length = *length};
    for (auto index = std::uint32_t{}; index != *count; ++index) {
        auto size = read_u32(input);
        if (!size || *size > 1024U * 1024U) {
            return std::unexpected(failure(errc::corrupt, "invalid term length", path));
        }
        auto term = std::string(*size, '\0');
        input.read(term.data(), static_cast<std::streamsize>(term.size()));
        auto frequency = read_u32(input);
        if (!input || !frequency || *frequency == 0) {
            return std::unexpected(failure(errc::corrupt, "truncated term index", path));
        }
        if (wanted.contains(term)) {
            output.frequencies.emplace(std::move(term), *frequency);
        }
    }
    return output;
}

auto tokenize_record(std::string_view text, const std::unordered_set<std::string>& wanted)
    -> term_record {
    const auto tokens = oramacore::tokenize::tokenize(text);
    auto output = term_record{.length = static_cast<std::uint32_t>(tokens.size())};
    for (const auto& token : tokens) {
        if (wanted.contains(token.text)) {
            ++output.frequencies[token.text];
        }
    }
    return output;
}

auto bm25_scores(const std::map<std::string, digest>& paths,
                 const std::map<std::string, term_record>& content_records,
                 const std::map<std::string, term_record>& path_records,
                 const std::vector<std::string>& query_terms,
                 const std::unordered_set<std::string>& wanted)
    -> std::unordered_map<std::string, double> {
    auto output = std::unordered_map<std::string, double>{};
    if (paths.empty() || query_terms.empty()) {
        return output;
    }
    auto content_length = std::uint64_t{};
    auto path_length = std::uint64_t{};
    auto document_frequencies = std::unordered_map<std::string, std::uint64_t>{};
    for (const auto& [path, id] : paths) {
        const auto& content = content_records.at(to_string(id));
        const auto& path_record = path_records.at(path);
        content_length += content.length;
        path_length += path_record.length;
        for (const auto& term : wanted) {
            if (content.frequencies.contains(term) || path_record.frequencies.contains(term)) {
                ++document_frequencies[term];
            }
        }
    }
    const auto count = static_cast<double>(paths.size());
    const auto parameters = std::vector<oramacore::bm25f::FieldParams>{{1.0, 0.75}, {3.0, 0.75}};
    const auto statistics = std::vector<oramacore::bm25f::FieldStats>{
        {static_cast<double>(content_length) / count}, {static_cast<double>(path_length) / count}};
    const auto global = oramacore::bm25f::Params{};
    for (const auto& [path, id] : paths) {
        const auto& content = content_records.at(to_string(id));
        const auto& path_record = path_records.at(path);
        for (const auto& term : query_terms) {
            auto hits = std::vector<oramacore::bm25f::FieldHit>{};
            const auto content_hit = content.frequencies.find(term);
            if (content_hit != content.frequencies.end()) {
                hits.push_back({0, content_hit->second, content.length});
            }
            const auto path_hit = path_record.frequencies.find(term);
            if (path_hit != path_record.frequencies.end()) {
                hits.push_back({1, path_hit->second, path_record.length});
            }
            if (hits.empty()) {
                continue;
            }
            const auto pseudo_frequency = oramacore::bm25f::pseudo_tf(hits, parameters, statistics);
            output[path] += oramacore::bm25f::score_term(
                oramacore::bm25f::idf(paths.size(), document_frequencies.at(term)),
                pseudo_frequency,
                global);
        }
    }
    return output;
}

auto normalized_query_terms(std::string_view text) -> std::vector<std::string> {
    auto unique = std::set<std::string>{};
    for (const auto& token : oramacore::tokenize::tokenize(text)) {
        unique.insert(token.text);
    }
    return {unique.begin(), unique.end()};
}

auto indexed_contiguous_positions(const std::filesystem::path& path,
                                  const std::vector<std::string>& keys) -> result<bool> {
    if (keys.empty()) {
        return true;
    }
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::not_found, "trigram index is absent", path));
    }
    constexpr auto expected = std::array<char, 8>{'L', 'Q', 'T', 'R', 'I', '0', '1', '\0'};
    auto magic = std::array<char, 8>{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != expected) {
        return std::unexpected(failure(errc::corrupt, "invalid trigram index", path));
    }
    auto count = read_u32(input);
    if (!count) {
        return std::unexpected(count.error());
    }
    auto wanted = std::unordered_set<std::string>{keys.begin(), keys.end()};
    auto matches = std::unordered_map<std::string, std::vector<std::uint32_t>>{};
    for (auto entry = std::uint32_t{}; entry != *count; ++entry) {
        auto key = std::array<char, 3>{};
        input.read(key.data(), static_cast<std::streamsize>(key.size()));
        if (!input) {
            return std::unexpected(failure(errc::corrupt, "truncated trigram key", path));
        }
        auto positions = read_u32(input);
        if (!positions) {
            return std::unexpected(positions.error());
        }
        const auto text = std::string{key.data(), key.size()};
        if (!wanted.contains(text)) {
            input.seekg(static_cast<std::streamoff>(*positions) * 4, std::ios::cur);
            if (!input) {
                return std::unexpected(failure(errc::corrupt, "truncated trigram positions", path));
            }
            continue;
        }
        auto& output = matches[text];
        output.reserve(*positions);
        for (auto position = std::uint32_t{}; position != *positions; ++position) {
            auto value = read_u32(input);
            if (!value) {
                return std::unexpected(value.error());
            }
            output.push_back(*value);
        }
    }
    const auto initial = matches.find(keys.front());
    if (initial == matches.end()) {
        return false;
    }
    for (const auto position : initial->second) {
        auto found = true;
        for (std::size_t offset = 1; offset != keys.size(); ++offset) {
            const auto next = matches.find(keys[offset]);
            if (next == matches.end() ||
                !std::ranges::binary_search(next->second,
                                            position + static_cast<std::uint32_t>(offset))) {
                found = false;
                break;
            }
        }
        if (found) {
            return true;
        }
    }
    return false;
}

auto catalog_candidates(const std::filesystem::path& directory,
                        const std::vector<std::string>& keys)
    -> result<std::unordered_set<std::string>> {
    auto wanted = std::unordered_set<std::string>{keys.begin(), keys.end()};
    auto postings = std::unordered_map<std::string, std::unordered_set<std::string>>{};
    auto suffixes = std::unordered_set<std::string>{};
    for (const auto& key : wanted) {
        suffixes.insert(shard_suffix(std::byte{static_cast<std::uint8_t>(key.front())}));
    }
    const auto catalog = directory / "catalog";
    if (!std::filesystem::exists(catalog)) {
        return std::unordered_set<std::string>{};
    }
    auto code = std::error_code{};
    for (const auto& entry : std::filesystem::directory_iterator{catalog, code}) {
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), catalog));
        }
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || std::ranges::none_of(suffixes, [&](const auto& suffix) {
                return name.ends_with(suffix);
            })) {
            continue;
        }
        auto input = std::ifstream{entry.path(), std::ios::binary};
        constexpr auto expected = std::array<char, 8>{'L', 'Q', 'T', 'C', 'A', 'T', '1', '\0'};
        auto magic = std::array<char, 8>{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        if (!input || magic != expected) {
            return std::unexpected(failure(errc::corrupt, "invalid trigram catalog", entry.path()));
        }
        auto count = read_u32(input);
        if (!count) {
            return std::unexpected(count.error());
        }
        for (auto record = std::uint32_t{}; record != *count; ++record) {
            auto key = std::array<char, 3>{};
            auto algorithm_value = std::uint8_t{};
            auto digest_bytes = std::array<std::byte, 32>{};
            input.read(key.data(), static_cast<std::streamsize>(key.size()));
            input.read(reinterpret_cast<char*>(&algorithm_value), 1);
            input.read(reinterpret_cast<char*>(digest_bytes.data()),
                       static_cast<std::streamsize>(digest_bytes.size()));
            if (!input || algorithm_value < 1 || algorithm_value > 3) {
                return std::unexpected(
                    failure(errc::corrupt, "truncated trigram catalog", entry.path()));
            }
            const auto text = std::string{key.data(), key.size()};
            if (wanted.contains(text)) {
                postings[text].insert(
                    to_string(digest{static_cast<algorithm>(algorithm_value), digest_bytes}));
            }
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), catalog));
    }
    const auto initial = postings.find(keys.front());
    if (initial == postings.end()) {
        return std::unordered_set<std::string>{};
    }
    auto output = initial->second;
    for (std::size_t index = 1; index != keys.size(); ++index) {
        const auto found = postings.find(keys[index]);
        if (found == postings.end()) {
            output.clear();
            break;
        }
        std::erase_if(output, [&](const auto& blob) { return !found->second.contains(blob); });
    }
    return output;
}

auto regex_literal(const std::string& expression) -> std::string {
    constexpr auto metacharacters = std::string_view{"^$|()[]{}*+?.\\"};
    auto output = std::string{};
    auto escaped = false;
    for (const auto character : expression) {
        if (escaped) {
            if (!metacharacters.contains(character)) {
                return {};
            }
            output.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (metacharacters.contains(character)) {
            return {};
        }
        output.push_back(character);
    }
    if (escaped) {
        return {};
    }
    return output;
}

auto snippet_for(const std::string& content, const std::string& needle) -> std::string {
    auto at = content.find(needle);
    if (at == std::string::npos) {
        at = 0;
    }
    const auto begin = at > 48 ? at - 48 : 0;
    const auto length = std::min<std::size_t>(160, content.size() - begin);
    auto output = content.substr(begin, length);
    std::ranges::replace(output, '\n', ' ');
    std::ranges::replace(output, '\r', ' ');
    return output;
}

auto ignored_path(const std::filesystem::path& relative) -> bool {
    if (relative.empty()) {
        return true;
    }
    for (const auto& component : relative) {
        if (component == ".git" || component == ".liquid") {
            return true;
        }
    }
    const auto first = *relative.begin();
    const auto first_name = first.string();
    return first == ".git" || first == ".liquid" || first == ".liquid-store" ||
           first == ".liquid-build" || first_name == "build" || first_name.starts_with("build-") ||
           relative == "LIQUID_REPO" || relative == "LIQUID_JOURNAL";
}

auto read_document(const std::filesystem::path& checkout, const std::filesystem::path& relative)
    -> result<document> {
    const auto path = checkout / relative;
    auto contents = read_file(path);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    auto id = hash(algorithm::blake3, *contents);
    if (!id) {
        return std::unexpected(id.error());
    }
    return document{relative.generic_string(), *id, std::move(*contents)};
}

auto scan_checkout(const std::filesystem::path& checkout) -> result<std::vector<document>> {
    auto output = std::vector<document>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 checkout, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        const auto relative = iterator->path().lexically_relative(checkout);
        if (ignored_path(relative)) {
            if (iterator->is_directory(code)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (!iterator->is_regular_file(code)) {
            continue;
        }
        auto value = read_document(checkout, relative);
        if (!value) {
            return std::unexpected(value.error());
        }
        output.push_back(std::move(*value));
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), checkout));
    }
    std::ranges::sort(output, {}, &document::path);
    return output;
}

auto checkout_updates(const std::filesystem::path& checkout, const drip& value)
    -> result<std::vector<document_update>> {
    auto paths = std::set<std::string>{};
    for (const auto& operation : value.ops) {
        paths.insert(operation.path);
    }
    auto output = std::vector<document_update>{};
    for (const auto& path_text : paths) {
        const auto relative = std::filesystem::path{path_text}.lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == ".." ||
            ignored_path(relative)) {
            continue;
        }
        auto code = std::error_code{};
        const auto path = checkout / relative;
        if (std::filesystem::is_regular_file(path, code)) {
            auto value = read_document(checkout, relative);
            if (!value) {
                return std::unexpected(value.error());
            }
            output.push_back({relative.generic_string(), std::move(*value)});
        } else if (!code && !std::filesystem::exists(path)) {
            output.push_back({relative.generic_string(), std::nullopt});
        } else if (code) {
            return std::unexpected(failure(errc::io, code.message(), path));
        }
    }
    return output;
}

class index_daemon_target final : public daemon_target {
public:
    explicit index_daemon_target(std::filesystem::path checkout)
        : checkout_(std::move(checkout)), directory_(checkout_ / ".liquid" / "index") {}

    auto name() const -> std::string override {
        return "search";
    }

    runtime::task<void> deliver(daemon_delivery delivery) override {
        auto updated = co_await runtime::offload(
            [checkout = checkout_, directory = directory_, value = std::move(delivery.value)] {
                auto present = has_slot(directory, std::string{working_slot});
                if (!present) {
                    return result<void>{std::unexpected(present.error())};
                }
                const auto generation = to_string(value.id.value);
                if (!*present) {
                    return publish_checkout(checkout, directory, generation);
                }
                auto updates = checkout_updates(checkout, value);
                if (!updates) {
                    return result<void>{std::unexpected(updates.error())};
                }
                return publish_delta(
                    directory, std::string{working_slot}, generation, std::move(*updates));
            });
        if (!updated) {
            co_return std::unexpected(updated.error());
        }
        if (!*updated) {
            co_return std::unexpected(updated->error());
        }
        co_return result<void>{};
    }

private:
    std::filesystem::path checkout_;
    std::filesystem::path directory_;
};

struct query_plan {
    bool regex{};
    bool substring{};
    std::string needle;
    std::optional<std::regex> expression;
};

struct query_matches {
    std::unordered_map<std::string, double> blob_scores;
    std::unordered_map<std::string, double> path_scores;
    std::unordered_map<std::string, std::string> contents;
};

auto plan_query(const std::string& text) -> result<query_plan> {
    auto output = query_plan{};
    output.regex = text.size() > 2 && text.front() == '/' && text.back() == '/';
    output.substring = text.size() > 1 && text.front() == '"' && text.back() == '"';
    output.needle = output.regex || output.substring ? text.substr(1, text.size() - 2) : text;
    if (!output.regex) {
        return output;
    }
    try {
        output.expression.emplace(output.needle);
    } catch (const std::regex_error& caught) {
        return std::unexpected(
            error{errc::invalid_argument, "invalid search regex: " + std::string{caught.what()}});
    }
    return output;
}

auto unique_blobs(const std::map<std::string, digest>& paths) -> std::map<std::string, digest> {
    auto output = std::map<std::string, digest>{};
    for (const auto& [path, id] : paths) {
        static_cast<void>(path);
        output.emplace(to_string(id), id);
    }
    return output;
}

auto ranked_matches(const std::filesystem::path& directory,
                    const std::map<std::string, digest>& paths,
                    const std::map<std::string, digest>& blobs,
                    const std::string& needle) -> result<query_matches> {
    const auto query_terms = normalized_query_terms(needle);
    const auto wanted = std::unordered_set<std::string>{query_terms.begin(), query_terms.end()};
    auto content_records = std::map<std::string, term_record>{};
    for (const auto& [key, id] : blobs) {
        auto record = read_term_record(term_path(directory, id), wanted);
        if (!record) {
            return std::unexpected(record.error());
        }
        content_records.emplace(key, std::move(*record));
    }
    auto path_records = std::map<std::string, term_record>{};
    for (const auto& [path, id] : paths) {
        static_cast<void>(id);
        path_records.emplace(path, tokenize_record(path, wanted));
    }
    auto output = query_matches{};
    output.path_scores = bm25_scores(paths, content_records, path_records, query_terms, wanted);
    return output;
}

auto positional_matches(const std::filesystem::path& directory,
                        const std::map<std::string, digest>& blobs,
                        const query_plan& plan) -> result<query_matches> {
    const auto prefilter = plan.regex ? regex_literal(plan.needle) : plan.needle;
    const auto keys = trigrams(prefilter);
    auto candidates = std::unordered_set<std::string>{};
    if (!keys.empty()) {
        auto catalog = catalog_candidates(directory, keys);
        if (!catalog) {
            return std::unexpected(catalog.error());
        }
        candidates = std::move(*catalog);
    } else {
        for (const auto& [key, id] : blobs) {
            static_cast<void>(id);
            candidates.insert(key);
        }
    }
    auto output = query_matches{};
    for (const auto& [key, id] : blobs) {
        if (!candidates.contains(key)) {
            continue;
        }
        if (!keys.empty()) {
            auto candidate = indexed_contiguous_positions(trigram_path(directory, id), keys);
            if (!candidate) {
                return std::unexpected(candidate.error());
            }
            if (!*candidate) {
                continue;
            }
        }
        auto content = load_blob(directory, id);
        if (!content) {
            return std::unexpected(content.error());
        }
        const auto matched = plan.regex ? std::regex_search(*content, *plan.expression)
                                        : content->find(plan.needle) != std::string::npos;
        if (!matched) {
            continue;
        }
        output.contents.emplace(key, std::move(*content));
        output.blob_scores[key] = 1.0;
    }
    return output;
}

auto render_hits(const std::filesystem::path& directory,
                 const std::map<std::string, digest>& paths,
                 const query_plan& plan,
                 query_matches matches,
                 std::uint64_t limit) -> result<std::vector<search_hit>> {
    auto output = std::vector<search_hit>{};
    const auto snippet_needle = plan.regex ? regex_literal(plan.needle) : plan.needle;
    for (const auto& [path, id] : paths) {
        const auto key = to_string(id);
        const auto blob_score = matches.blob_scores.find(key);
        const auto path_score = matches.path_scores.find(path);
        if (blob_score == matches.blob_scores.end() && path_score == matches.path_scores.end()) {
            continue;
        }
        if (!matches.contents.contains(key)) {
            auto content = load_blob(directory, id);
            if (!content) {
                return std::unexpected(content.error());
            }
            matches.contents.emplace(key, std::move(*content));
        }
        const auto score = (blob_score == matches.blob_scores.end() ? 0.0 : blob_score->second) +
                           (path_score == matches.path_scores.end() ? 0.0 : path_score->second);
        output.push_back({path, id, snippet_for(matches.contents.at(key), snippet_needle), score});
    }
    std::ranges::sort(output, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.path < right.path;
    });
    if (output.size() > limit) {
        output.resize(static_cast<std::size_t>(limit));
    }
    return output;
}

} // namespace

auto has_slot(const std::filesystem::path& directory, const std::string& slot) -> result<bool> {
    try {
        auto manifest = load_manifest(directory);
        if (!manifest) {
            return std::unexpected(manifest.error());
        }
        return manifest->slots.contains(slot);
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), directory.string()});
    }
}

auto publish_full(const std::filesystem::path& directory,
                  const std::string& slot,
                  const std::string& generation,
                  const std::vector<document>& documents) -> result<void> {
    auto updates = std::vector<document_update>{};
    updates.reserve(documents.size());
    for (const auto& value : documents) {
        updates.push_back({value.path, value});
    }
    try {
        return publish_updates(directory, slot, generation, updates, true);
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), directory.string()});
    }
}

auto publish_delta(const std::filesystem::path& directory,
                   const std::string& slot,
                   const std::string& generation,
                   const std::vector<document_update>& updates) -> result<void> {
    if (updates.empty()) {
        return {};
    }
    try {
        return publish_updates(directory, slot, generation, updates, false);
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), directory.string()});
    }
}

auto query(const std::filesystem::path& directory,
           const std::string& slot,
           const std::string& text,
           std::uint64_t limit) -> result<std::vector<search_hit>> {
    try {
        if (text.empty()) {
            return std::unexpected(error{errc::invalid_argument, "search query is required"});
        }
        if (limit == 0 || limit > 1000) {
            return std::unexpected(
                error{errc::invalid_argument, "search limit must be between 1 and 1000"});
        }
        auto paths = active_paths(directory, slot);
        if (!paths) {
            return std::unexpected(paths.error());
        }
        auto plan = plan_query(text);
        if (!plan) {
            return std::unexpected(plan.error());
        }
        const auto blobs = unique_blobs(*paths);
        auto matches = plan->regex || plan->substring
                           ? positional_matches(directory, blobs, *plan)
                           : ranked_matches(directory, *paths, blobs, plan->needle);
        if (!matches) {
            return std::unexpected(matches.error());
        }
        return render_hits(directory, *paths, *plan, std::move(*matches), limit);
    } catch (const std::exception& caught) {
        return std::unexpected(error{errc::internal, caught.what(), directory.string()});
    }
}

auto statistics(const std::filesystem::path& directory, const std::string& slot)
    -> result<index_statistics> {
    auto paths = active_paths(directory, slot);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    auto blobs = std::set<std::string>{};
    auto output = index_statistics{.paths = paths->size()};
    for (const auto& [path, id] : *paths) {
        static_cast<void>(path);
        if (!blobs.insert(to_string(id)).second) {
            continue;
        }
        auto code = std::error_code{};
        output.bytes += std::filesystem::file_size(blob_path(directory, id), code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), blob_path(directory, id)));
        }
    }
    output.unique_blobs = blobs.size();
    return output;
}

auto publish_checkout(const std::filesystem::path& checkout,
                      const std::filesystem::path& directory,
                      const std::string& generation) -> result<void> {
    auto documents = scan_checkout(checkout);
    if (!documents) {
        return std::unexpected(documents.error());
    }
    return publish_full(directory, std::string{working_slot}, generation, *documents);
}

auto make_daemon_target(std::filesystem::path checkout) -> std::shared_ptr<daemon_target> {
    return std::make_shared<index_daemon_target>(std::move(checkout));
}

} // namespace liquid::search_detail
