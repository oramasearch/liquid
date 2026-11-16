// SPDX-License-Identifier: MIT

#include "build/facts.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

namespace liquid::build_detail {
namespace {

constexpr auto fact_magic = std::array<char, 8>{'L', 'Q', 'F', 'A', 'C', 'T', 1, 0};

auto facts_error(errc code, std::string message, const std::filesystem::path& context = {})
    -> error {
    return {code, std::move(message), context.string()};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto same_key(const fact_key& left, const fact_key& right) -> bool {
    return same_digest(left.toolchain, right.toolchain) &&
           same_digest(left.sysroot, right.sysroot) && left.target == right.target;
}

auto append_u32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    for (auto index = 0U; index != 4U; ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

auto append_string(std::vector<std::byte>& output, std::string_view value) -> result<void> {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(facts_error(errc::invalid_argument, "fact text is too large"));
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    output.insert(output.end(), first, first + value.size());
    return {};
}

auto append_digest(std::vector<std::byte>& output, const digest& value) -> void {
    output.push_back(static_cast<std::byte>(value.alg));
    output.insert(output.end(), value.bytes.begin(), value.bytes.end());
}

auto read_u32(std::span<const std::byte> input, std::size_t& offset) -> result<std::uint32_t> {
    if (input.size() - std::min(input.size(), offset) < 4U) {
        return std::unexpected(facts_error(errc::corrupt, "truncated fact record"));
    }
    auto value = std::uint32_t{};
    for (auto index = 0U; index != 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(input[offset++]) << (index * 8U);
    }
    return value;
}

auto read_string(std::span<const std::byte> input, std::size_t& offset) -> result<std::string> {
    auto size = read_u32(input, offset);
    if (!size || *size > input.size() - std::min(input.size(), offset)) {
        return std::unexpected(facts_error(errc::corrupt, "truncated fact text"));
    }
    const auto* first = reinterpret_cast<const char*>(input.data() + offset);
    auto output = std::string{first, *size};
    offset += *size;
    return output;
}

auto read_digest(std::span<const std::byte> input, std::size_t& offset) -> result<digest> {
    if (input.size() - std::min(input.size(), offset) < 33U) {
        return std::unexpected(facts_error(errc::corrupt, "truncated fact digest"));
    }
    const auto tag = std::to_integer<std::uint8_t>(input[offset++]);
    if (tag < static_cast<std::uint8_t>(algorithm::blake3) ||
        tag > static_cast<std::uint8_t>(algorithm::blake3_keyed)) {
        return std::unexpected(facts_error(errc::corrupt, "invalid fact digest algorithm"));
    }
    auto output = digest{.alg = static_cast<algorithm>(tag)};
    std::ranges::copy(input.subspan(offset, output.bytes.size()), output.bytes.begin());
    offset += output.bytes.size();
    return output;
}

auto encode_fact(const configure_fact& value) -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    for (const auto byte : fact_magic) {
        output.push_back(static_cast<std::byte>(byte));
    }
    auto probe = append_string(output, value.probe);
    if (!probe) {
        return std::unexpected(probe.error());
    }
    append_digest(output, value.key.toolchain);
    append_digest(output, value.key.sysroot);
    auto target = append_string(output, value.key.target);
    if (!target) {
        return std::unexpected(target.error());
    }
    append_u32(output, static_cast<std::uint32_t>(value.answer.exit));
    append_u32(output, static_cast<std::uint32_t>(value.answer.outputs.size()));
    for (const auto& item : value.answer.outputs) {
        auto path = append_string(output, item.path);
        if (!path) {
            return std::unexpected(path.error());
        }
        append_u32(output, item.mode);
        append_digest(output, item.blob);
    }
    append_u32(output, static_cast<std::uint32_t>(value.answer.variables.size()));
    for (const auto& [name, answer] : value.answer.variables) {
        auto encoded_name = append_string(output, name);
        auto encoded_answer = append_string(output, answer);
        if (!encoded_name || !encoded_answer) {
            return std::unexpected(!encoded_name ? encoded_name.error() : encoded_answer.error());
        }
    }
    output.push_back(value.answer.result ? std::byte{1} : std::byte{0});
    if (value.answer.result) {
        append_digest(output, *value.answer.result);
    }
    return output;
}

auto decode_fact(std::span<const std::byte> input, digest id) -> result<configure_fact> {
    if (input.size() < fact_magic.size() ||
        !std::ranges::equal(input.first(fact_magic.size()), fact_magic, {}, [](std::byte value) {
            return static_cast<char>(value);
        })) {
        return std::unexpected(facts_error(errc::corrupt, "invalid fact record header"));
    }
    auto offset = fact_magic.size();
    auto probe = read_string(input, offset);
    auto toolchain = read_digest(input, offset);
    auto sysroot = read_digest(input, offset);
    auto target = read_string(input, offset);
    auto exit = read_u32(input, offset);
    auto output_count = read_u32(input, offset);
    if (!probe || !toolchain || !sysroot || !target || !exit || !output_count) {
        return std::unexpected(facts_error(errc::corrupt, "invalid fact record"));
    }
    auto output =
        configure_fact{.id = id,
                       .probe = std::move(*probe),
                       .key = {*toolchain, *sysroot, std::move(*target)},
                       .answer = {.exit = static_cast<int>(static_cast<std::int32_t>(*exit))}};
    for (auto index = 0U; index != *output_count; ++index) {
        auto path = read_string(input, offset);
        auto mode = read_u32(input, offset);
        auto blob = read_digest(input, offset);
        if (!path || !mode || !blob) {
            return std::unexpected(facts_error(errc::corrupt, "invalid fact output"));
        }
        output.answer.outputs.push_back({std::move(*path), *mode, *blob});
    }
    auto variable_count = read_u32(input, offset);
    if (!variable_count) {
        return std::unexpected(variable_count.error());
    }
    for (auto index = 0U; index != *variable_count; ++index) {
        auto name = read_string(input, offset);
        auto answer = read_string(input, offset);
        if (!name || !answer) {
            return std::unexpected(facts_error(errc::corrupt, "invalid fact variable"));
        }
        output.answer.variables.insert_or_assign(std::move(*name), std::move(*answer));
    }
    if (offset >= input.size()) {
        return std::unexpected(facts_error(errc::corrupt, "missing fact result flag"));
    }
    const auto has_result = std::to_integer<std::uint8_t>(input[offset++]);
    if (has_result > 1U) {
        return std::unexpected(facts_error(errc::corrupt, "invalid fact result flag"));
    }
    if (has_result == 1U) {
        auto result_id = read_digest(input, offset);
        if (!result_id) {
            return std::unexpected(result_id.error());
        }
        output.answer.result = *result_id;
    }
    if (offset != input.size()) {
        return std::unexpected(facts_error(errc::corrupt, "trailing fact record bytes"));
    }
    return output;
}

auto record_name(digest id) -> std::string {
    auto output = to_string(id);
    std::ranges::replace(output, ':', '-');
    return output + ".fact";
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(facts_error(errc::io, "cannot read fact record", path));
    }
    auto characters =
        std::vector<char>{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        return std::unexpected(facts_error(errc::io, "cannot read fact record", path));
    }
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(static_cast<std::byte>(character));
    }
    return output;
}

auto write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected(facts_error(errc::io, "cannot write fact record", path));
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return std::unexpected(facts_error(errc::io, "cannot write fact record", path));
    }
    return {};
}

