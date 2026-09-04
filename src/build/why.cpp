#include "liquid/build.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "build/cache.hpp"
#include "build/policy.hpp"
#include "build/repository_access.hpp"
#include "build/why.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {
namespace {

auto why_error(errc code, std::string message, const std::filesystem::path& context = {}) -> error {
    return {code, std::move(message), context.string()};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto hex(std::string_view input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto character : input) {
        const auto value = static_cast<unsigned char>(character);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

auto unhex(std::string_view input) -> result<std::string> {
    if (input.size() % 2 != 0) {
        return std::unexpected(why_error(errc::corrupt, "invalid encoded tool path"));
    }
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    auto output = std::string{};
    output.reserve(input.size() / 2);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 2) {
        const auto high = digit(input[offset]);
        const auto low = digit(input[offset + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(why_error(errc::corrupt, "invalid encoded tool path"));
        }
        output.push_back(static_cast<char>((high << 4U) | low));
    }
    return output;
}

auto project_key(const std::filesystem::path& root) -> result<std::string> {
    auto code = std::error_code{};
    const auto canonical = std::filesystem::weakly_canonical(root, code);
    if (code) {
        return std::unexpected(why_error(errc::io, code.message(), root));
    }
    const auto material = canonical.generic_string();
    auto value = hash(algorithm::blake3,
                      {reinterpret_cast<const std::byte*>(material.data()), material.size()});
    if (!value) {
        return std::unexpected(value.error());
    }
    auto text = to_string(*value);
    std::ranges::replace(text, ':', '-');
    return text;
}

auto load_result(repository& source, digest id) -> runtime::task<action_result> {
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    co_return objects::decode_result(*bytes);
}

auto load_action(repository& source, digest id) -> runtime::task<objects::action_record> {
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    co_return objects::decode_action(*bytes);
}

auto manifest_digest(repository& source, digest tree_id, std::string_view path)
    -> runtime::task<std::optional<digest>> {
    auto current = tree_id;
    auto offset = std::size_t{};
    while (offset < path.size()) {
        const auto separator = path.find('/', offset);
        const auto name = path.substr(offset,
                                      separator == std::string_view::npos ? path.size() - offset
                                                                          : separator - offset);
        auto bytes = co_await repository_detail::build_access::get(source, current);
        if (!bytes) {
            if (bytes.error().code == errc::not_found) {
                co_return std::nullopt;
            }
            co_return std::unexpected(bytes.error());
        }
        auto decoded = objects::decode_tree(*bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto found = std::ranges::find(decoded->entries, name, &tree_entry::name);
        if (found == decoded->entries.end()) {
            co_return std::nullopt;
        }
        if (separator == std::string_view::npos) {
            if (found->kind != node_kind::blob) {
                co_return std::nullopt;
            }
            co_return found->ref;
        }
        if (found->kind != node_kind::tree) {
            co_return std::nullopt;
        }
        current = found->ref;
        offset = separator + 1;
    }
    co_return std::nullopt;
}

auto blob_contents(repository& source, digest id) -> runtime::task<std::vector<std::byte>> {
    auto encoded = co_await repository_detail::build_access::get(source, id);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = objects::decode_blob(*encoded);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    if (record->size > std::numeric_limits<std::size_t>::max()) {
        co_return std::unexpected(why_error(errc::io, "input is too large to compare"));
    }
    if (record->content.alg == algorithm::blake3 || record->content.alg == algorithm::sha256 ||
        record->content.alg == algorithm::blake3_keyed) {
        auto contents = co_await repository_detail::build_access::get(source, record->content);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        if (contents->size() != record->size) {
            co_return std::unexpected(why_error(errc::corrupt, "input blob size does not match"));
        }
        co_return contents;
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(record->size));
    for (const auto& chunk : record->chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != record->size) {
        co_return std::unexpected(why_error(errc::corrupt, "input blob size does not match"));
    }
    co_return output;
}

auto text_input(std::span<const std::byte> bytes) -> bool {
    for (auto index = std::size_t{}; index < bytes.size();) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        if (value < 0x80) {
            if (value != '\n' && value != '\r' && value != '\t' && value < 0x20) {
                return false;
            }
            ++index;
            continue;
        }
        auto length = std::size_t{};
        auto minimum_second = 0x80U;
        auto maximum_second = 0xbfU;
        if (value >= 0xc2 && value <= 0xdf) {
            length = 2;
        } else if (value >= 0xe0 && value <= 0xef) {
            length = 3;
            if (value == 0xe0) {
                minimum_second = 0xa0;
            } else if (value == 0xed) {
                maximum_second = 0x9f;
            }
        } else if (value >= 0xf0 && value <= 0xf4) {
            length = 4;
            if (value == 0xf0) {
                minimum_second = 0x90;
            } else if (value == 0xf4) {
                maximum_second = 0x8f;
            }
        } else {
            return false;
        }
        if (index + length > bytes.size()) {
            return false;
        }
        const auto second = std::to_integer<unsigned char>(bytes[index + 1]);
        if (second < minimum_second || second > maximum_second) {
            return false;
        }
        for (auto continuation = std::size_t{2}; continuation < length; ++continuation) {
            const auto byte = std::to_integer<unsigned char>(bytes[index + continuation]);
            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

auto lines(std::span<const std::byte> bytes) -> std::vector<std::string_view> {
    const auto source = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    auto output = std::vector<std::string_view>{};
    auto offset = std::size_t{};
    while (offset < source.size()) {
        const auto newline = source.find('\n', offset);
        output.push_back(source.substr(
            offset, newline == std::string_view::npos ? source.size() - offset : newline - offset));
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return output;
}

auto line_difference(std::span<const std::byte> old_bytes, std::span<const std::byte> new_bytes)
    -> std::optional<line_change> {
    if (!text_input(old_bytes) || !text_input(new_bytes)) {
        return std::nullopt;
    }
    const auto old_lines = lines(old_bytes);
    const auto new_lines = lines(new_bytes);
    auto prefix = std::size_t{};
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    auto suffix = std::size_t{};
    while (suffix < old_lines.size() - prefix && suffix < new_lines.size() - prefix &&
           old_lines[old_lines.size() - suffix - 1] == new_lines[new_lines.size() - suffix - 1]) {
        ++suffix;
    }
    return line_change{prefix + 1,
                       old_lines.size() - prefix - suffix,
                       prefix + 1,
                       new_lines.size() - prefix - suffix};
}

auto changed_tool(repository& source, digest old_id, digest new_id) -> runtime::task<std::string> {
    auto old_bytes = co_await repository_detail::build_access::get(source, old_id);
    auto new_bytes = co_await repository_detail::build_access::get(source, new_id);
    if (!old_bytes || !new_bytes) {
        co_return std::string{};
    }
    auto old_tree = objects::decode_tree(*old_bytes);
    auto new_tree = objects::decode_tree(*new_bytes);
    if (!old_tree || !new_tree) {
        co_return std::string{};
    }
    auto old_entries = std::map<std::string, digest>{};
    auto new_entries = std::map<std::string, digest>{};
    for (const auto& entry : old_tree->entries) {
        old_entries.emplace(entry.name, entry.ref);
    }
    for (const auto& entry : new_tree->entries) {
        new_entries.emplace(entry.name, entry.ref);
    }
    auto names = std::set<std::string>{};
    for (const auto& [name, id] : old_entries) {
        names.insert(name);
    }
    for (const auto& [name, id] : new_entries) {
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto old_entry = old_entries.find(name);
        const auto new_entry = new_entries.find(name);
        if (old_entry != old_entries.end() && new_entry != new_entries.end() &&
            same_digest(old_entry->second, new_entry->second)) {
            continue;
        }
        const auto separator = name.find('-');
        auto decoded = unhex(separator == std::string::npos ? name : name.substr(separator + 1));
        co_return decoded ? *decoded : name;
    }
    co_return std::string{};
}

auto load_stats(const std::filesystem::path& path) -> result<cache_report> {
    auto output = cache_report{};
    if (!std::filesystem::exists(path)) {
        return output;
    }
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(why_error(errc::io, "cannot read cache statistics", path));
    }
    auto key = std::string{};
    auto value = std::uint64_t{};
    while (input >> key >> value) {
        if (key == "hits") {
            output.hits = value;
        } else if (key == "misses") {
            output.misses = value;
        } else if (key == "bytes_served") {
            output.bytes_served = value;
        } else if (key == "bytes_rebuilt") {
            output.bytes_rebuilt = value;
        } else if (key == "bytes_avoided") {
            output.disk.bytes_avoided = value;
        } else if (key == "disk.hits") {
            output.disk.hits = value;
        } else if (key == "disk.misses") {
            output.disk.misses = value;
        } else if (key == "disk.bytes_transferred") {
            output.disk.bytes_transferred = value;
        } else if (key == "remote.available") {
            output.remote.available = value != 0;
        } else if (key == "remote.hits") {
            output.remote.hits = value;
        } else if (key == "remote.misses") {
            output.remote.misses = value;
        } else if (key == "remote.bytes_transferred") {
            output.remote.bytes_transferred = value;
        } else if (key == "remote.bytes_avoided") {
            output.remote.bytes_avoided = value;
        } else if (key.starts_with("reason.")) {
            auto index = unsigned{};
            const auto text = std::string_view{key}.substr(7);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                index > static_cast<unsigned>(cache_miss_reason::non_hermetic_read)) {
                return std::unexpected(why_error(errc::corrupt, "invalid cache statistics", path));
            }
            output.misses_by_reason[static_cast<cache_miss_reason>(index)] = value;
        }
    }
    if (!input.eof()) {
        return std::unexpected(why_error(errc::corrupt, "invalid cache statistics", path));
    }
    if (output.disk.hits == 0 && output.disk.misses == 0 && output.remote.hits == 0 &&
        output.remote.misses == 0) {
        output.disk.hits = output.hits;
        output.disk.misses = output.misses;
        output.disk.bytes_transferred = output.bytes_served;
    }
    return output;
}

auto save_stats(const std::filesystem::path& path, const cache_report& value) -> result<void> {
    auto temporary = path;
    temporary += ".tmp";
    auto output = std::ofstream{temporary, std::ios::trunc};
    if (!output) {
        return std::unexpected(why_error(errc::io, "cannot write cache statistics", path));
    }
    output << "hits " << value.hits << '\n';
    output << "misses " << value.misses << '\n';
    output << "bytes_served " << value.bytes_served << '\n';
    output << "bytes_avoided " << value.disk.bytes_avoided << '\n';
    output << "bytes_rebuilt " << value.bytes_rebuilt << '\n';
    output << "disk.hits " << value.disk.hits << '\n';
    output << "disk.misses " << value.disk.misses << '\n';
    output << "disk.bytes_transferred " << value.disk.bytes_transferred << '\n';
    output << "remote.available " << value.remote.available << '\n';
    output << "remote.hits " << value.remote.hits << '\n';
    output << "remote.misses " << value.remote.misses << '\n';
    output << "remote.bytes_transferred " << value.remote.bytes_transferred << '\n';
    output << "remote.bytes_avoided " << value.remote.bytes_avoided << '\n';
    for (const auto& [reason, count] : value.misses_by_reason) {
        output << "reason." << static_cast<unsigned>(reason) << ' ' << count << '\n';
    }
    output.close();
    if (!output) {
        return std::unexpected(why_error(errc::io, "cannot write cache statistics", path));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        return std::unexpected(why_error(errc::io, code.message(), path));
    }
    return {};
}

auto result_matches(repository& source,
                    const lookup_record& lookup,
                    std::string_view target,
                    const std::optional<digest>& parsed) -> runtime::task<bool> {
    if (parsed && same_digest(lookup.result.value, *parsed)) {
        co_return true;
    }
    auto value = co_await load_result(source, lookup.result.value);
    if (!value) {
        co_return false;
    }
    if (parsed && same_digest(value->action, *parsed)) {
        co_return true;
    }
    co_return std::ranges::any_of(
        value->outputs, [&](const action_output& output) { return output.path == target; });
}

auto quarantine_detail(const objects::action_index_entry& entry) -> std::string {
    auto detail = std::string{"the action index quarantines this divergent result"};
    if (entry.verification) {
        detail += "; verification attestation: ";
        detail += to_string(*entry.verification);
    }
    return detail;
}

} // namespace

auto wrap_cache_directory(const std::filesystem::path& working_tree,
                          const std::optional<std::filesystem::path>& requested)
    -> result<std::filesystem::path> {
    if (requested) {
        return *requested;
    }
    auto key = project_key(working_tree);
    if (!key) {
        return std::unexpected(key.error());
    }
    if (const auto* configured = std::getenv("LIQUID_WRAP_CACHE")) {
        return std::filesystem::path{configured} / *key;
    }
    if (const auto* configured = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path{configured} / "liquid/wrap" / *key;
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".cache/liquid/wrap" / *key;
    }
    return working_tree / ".liquid-wrap-cache" / *key;
}

auto append_lookup(const std::filesystem::path& action_directory, const lookup_record& value)
    -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(action_directory, code);
    if (code) {
        return std::unexpected(why_error(errc::io, code.message(), action_directory));
    }
    const auto path = action_directory / "LOOKUPS";
    auto output = std::ofstream{path, std::ios::app};
    if (!output) {
        return std::unexpected(why_error(errc::io, "cannot append cache lookup", path));
    }
    output << (value.status == cache_status::hit ? 'H' : 'M') << '\t'
           << to_string(value.result.value) << '\t'
           << (value.previous ? to_string(value.previous->value) : "-") << '\n';
    if (!output) {
        return std::unexpected(why_error(errc::io, "cannot append cache lookup", path));
    }
    return {};
}

auto read_lookups(const std::filesystem::path& action_directory)
    -> result<std::vector<lookup_record>> {
    const auto path = action_directory / "LOOKUPS";
    auto output = std::vector<lookup_record>{};
    if (!std::filesystem::exists(path)) {
        return output;
    }
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(why_error(errc::io, "cannot read cache lookups", path));
    }
    auto line = std::string{};
    while (std::getline(input, line)) {
        auto fields = std::istringstream{line};
        auto status = char{};
        auto result_text = std::string{};
        auto previous_text = std::string{};
        if (!(fields >> status >> result_text >> previous_text) ||
            (status != 'H' && status != 'M')) {
            return std::unexpected(why_error(errc::corrupt, "invalid cache lookup", path));
        }
        auto parsed_result = parse_digest(result_text);
        if (!parsed_result) {
            return std::unexpected(parsed_result.error());
        }
        auto previous = std::optional<result_id>{};
        if (previous_text != "-") {
            auto parsed = parse_digest(previous_text);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            previous = liquid::result_id{*parsed};
        }
        output.push_back(
            {status == 'H' ? cache_status::hit : cache_status::miss, {*parsed_result}, previous});
    }
    return output;
}

auto classify_miss(repository& source,
                   const action_input& actual,
                   digest actual_sandbox,
                   std::optional<result_id> previous,
                   const action_result* actual_result) -> runtime::task<cache_miss> {
    if (!previous) {
        co_return cache_miss{.reason = cache_miss_reason::never_built,
                             .detail = "no recorded result produced this target"};
    }
    auto old_result = co_await load_result(source, previous->value);
    if (!old_result) {
        co_return cache_miss{.reason = cache_miss_reason::unknown,
                             .detail = "the previous result record is unavailable"};
    }
    auto old_action = co_await load_action(source, old_result->action);
    if (!old_action) {
        co_return cache_miss{.reason = cache_miss_reason::unknown,
                             .detail = "the previous action record is unavailable"};
    }
    if (old_action->input.command != actual.command) {
        co_return cache_miss{.reason = cache_miss_reason::command_changed,
                             .expected_command = old_action->input.command,
                             .actual_command = actual.command};
    }
    auto environment_keys = std::set<std::string>{};
    for (const auto& [key, value] : old_action->input.env) {
        environment_keys.insert(key);
    }
    for (const auto& [key, value] : actual.env) {
        environment_keys.insert(key);
    }
    for (const auto& key : environment_keys) {
        const auto old_value = old_action->input.env.find(key);
        const auto new_value = actual.env.find(key);
        if (old_value != old_action->input.env.end() && new_value != actual.env.end() &&
            old_value->second == new_value->second) {
            continue;
        }
        auto miss =
            cache_miss{.reason = cache_miss_reason::environment_changed, .environment_key = key};
        if (old_value != old_action->input.env.end()) {
            miss.expected_value = old_value->second;
        }
        if (new_value != actual.env.end()) {
            miss.actual_value = new_value->second;
        }
        co_return miss;
    }
    if (!same_digest(old_action->input.toolchain, actual.toolchain)) {
        auto tool = co_await changed_tool(source, old_action->input.toolchain, actual.toolchain);
        if (!tool) {
            co_return std::unexpected(tool.error());
        }
        co_return cache_miss{.reason = cache_miss_reason::toolchain_changed,
                             .expected = old_action->input.toolchain,
                             .actual = actual.toolchain,
                             .tool = std::move(*tool)};
    }
    if (!same_digest(old_action->sandbox, actual_sandbox)) {
        co_return cache_miss{.reason = cache_miss_reason::sandbox_changed,
                             .expected = old_action->sandbox,
                             .actual = actual_sandbox};
    }
    for (const auto& expected : old_result->manifest) {
        auto actual_digest = result<std::optional<digest>>{std::nullopt};
        if (actual_result) {
            const auto found =
                std::ranges::find(actual_result->manifest, expected.path, &observed_input::path);
            if (found != actual_result->manifest.end()) {
                actual_digest = found->content;
            }
        } else {
            actual_digest = co_await manifest_digest(source, actual.inputs, expected.path);
            if (!actual_digest) {
                co_return std::unexpected(actual_digest.error());
            }
        }
        if (*actual_digest && same_digest(expected.content, **actual_digest)) {
            continue;
        }
        auto miss = cache_miss{.reason = cache_miss_reason::input_changed,
                               .path = expected.path,
                               .expected = expected.content,
                               .actual = *actual_digest};
        if (*actual_digest) {
            auto old_contents = co_await blob_contents(source, expected.content);
            auto new_contents = co_await blob_contents(source, **actual_digest);
            if (old_contents && new_contents) {
                miss.lines = line_difference(*old_contents, *new_contents);
            }
            auto attribution = co_await repository_detail::build_access::path_change(
                source, expected.path, **actual_digest);
            if (!attribution) {
                co_return std::unexpected(attribution.error());
            }
            miss.changed_by = *attribution;
        }
        co_return miss;
    }
    co_return cache_miss{.reason = cache_miss_reason::unknown,
                         .detail = "recorded actions differ in no supported field"};
}

auto output_bytes(repository& source, const action_result& value) -> runtime::task<std::uint64_t> {
    auto seen = std::vector<digest>{};
    auto total = std::uint64_t{};
    for (const auto& output : value.outputs) {
        if (std::ranges::any_of(
                seen, [&](const digest& item) { return same_digest(item, output.blob); })) {
            continue;
        }
        seen.push_back(output.blob);
        auto bytes = co_await repository_detail::build_access::get(source, output.blob);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto blob = objects::decode_blob(*bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        if (std::numeric_limits<std::uint64_t>::max() - total < blob->size) {
            co_return std::unexpected(why_error(errc::internal, "output byte count overflow"));
        }
        total += blob->size;
    }
    co_return total;
}

auto update_cache_stats(const std::filesystem::path& cache_directory, const wrap_report& report)
    -> result<void> {
    const auto path = cache_directory / "stats";
    auto current = load_stats(path);
    if (!current) {
        return std::unexpected(current.error());
    }
    current->hits += report.hits;
    current->misses += report.misses;
    current->bytes_served += report.cache.bytes_served;
    current->bytes_rebuilt += report.cache.bytes_rebuilt;
    current->disk.bytes_avoided += report.cache.disk.bytes_avoided;
    const auto has_tier_counts = report.cache.disk.hits != 0 || report.cache.disk.misses != 0 ||
                                 report.cache.remote.hits != 0 || report.cache.remote.misses != 0;
    current->disk.hits += has_tier_counts ? report.cache.disk.hits : report.hits;
    current->disk.misses += has_tier_counts ? report.cache.disk.misses : report.misses;
    current->disk.bytes_transferred += report.cache.disk.bytes_transferred != 0
                                           ? report.cache.disk.bytes_transferred
                                           : report.cache.bytes_served;
    current->remote.available = current->remote.available || report.cache.remote.available;
    current->remote.hits += report.cache.remote.hits;
    current->remote.misses += report.cache.remote.misses;
    current->remote.bytes_transferred += report.cache.remote.bytes_transferred;
    current->remote.bytes_avoided += report.cache.remote.bytes_avoided;
    for (const auto& [reason, count] : report.cache.misses_by_reason) {
        current->misses_by_reason[reason] += count;
    }
    return save_stats(path, *current);
}

} // namespace liquid::build_detail

namespace liquid::build {

runtime::task<why_report> why(std::filesystem::path working_tree,
                              std::string target,
                              std::optional<std::filesystem::path> requested_cache) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(build_detail::why_error(
            errc::invalid_argument, "working tree is not a directory", working_tree));
    }
    auto cache_directory = build_detail::wrap_cache_directory(working_tree, requested_cache);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto report = why_report{.target = target};
    auto statistics = build_detail::load_stats(*cache_directory / "stats");
    if (!statistics) {
        co_return std::unexpected(statistics.error());
    }
    report.cache = std::move(*statistics);
    const auto repository_path = *cache_directory / "repository";
    if (!std::filesystem::exists(repository_path / "LIQUID_REPO")) {
        report.cause = {.reason = cache_miss_reason::never_built,
                        .detail = "the wrapped project has no cache repository"};
        co_return report;
    }
    auto source = co_await repository::open(repository_path);
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto parsed = std::optional<digest>{};
    if (target.contains(':')) {
        auto value = parse_digest(target);
        if (value) {
            parsed = *value;
        }
    }
    if (!parsed) {
        auto path = std::filesystem::path{target};
        if (path.is_absolute()) {
            path = path.lexically_relative(working_tree);
        }
        if (path.empty() || path.is_absolute() ||
            std::ranges::any_of(path, [](const auto& part) { return part == ".."; })) {
            co_return std::unexpected(build_detail::why_error(
                errc::invalid_argument, "target is outside the working tree", target));
        }
        target = path.lexically_normal().generic_string();
        report.target = target;
    }
    const auto action_directory = repository_path / ".liquid-store" / "actions";
    auto lookups = build_detail::read_lookups(action_directory);
    if (!lookups) {
        co_return std::unexpected(lookups.error());
    }
    auto selected = std::optional<build_detail::lookup_record>{};
    for (auto found = lookups->rbegin(); found != lookups->rend(); ++found) {
        auto matches = co_await build_detail::result_matches(*source, *found, target, parsed);
        if (matches && *matches) {
            selected = *found;
            break;
        }
    }
    if (!selected) {
        auto cache = build_detail::disk_cache{*source};
        auto cache_opened = co_await cache.open();
        if (!cache_opened) {
            co_return std::unexpected(cache_opened.error());
        }
        auto index = build_detail::action_index{action_directory};
        auto opened = co_await index.open();
        if (!opened) {
            co_return std::unexpected(opened.error());
        }
        auto entries = index.entries();
        std::ranges::sort(entries, std::greater{}, &objects::action_index_entry::sequence);
        auto current = std::optional<result_id>{};
        for (const auto& entry : entries) {
            auto lookup = build_detail::lookup_record{cache_status::miss, result_id{entry.result}};
            auto matches = co_await build_detail::result_matches(*source, lookup, target, parsed);
            if (!matches || !*matches) {
                continue;
            }
            if (!current) {
                current = result_id{entry.result};
                selected = lookup;
                continue;
            }
            if (!build_detail::same_digest(current->value, entry.result)) {
                selected->previous = result_id{entry.result};
                break;
            }
        }
    }
    if (!selected) {
        report.cause = {.reason = cache_miss_reason::never_built,
                        .detail = "no recorded result produced this target"};
        co_return report;
    }
    report.lookup = selected->status;
    report.result = selected->result;
    report.previous_result = selected->previous;
    auto cache = build_detail::disk_cache{*source};
    auto cache_opened = co_await cache.open();
    if (!cache_opened) {
        co_return std::unexpected(cache_opened.error());
    }
    const auto quarantined = [&]() -> const objects::action_index_entry* {
        const auto matches = [&](const result_id& id) {
            return std::ranges::find_if(cache.entries(), [&](const auto& entry) {
                return build_detail::same_digest(entry.result, id.value) && entry.divergent;
            });
        };
        const auto result = matches(selected->result);
        if (result != cache.entries().end()) {
            return &*result;
        }
        if (selected->previous) {
            const auto previous = matches(*selected->previous);
            if (previous != cache.entries().end()) {
                return &*previous;
            }
        }
        return nullptr;
    }();
    auto value = co_await build_detail::load_result(*source, selected->result.value);
    if (!value) {
        report.cause = {.reason = cache_miss_reason::unknown,
                        .detail = "the recorded result is unavailable"};
        co_return report;
    }
    report.action = value->action;
    report.schedule = value->schedule;
    if (selected->previous) {
        auto previous = co_await build_detail::load_result(*source, selected->previous->value);
        if (previous) {
            report.previous_action = previous->action;
        }
    }
    auto action = co_await build_detail::load_action(*source, value->action);
    if (!action) {
        report.cause = {.reason = cache_miss_reason::unknown,
                        .detail = "the recorded action is unavailable"};
        co_return report;
    }
    auto policy = co_await build_detail::enforce_policy(*source, action->input);
    if (!policy) {
        co_return std::unexpected(policy.error());
    }
    if (!policy->allowed) {
        const auto refused = std::ranges::find_if(policy->inputs, [](const auto& input) {
            return !input.satisfied && input.on_missing == policy_on_missing::refuse;
        });
        const auto refusal = build_detail::policy_refusal(*policy);
        report.cause = {.reason = cache_miss_reason::policy_refused,
                        .path = refused == policy->inputs.end() ? std::string{} : refused->input,
                        .detail = refusal.message + " (" + refusal.context + ")"};
        co_return report;
    }
    if (selected->status == cache_status::hit) {
        report.cause = {.reason = cache_miss_reason::unknown, .detail = "cache hit"};
        co_return report;
    }
    if (quarantined) {
        report.cause = {.reason = cache_miss_reason::divergent_result_quarantined,
                        .detail = build_detail::quarantine_detail(*quarantined)};
        co_return report;
    }
    auto cause = co_await build_detail::classify_miss(
        *source, action->input, action->sandbox, selected->previous, &*value);
    if (!cause) {
        co_return std::unexpected(cause.error());
    }
    report.cause = std::move(*cause);
    co_return report;
}

runtime::task<cache_report> cache_stats(std::filesystem::path working_tree,
                                        std::optional<std::filesystem::path> requested_cache) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(build_detail::why_error(
            errc::invalid_argument, "working tree is not a directory", working_tree));
    }
    auto cache_directory = build_detail::wrap_cache_directory(working_tree, requested_cache);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    co_return build_detail::load_stats(*cache_directory / "stats");
}

} // namespace liquid::build