auto json_escape(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr auto digits = std::string_view{"0123456789abcdef"};
                const auto value = static_cast<unsigned char>(character);
                output += "\\u00";
                output.push_back(digits[value >> 4U]);
                output.push_back(digits[value & 0x0fU]);
            } else {
                output.push_back(character);
            }
            break;
        }
    }
    return output;
}

auto trim(std::string value) -> std::string {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2U && ((value.front() == '\'' && value.back() == '\'') ||
                               (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2U);
    }
    return value;
}

auto hash_file(const std::filesystem::path& path) -> result<digest> {
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return hash(algorithm::blake3, *bytes);
}

auto hash_tree(const std::filesystem::path& root) -> result<digest> {
    auto material = std::vector<std::byte>{};
    if (root.empty()) {
        return hash(algorithm::blake3, material);
    }
    auto paths = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (iterator->is_regular_file(code)) {
            paths.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(facts_error(errc::io, code.message(), root));
    }
    std::ranges::sort(paths);
    for (const auto& path : paths) {
        auto file = hash_file(path);
        if (!file) {
            return std::unexpected(file.error());
        }
        const auto relative = path.lexically_relative(root).generic_string();
        const auto* first = reinterpret_cast<const std::byte*>(relative.data());
        material.insert(material.end(), first, first + relative.size());
        material.push_back(std::byte{0});
        append_digest(material, *file);
    }
    return hash(algorithm::blake3, material);
}

auto executable_on_path(std::string name) -> result<std::filesystem::path> {
    if (const auto space = name.find_first_of(" \t"); space != std::string::npos) {
        name.resize(space);
    }
    auto candidate = std::filesystem::path{name};
    if (candidate.is_absolute() || candidate.has_parent_path()) {
        auto code = std::error_code{};
        candidate = std::filesystem::canonical(candidate, code);
        if (!code && std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
        return std::unexpected(facts_error(errc::not_found, "configured compiler is absent", name));
    }
    const auto* path = std::getenv("PATH");
    auto paths = std::stringstream{path ? path : ""};
    auto directory = std::string{};
    while (std::getline(paths, directory, ':')) {
        auto found = std::filesystem::path{directory} / candidate;
        auto code = std::error_code{};
        if (std::filesystem::is_regular_file(found, code)) {
            return std::filesystem::canonical(found, code);
        }
    }
    return std::unexpected(facts_error(errc::not_found, "configured compiler is absent", name));
}

auto configured_sysroot() -> std::filesystem::path {
    if (const auto* value = std::getenv("SYSROOT")) {
        return value;
    }
    if (const auto* value = std::getenv("SDKROOT")) {
        return value;
    }
    if (const auto* compiler = std::getenv("CC")) {
        const auto expression = std::regex{R"((?:^|\s)--sysroot(?:=|\s+)([^\s]+))"};
        auto match = std::cmatch{};
        if (std::regex_search(compiler, match, expression)) {
            return match[1].str();
        }
    }
    return {};
}

auto normalize_probe_text(std::string value) -> std::string {
    value = std::regex_replace(value, std::regex{R"(TryCompile-[A-Za-z0-9_-]+)"}, "TryCompile-$ID");
    value = std::regex_replace(value, std::regex{R"(cmTC_[A-Fa-f0-9]+)"}, "cmTC_$ID");
    value = std::regex_replace(
        value, std::regex{R"((?:[^\s]*/)?conftest\.(?:c|cc|cpp|cxx|o|obj|exe))"}, "$CONFTEST");
    value = std::regex_replace(value, std::regex{R"(/liquid/work(?:/[^\s]*)?)"}, "$WORK");
    return value;
}

auto configuration_files(const std::filesystem::path& root, std::string_view name)
    -> std::vector<std::filesystem::path> {
    auto output = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (iterator->is_regular_file(code) && iterator->path().filename() == name) {
            output.push_back(iterator->path());
        }
    }
    return output;
}

auto autoconf_probe_needle(std::string_view variable) -> std::optional<std::string> {
    constexpr auto header_prefix = std::string_view{"ac_cv_header_"};
    if (variable.starts_with(header_prefix)) {
        auto header = std::string{variable.substr(header_prefix.size())};
        if (header.ends_with("_h")) {
            header.resize(header.size() - 2U);
            std::ranges::replace(header, '_', '/');
            header += ".h";
            return "#include <" + header + ">";
        }
    }
    constexpr auto function_prefix = std::string_view{"ac_cv_func_"};
    if (variable.starts_with(function_prefix)) {
        return std::string{variable.substr(function_prefix.size())} + " (";
    }
    return std::nullopt;
}

auto record_variables(const std::filesystem::path& path,
                      std::string_view prefix,
                      const fact_key& key,
                      fact_base& base,
                      bool cmake) -> result<void> {
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(facts_error(errc::io, "cannot read configuration answers", path));
    }
    auto line = std::string{};
    while (std::getline(input, line)) {
        auto name = std::string{};
        auto answer = std::string{};
        if (cmake) {
            const auto colon = line.find(':');
            const auto equals = line.find('=');
            if (colon == std::string::npos || equals == std::string::npos || colon > equals) {
                continue;
            }
            name = line.substr(0, colon);
            const auto type = line.substr(colon + 1, equals - colon - 1);
            if (type != "INTERNAL" ||
                !(name.starts_with("HAVE_") || name.starts_with("HAS_") ||
                  name.starts_with("CMAKE_HAVE_") || name.contains("_HAVE_"))) {
                continue;
            }
            answer = trim(line.substr(equals + 1));
        } else {
            const auto equals = line.find('=');
            if (equals == std::string::npos) {
                continue;
            }
            name = line.substr(0, equals);
            if (!name.starts_with("ac_cv_")) {
                continue;
            }
            answer = trim(line.substr(equals + 1));
        }
        auto fact_answer_value = fact_answer{.exit = answer == "no" || answer.empty() ? 1 : 0};
        fact_answer_value.variables.insert_or_assign(name, answer);
        auto associated = false;
        if (!cmake) {
            const auto needle = autoconf_probe_needle(name);
            for (const auto& probe : base.matching(key)) {
                if (!needle || !probe.probe.starts_with("probe-v1\n") ||
                    !probe.probe.contains(*needle)) {
                    continue;
                }
                auto combined = probe.answer;
                combined.exit = fact_answer_value.exit;
                combined.variables.insert_or_assign(name, answer);
                auto stored = base.put(probe.probe, key, std::move(combined));
                if (!stored) {
                    return std::unexpected(stored.error());
                }
                associated = true;
            }
        }
        if (associated) {
            continue;
        }
        auto stored = base.put(std::string{prefix} + "\n" + name, key, fact_answer_value);
        if (!stored) {
            return std::unexpected(stored.error());
        }
    }
    return {};
}

} // namespace

fact_base::fact_base(std::filesystem::path directory) : directory_(std::move(directory)) {}

result<void> fact_base::open() {
    auto code = std::error_code{};
    const auto records = directory_ / "records";
    std::filesystem::create_directories(records, code);
    if (code) {
        return std::unexpected(facts_error(errc::io, code.message(), records));
    }
    facts_.clear();
    for (const auto& entry : std::filesystem::directory_iterator{records, code}) {
        if (code) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".fact") {
            continue;
        }
        auto bytes = read_file(entry.path());
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto id = hash(algorithm::blake3, *bytes);
        if (!id || record_name(*id) != entry.path().filename()) {
            return std::unexpected(
                facts_error(errc::corrupt, "fact filename does not match content", entry.path()));
        }
        auto decoded = decode_fact(*bytes, *id);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        facts_.push_back(std::move(*decoded));
    }
    if (code) {
        return std::unexpected(facts_error(errc::io, code.message(), records));
    }
    std::ranges::sort(facts_, {}, [](const configure_fact& value) { return to_string(value.id); });
    return write_index();
}

result<configure_fact> fact_base::put(std::string probe, fact_key key, fact_answer answer) {
    auto value = configure_fact{
        .probe = std::move(probe), .key = std::move(key), .answer = std::move(answer)};
    auto encoded = encode_fact(value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    auto id = hash(algorithm::blake3, *encoded);
    if (!id) {
        return std::unexpected(id.error());
    }
    value.id = *id;
    const auto found = std::ranges::find_if(
        facts_, [&](const configure_fact& item) { return same_digest(item.id, value.id); });
    if (found != facts_.end()) {
        return *found;
    }
    const auto path = directory_ / "records" / record_name(*id);
    auto written = write_file(path, *encoded);
    if (!written) {
        return std::unexpected(written.error());
    }
    facts_.push_back(value);
    std::ranges::sort(facts_, {}, [](const configure_fact& item) { return to_string(item.id); });
    auto indexed = write_index();
    if (!indexed) {
        return std::unexpected(indexed.error());
    }
    return value;
}

const std::vector<configure_fact>& fact_base::facts() const noexcept {
    return facts_;
}

result<configure_fact> fact_base::show(digest id) const {
    const auto found = std::ranges::find_if(
        facts_, [&](const configure_fact& value) { return same_digest(value.id, id); });
    if (found == facts_.end()) {
        return std::unexpected(
            facts_error(errc::not_found, "configure fact is absent", to_string(id)));
    }
    return *found;
}

std::vector<configure_fact> fact_base::matching(const fact_key& key) const {
    auto output = std::vector<configure_fact>{};
    std::ranges::copy_if(facts_, std::back_inserter(output), [&](const configure_fact& value) {
        return same_key(value.key, key);
    });
    return output;
}

result<void> fact_base::write_index() const {
    const auto temporary = directory_ / "INDEX.tmp";
    const auto target = directory_ / "INDEX";
    auto output = std::ofstream{temporary, std::ios::trunc};
    if (!output) {
        return std::unexpected(facts_error(errc::io, "cannot write fact index", temporary));
    }
    output << "LQFACT-INDEX\t1\n";
    for (const auto& fact : facts_) {
        auto probe_bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(fact.probe.data()), fact.probe.size()};
        auto probe = hash(algorithm::blake3, probe_bytes);
        if (!probe) {
            return std::unexpected(probe.error());
        }
        output << to_string(fact.key.toolchain) << '\t' << to_string(fact.key.sysroot) << '\t'
               << fact.key.target << '\t' << to_string(*probe) << '\t' << to_string(fact.id)
               << '\n';
    }
    output.close();
    if (!output) {
        return std::unexpected(facts_error(errc::io, "cannot write fact index", temporary));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, target, code);
    if (code) {
        std::filesystem::remove(target, code);
        code.clear();
        std::filesystem::rename(temporary, target, code);
    }
    if (code) {
        return std::unexpected(facts_error(errc::io, code.message(), target));
    }
    return {};
}

result<std::string> fact_base::export_bundle() const {
    auto body = std::ostringstream{};
    body << "{\"v\":1,\"kind\":\"fact-bundle/1\",\"facts\":[";
    auto first_fact = true;
    for (const auto& fact : facts_) {
        if (!first_fact) {
            body << ',';
        }
        first_fact = false;
        body << "{\"id\":\"" << to_string(fact.id) << "\",\"probe\":\"" << json_escape(fact.probe)
             << "\",\"key\":{\"toolchain\":\"" << to_string(fact.key.toolchain)
             << "\",\"sysroot\":\"" << to_string(fact.key.sysroot) << "\",\"target\":\""
             << json_escape(fact.key.target) << "\"},\"answer\":{\"exit\":" << fact.answer.exit
             << ",\"variables\":{";
        auto first_variable = true;
        for (const auto& [name, answer] : fact.answer.variables) {
            if (!first_variable) {
                body << ',';
            }
            first_variable = false;
            body << '"' << json_escape(name) << "\":\"" << json_escape(answer) << '"';
        }
        body << "},\"outputs\":[";
        auto first_output = true;
        for (const auto& output : fact.answer.outputs) {
            if (!first_output) {
                body << ',';
            }
            first_output = false;
            body << "{\"path\":\"" << json_escape(output.path) << "\",\"mode\":" << output.mode
                 << ",\"blob\":\"" << to_string(output.blob) << "\"}";
        }
        body << ']';
        if (fact.answer.result) {
            body << ",\"result\":\"" << to_string(*fact.answer.result) << '"';
        }
        body << "}}";
    }
    body << "]}";
    const auto content = body.str();
    auto id = hash(algorithm::blake3,
                   {reinterpret_cast<const std::byte*>(content.data()), content.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    auto output = content;
    output.insert(output.size() - 1U, ",\"bundle\":\"" + to_string(*id) + "\"");
    return output;
}

result<std::filesystem::path> default_fact_directory(const std::filesystem::path& working_tree) {
    if (const auto* configured = std::getenv("LIQUID_FACTS_CACHE")) {
        return std::filesystem::path{configured};
    }
    if (const auto* configured = std::getenv("LIQUID_WRAP_CACHE")) {
        return std::filesystem::path{configured} / "facts";
    }
    if (const auto* configured = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path{configured} / "liquid/facts";
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".cache/liquid/facts";
    }
    if (working_tree.empty()) {
        return std::unexpected(facts_error(errc::not_found, "cannot locate configure fact base"));
    }
    return working_tree / ".liquid-facts";
}

std::string host_target() {
#if defined(__x86_64__) || defined(_M_X64)
    constexpr auto architecture = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    constexpr auto architecture = "aarch64";
#else
    constexpr auto architecture = "unknown";
#endif
#if defined(__linux__)
    return std::string{architecture} + "-unknown-linux-gnu";
#elif defined(__APPLE__)
    return std::string{architecture} + "-apple-darwin";
#elif defined(_WIN32)
    return std::string{architecture} + "-pc-windows-msvc";
#else
    return std::string{architecture} + "-unknown-unknown";
#endif
}

result<fact_key> detect_fact_key(std::optional<std::string> target) {
    if (target && (target->empty() || !std::ranges::all_of(*target, [](unsigned char character) {
                       return std::isalnum(character) || character == '-' || character == '_' ||
                              character == '.';
                   }))) {
        return std::unexpected(
            facts_error(errc::invalid_argument, "target triple contains invalid characters"));
    }
    const auto compiler_name = std::getenv("CC") ? std::string{std::getenv("CC")} : "cc";
    auto compiler = executable_on_path(compiler_name);
    if (!compiler) {
        compiler = executable_on_path("clang");
    }
    if (!compiler) {
        return std::unexpected(compiler.error());
    }
    auto toolchain = hash_file(*compiler);
    if (!toolchain) {
        return std::unexpected(toolchain.error());
    }
    const auto sysroot_path = configured_sysroot();
    auto sysroot = hash_tree(sysroot_path);
    if (!sysroot) {
        return std::unexpected(sysroot.error());
    }
    return fact_key{*toolchain, *sysroot, target ? std::move(*target) : host_target()};
}

bool foreign_target(const fact_key& key) {
    return key.target != host_target();
}

bool probe_process(std::span<const std::string> command, std::string_view cwd) {
    if (cwd.contains("CMakeScratch/TryCompile-") || cwd.contains("CMakeTmp")) {
        return true;
    }
    return std::ranges::any_of(command, [](const std::string& argument) {
        return argument.contains("conftest.c") || argument.contains("conftest.cc") ||
               argument.contains("conftest.cpp") || argument.contains("CMakeScratch/TryCompile-") ||
               argument.contains("CMakeTmp") || argument.contains("cmTC_");
    });
}

std::string canonical_probe(std::span<const std::string> command,
                            std::string_view cwd,
                            std::string_view test_program) {
    auto output =
        std::string{"probe-v1\ncwd="} + normalize_probe_text(std::string{cwd}) + "\nargv=";
    for (const auto& argument : command) {
        output += normalize_probe_text(argument);
        output.push_back('\0');
    }
    output += "\nprogram=";
    output += normalize_probe_text(std::string{test_program});
    return output;
}

std::map<std::string, std::string> cached_variables(const fact_base& base, const fact_key& key) {
    auto output = std::map<std::string, std::string>{};
    for (const auto& fact : base.matching(key)) {
        output.insert(fact.answer.variables.begin(), fact.answer.variables.end());
    }
    return output;
}

result<void> record_configuration_facts(const std::filesystem::path& working_tree,
                                        const fact_key& key,
                                        fact_base& base) {
    for (const auto& path : configuration_files(working_tree, "config.log")) {
        auto recorded = record_variables(path, "autoconf-cache-v1", key, base, false);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
    }
    for (const auto& path : configuration_files(working_tree, "CMakeCache.txt")) {
        auto recorded = record_variables(path, "cmake-cache-v1", key, base, true);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
    }
    return {};
}

} // namespace liquid::build_detail

namespace liquid::build {

runtime::task<std::vector<configure_fact>>
facts_list(std::optional<std::filesystem::path> directory) {
    auto selected = directory
                        ? liquid::result<std::filesystem::path>{*directory}
                        : build_detail::default_fact_directory(std::filesystem::current_path());
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto base = build_detail::fact_base{*selected};
    auto opened = base.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return base.facts();
}

runtime::task<configure_fact> facts_show(digest id,
                                         std::optional<std::filesystem::path> directory) {
    auto selected = directory
                        ? liquid::result<std::filesystem::path>{*directory}
                        : build_detail::default_fact_directory(std::filesystem::current_path());
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto base = build_detail::fact_base{*selected};
    auto opened = base.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return base.show(id);
}

runtime::task<std::string> facts_export(std::optional<std::filesystem::path> directory) {
    auto selected = directory
                        ? liquid::result<std::filesystem::path>{*directory}
                        : build_detail::default_fact_directory(std::filesystem::current_path());
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto base = build_detail::fact_base{*selected};
    auto opened = base.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return base.export_bundle();
}

} // namespace liquid::build
