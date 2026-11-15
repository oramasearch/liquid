#include "liquid/lake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/version.hpp"
#include "net/http_client.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;
using nlohmann::json;

constexpr auto repository_lake_key = "lake.v1";
constexpr auto sync_protocols = std::to_array<std::uint32_t>({1});
constexpr auto forge_apis = std::to_array<std::uint32_t>({1});

auto lake_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto home_directory() -> std::filesystem::path {
    if (const auto* configured = std::getenv("LIQUID_HOME")) {
        return configured;
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".liquid";
    }
    return std::filesystem::current_path() / ".liquid";
}

auto is_url(std::string_view value) -> bool {
    return value.starts_with("https://") || value.starts_with("http://");
}

auto valid_name(std::string_view name) -> bool {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!std::islower(first) && !std::isdigit(first)) {
        return false;
    }
    return std::ranges::all_of(name, [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::islower(value) || std::isdigit(value) || character == '-' || character == '_';
    });
}

// The key a lake's token is stored under: origin with an explicit port, plus the path prefix.
auto credential_key(const net_detail::parsed_url& url) -> std::string {
    return url.path == "/" ? url.origin : url.origin + url.path;
}

auto parse_lake_url(std::string_view url) -> result<net_detail::parsed_url> {
    if (url.find_first_of("?# \t\r\n") != std::string_view::npos) {
        return std::unexpected(
            lake_error(errc::invalid_argument,
                       "a lake URL is scheme, host, optional port and optional path only",
                       std::string{url}));
    }
    return net_detail::parse_url(url, "lake URL");
}

auto normalized_url(std::string url) -> std::string {
    while (url.ends_with('/') && url.find("://") + 3 < url.size()) {
        url.pop_back();
    }
    return url;
}

auto environment_name(std::string_view name) -> std::string {
    auto output = std::string{"LIQUID_TOKEN_"};
    for (const auto character : name) {
        output += character == '-'
                      ? '_'
                      : static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return output;
}

auto read_registry(const std::filesystem::path& path) -> result<std::vector<lake>> {
    auto code = std::error_code{};
    if (!std::filesystem::exists(path, code)) {
        return std::vector<lake>{};
    }
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(lake_error(errc::io, "cannot read the lake list", path.string()));
    }
    const auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    try {
        const auto document = json::parse(text);
        if (document.value("v", 0) != 1 || !document.contains("lakes") ||
            !document.at("lakes").is_array()) {
            return std::unexpected(
                lake_error(errc::corrupt, "the lake list has an unknown format", path.string()));
        }
        auto output = std::vector<lake>{};
        for (const auto& item : document.at("lakes")) {
            auto entry =
                lake{item.at("name").get<std::string>(), item.at("url").get<std::string>()};
            if (!valid_name(entry.name) || !parse_lake_url(entry.url)) {
                return std::unexpected(
                    lake_error(errc::corrupt, "the lake list has an invalid entry", path.string()));
            }
            output.push_back(std::move(entry));
        }
        return output;
    } catch (const json::exception& exception) {
        return std::unexpected(lake_error(errc::corrupt, exception.what(), path.string()));
    }
}

auto write_registry(const std::filesystem::path& path, std::span<const lake> lakes)
    -> result<void> {
    auto entries = json::array();
    for (const auto& entry : lakes) {
        entries.push_back({{"name", entry.name}, {"url", entry.url}});
    }
    const auto text = json{{"v", 1}, {"lakes", std::move(entries)}}.dump(2) + "\n";
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(lake_error(errc::io, code.message(), path.parent_path().string()));
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        output << text;
        if (!output) {
            return std::unexpected(
                lake_error(errc::io, "cannot write the lake list", temporary.string()));
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        return std::unexpected(lake_error(errc::io, code.message(), path.string()));
    }
    return {};
}

class memory_store final : public lake_credential_store {
public:
    auto read(std::string_view origin) -> result<std::optional<std::string>> override {
        auto lock = std::scoped_lock{mutex_};
        const auto found = values_.find(std::string{origin});
        if (found == values_.end()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{found->second};
    }

    auto write(std::string_view origin, std::string_view token) -> result<void> override {
        auto lock = std::scoped_lock{mutex_};
        values_[std::string{origin}] = std::string{token};
        return {};
    }

    auto erase(std::string_view origin) -> result<void> override {
        auto lock = std::scoped_lock{mutex_};
        values_.erase(std::string{origin});
        return {};
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string, std::less<>> values_;
};

struct parsed_version {
    unsigned major{};
    unsigned minor{};
    unsigned patch{};

    auto operator<=>(const parsed_version&) const = default;
};

auto parse_version(std::string_view text) -> std::optional<parsed_version> {
    if (text.starts_with('v')) {
        text.remove_prefix(1);
    }
    auto output = parsed_version{};
    auto parts = std::array{&output.major, &output.minor, &output.patch};
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto [end, failure] =
            std::from_chars(text.data(), text.data() + text.size(), *parts[index]);
        if (failure != std::errc{}) {
            return std::nullopt;
        }
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (index + 1 < parts.size()) {
            if (!text.starts_with('.')) {
                return std::nullopt;
            }
            text.remove_prefix(1);
        }
    }
    if (!text.empty() && !text.starts_with('-') && !text.starts_with('+')) {
        return std::nullopt;
    }
    return output;
}

auto versions_text(std::span<const std::uint32_t> values) -> std::string {
    auto output = std::string{};
    for (const auto value : values) {
        output += output.empty() ? "" : ", ";
        output += std::to_string(value);
    }
    return output;
}

auto lake_version_text(const lake_server_info& info) -> std::string {
    if (!info.version.empty()) {
        return info.revision.empty() ? "version " + info.version
                                     : "version " + info.version + ", revision " + info.revision;
    }
    if (!info.revision.empty()) {
        return "revision " + info.revision;
    }
    return "version unknown";
}

auto unsigned_array(const json& document, std::string_view key) -> std::vector<std::uint32_t> {
    auto output = std::vector<std::uint32_t>{};
    const auto found = document.find(key);
    if (found == document.end() || !found->is_array()) {
        return output;
    }
    for (const auto& item : *found) {
        if (item.is_number_unsigned() && item.get<std::uint64_t>() <= 0xffffffffULL) {
            output.push_back(static_cast<std::uint32_t>(item.get<std::uint64_t>()));
        }
    }
    return output;
}

auto string_field(const json& document, std::string_view key) -> std::string {
    const auto found = document.find(key);
    return found != document.end() && found->is_string() ? found->get<std::string>()
                                                         : std::string{};
}

// A URL with no named lake: the part before `/v1/`, or the bare origin.
auto unnamed_base(std::string_view url) -> std::string {
    const auto scheme = url.find("://");
    const auto path = url.find('/', scheme == std::string_view::npos ? 0 : scheme + 3);
    if (path == std::string_view::npos) {
        return normalized_url(std::string{url});
    }
    const auto api = url.find("/v1/", path);
    const auto end = api == std::string_view::npos ? path : api;
    return normalized_url(std::string{url.substr(0, end)});
}

auto shared_token() -> lake_token {
    if (const auto* value = std::getenv("LIQUID_TOKEN"); value && *value) {
        return {value, lake_token_source::shared_environment};
    }
    return {};
}

std::mutex checked_mutex;
std::map<std::string, lake_compatibility, std::less<>> checked_lakes;

} // namespace

lake_credential_store::~lake_credential_store() = default;

auto memory_lake_credential_store() -> std::shared_ptr<lake_credential_store> {
    return std::make_shared<memory_store>();
}

struct lake_registry::impl {
    std::filesystem::path path;
    std::shared_ptr<lake_credential_store> credentials;
    std::vector<lake> lakes;
};

lake_registry::lake_registry() : impl_(std::make_unique<impl>()) {}
lake_registry::~lake_registry() = default;
lake_registry::lake_registry(lake_registry&&) noexcept = default;
lake_registry& lake_registry::operator=(lake_registry&&) noexcept = default;

runtime::task<lake_registry> lake_registry::open(lake_registry_options options) {
    auto output = lake_registry{};
    output.impl_->path =
        options.path.empty() ? home_directory() / "lakes.json" : std::move(options.path);
    output.impl_->credentials =
        options.credentials ? std::move(options.credentials) : os_lake_credential_store();
    auto loaded =
        co_await runtime::offload([path = output.impl_->path] { return read_registry(path); });
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    if (!*loaded) {
        co_return std::unexpected(loaded->error());
    }
    output.impl_->lakes = std::move(**loaded);
    co_return output;
}

auto lake_registry::list() const -> std::span<const lake> {
    return impl_->lakes;
}

auto lake_registry::find(std::string_view name) const -> std::optional<lake> {
    const auto found = std::ranges::find(impl_->lakes, name, &lake::name);
    return found == impl_->lakes.end() ? std::nullopt : std::optional<lake>{*found};
}

auto lake_registry::match(std::string_view url) const -> std::optional<lake> {
    auto candidate = net_detail::parse_url(url, "URL");
    if (!candidate) {
        return std::nullopt;
    }
    auto best = std::optional<lake>{};
    auto best_length = std::size_t{};
    for (const auto& entry : impl_->lakes) {
        auto parsed = net_detail::parse_url(entry.url, "lake URL");
        if (!parsed || parsed->origin != candidate->origin) {
            continue;
        }
        const auto& prefix = parsed->path;
        const auto inside =
            prefix == "/" || candidate->path == prefix ||
            (candidate->path.starts_with(prefix) && candidate->path[prefix.size()] == '/');
        if (inside && (!best || prefix.size() > best_length)) {
            best = entry;
            best_length = prefix.size();
        }
    }
    return best;
}

runtime::task<lake> lake_registry::add(std::string name, std::string url) {
    if (!valid_name(name)) {
        co_return std::unexpected(lake_error(
            errc::invalid_argument,
            "a lake name is 1 to 64 characters of a-z, 0-9, '-' and '_', starting with a letter "
            "or digit",
            name));
    }
    url = normalized_url(std::move(url));
    auto parsed = parse_lake_url(url);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    if (find(name)) {
        co_return std::unexpected(
            lake_error(errc::already_exists, "a lake with this name already exists", name));
    }
    for (const auto& entry : impl_->lakes) {
        auto existing = net_detail::parse_url(entry.url, "lake URL");
        if (existing && credential_key(*existing) == credential_key(*parsed)) {
            co_return std::unexpected(lake_error(
                errc::already_exists, "this URL is already the lake '" + entry.name + "'", url));
        }
    }
    auto lakes = impl_->lakes;
    lakes.push_back(lake{name, url});
    auto saved = co_await runtime::offload(
        [path = impl_->path, lakes] { return write_registry(path, lakes); });
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    if (!*saved) {
        co_return std::unexpected(saved->error());
    }
    impl_->lakes = std::move(lakes);
    co_return impl_->lakes.back();
}

runtime::task<void> lake_registry::remove(std::string name) {
    auto entry = find(name);
    if (!entry) {
        co_return std::unexpected(lake_error(errc::not_found, "no lake has this name", name));
    }
    auto parsed = net_detail::parse_url(entry->url, "lake URL");
    if (parsed) {
        auto erased = co_await runtime::offload(
            [credentials = impl_->credentials, key = credential_key(*parsed)] {
                return credentials->erase(key);
            });
        if (erased && !*erased && erased->error().code != errc::unavailable) {
            co_return std::unexpected(erased->error());
        }
    }
    auto lakes = impl_->lakes;
    std::erase_if(lakes, [&](const lake& item) { return item.name == name; });
    auto saved = co_await runtime::offload(
        [path = impl_->path, lakes] { return write_registry(path, lakes); });
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    if (!*saved) {
        co_return std::unexpected(saved->error());
    }
    impl_->lakes = std::move(lakes);
    co_return result<void>{};
}

runtime::task<void> lake_registry::set_token(std::string name, std::string token) {
    auto entry = find(name);
    if (!entry) {
        co_return std::unexpected(lake_error(errc::not_found, "no lake has this name", name));
    }
    if (token.empty()) {
        co_return std::unexpected(lake_error(errc::invalid_argument, "the token is empty", name));
    }
    auto parsed = net_detail::parse_url(entry->url, "lake URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    auto written = co_await runtime::offload(
        [credentials = impl_->credentials,
         key = credential_key(*parsed),
         token = std::move(token)] { return credentials->write(key, token); });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        auto failure = written->error();
        if (failure.code == errc::unavailable) {
            failure.message += "; set " + environment_name(name) + " instead";
        }
        co_return std::unexpected(std::move(failure));
    }
    co_return result<void>{};
}

runtime::task<void> lake_registry::clear_token(std::string name) {
    auto entry = find(name);
    if (!entry) {
        co_return std::unexpected(lake_error(errc::not_found, "no lake has this name", name));
    }
    auto parsed = net_detail::parse_url(entry->url, "lake URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    auto erased = co_await runtime::offload(
        [credentials = impl_->credentials, key = credential_key(*parsed)] {
            return credentials->erase(key);
        });
    if (!erased) {
        co_return std::unexpected(erased.error());
    }
    if (!*erased && erased->error().code != errc::unavailable) {
        co_return std::unexpected(erased->error());
    }
    co_return result<void>{};
}

runtime::task<lake_token> lake_registry::token(std::string name) const {
    auto entry = find(name);
    if (!entry) {
        co_return std::unexpected(lake_error(errc::not_found, "no lake has this name", name));
    }
    auto parsed = net_detail::parse_url(entry->url, "lake URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    auto stored = co_await runtime::offload(
        [credentials = impl_->credentials, key = credential_key(*parsed)] {
            return credentials->read(key);
        });
    if (stored && *stored && **stored && !(***stored).empty()) {
        co_return lake_token{std::move(***stored), lake_token_source::credential_store};
    }
    const auto variable = environment_name(name);
    if (const auto* value = std::getenv(variable.c_str()); value && *value) {
        co_return lake_token{value, lake_token_source::lake_environment};
    }
    co_return shared_token();
}

runtime::task<std::optional<repository_lake>> read_repository_lake(repository& subject) {
    auto stored = co_await subject.metadata(repository_lake_key);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!*stored) {
        co_return std::optional<repository_lake>{};
    }
    auto decoded = cbor::decode(**stored);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    const auto* fields = std::get_if<cbor::value::map>(&decoded->data);
    auto output = repository_lake{};
    auto version = std::uint64_t{};
    for (const auto& [key, value] : fields ? *fields : cbor::value::map{}) {
        const auto* name = std::get_if<std::string>(&key.data);
        if (!name) {
            continue;
        }
        if (*name == "v") {
            if (const auto* number = std::get_if<std::uint64_t>(&value.data)) {
                version = *number;
            }
        } else if (const auto* text = std::get_if<std::string>(&value.data)) {
            if (*name == "lake") {
                output.lake = *text;
            } else if (*name == "repository") {
                output.repository = *text;
            }
        }
    }
    if (version != 1 || output.lake.empty()) {
        co_return std::unexpected(lake_error(
            errc::corrupt, "the repository's lake record is invalid", repository_lake_key));
    }
    co_return std::optional<repository_lake>{std::move(output)};
}

runtime::task<void> write_repository_lake(repository& subject, repository_lake value) {
    if (!valid_name(value.lake) ||
        (!value.repository.empty() && !value.repository.starts_with('/'))) {
        co_return std::unexpected(
            lake_error(errc::invalid_argument, "invalid repository lake record", value.lake));
    }
    auto encoded = cbor::encode(cbor::value{cbor::value::map{
        {"v", std::uint64_t{1}},
        {"lake", std::move(value.lake)},
        {"repository", std::move(value.repository)},
    }});
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await subject.set_metadata(repository_lake_key, std::move(*encoded));
}

auto repository_path_for(const lake& target, std::string_view repository) -> result<std::string> {
    if (repository.empty()) {
        return std::string{};
    }
    if (is_url(repository)) {
        auto parsed_lake = net_detail::parse_url(target.url, "lake URL");
        auto parsed = net_detail::parse_url(repository, "repository URL");
        if (!parsed_lake || !parsed || parsed->origin != parsed_lake->origin) {
            return std::unexpected(
                lake_error(errc::invalid_argument,
                           "the repository URL is not on the lake '" + target.name + "'",
                           std::string{repository}));
        }
        const auto& prefix = parsed_lake->path;
        if (prefix == "/") {
            return parsed->path;
        }
        if (parsed->path.size() <= prefix.size() || !parsed->path.starts_with(prefix) ||
            parsed->path[prefix.size()] != '/') {
            return std::unexpected(
                lake_error(errc::invalid_argument,
                           "the repository URL is not under the lake '" + target.name + "'",
                           std::string{repository}));
        }
        return parsed->path.substr(prefix.size());
    }
    if (repository.starts_with('/')) {
        if (repository.find_first_of("?# \t\r\n") != std::string_view::npos) {
            return std::unexpected(lake_error(
                errc::invalid_argument, "invalid repository path", std::string{repository}));
        }
        return std::string{repository};
    }
    if (repository.find_first_of("/?# \t\r\n:") != std::string_view::npos) {
        return std::unexpected(lake_error(
            errc::invalid_argument, "invalid repository identifier", std::string{repository}));
    }
    return "/v1/repositories/" + std::string{repository};
}

runtime::task<lake_target>
resolve_lake_target(const lake_registry& lakes, repository* subject, std::string argument) {
    if (is_url(argument)) {
        auto parsed = net_detail::parse_url(argument, "repository URL");
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto output = lake_target{};
        output.named = lakes.match(argument);
        output.repository_url = argument;
        if (output.named) {
            output.base_url = output.named->url;
            auto token = co_await lakes.token(output.named->name);
            if (!token) {
                co_return std::unexpected(token.error());
            }
            output.token = std::move(*token);
        } else {
            output.base_url = unnamed_base(argument);
            output.token = shared_token();
        }
        co_return output;
    }
    auto recorded = std::optional<repository_lake>{};
    if (subject) {
        auto read = co_await read_repository_lake(*subject);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        recorded = std::move(*read);
    }
    auto name = argument.empty() && recorded ? recorded->lake : argument;
    if (name.empty()) {
        if (lakes.list().empty()) {
            co_return std::unexpected(lake_error(
                errc::not_configured,
                "no lake is configured: add one with `liquid lake add <name> <url>`, then "
                "`liquid lake use <name> <repository>` in this repository, or give a full URL",
                "lake"));
        }
        auto names = std::string{};
        for (const auto& entry : lakes.list()) {
            names += names.empty() ? entry.name : ", " + entry.name;
        }
        co_return std::unexpected(lake_error(errc::not_configured,
                                             "this repository has no lake: run `liquid lake use "
                                             "<name> <repository>` (lakes: " +
                                                 names + "), or give a full URL",
                                             "lake"));
    }
    auto entry = lakes.find(name);
    if (!entry) {
        co_return std::unexpected(lake_error(
            errc::not_found,
            "no lake is named '" + name + "': add it with `liquid lake add " + name + " <url>`",
            name));
    }
    if (!recorded || recorded->repository.empty()) {
        co_return std::unexpected(lake_error(errc::not_configured,
                                             "this repository has no repository on the lake '" +
                                                 name + "': run `liquid lake use " + name +
                                                 " <repository>`",
                                             name));
    }
    auto token = co_await lakes.token(entry->name);
    if (!token) {
        co_return std::unexpected(token.error());
    }
    co_return lake_target{.named = entry,
                          .base_url = entry->url,
                          .repository_url = entry->url + recorded->repository,
                          .token = std::move(*token)};
}

auto supported_sync_protocol_versions() -> std::span<const std::uint32_t> {
    return sync_protocols;
}

auto supported_forge_api_versions() -> std::span<const std::uint32_t> {
    return forge_apis;
}

runtime::task<lake_server_info> fetch_lake_server_info(std::string base_url) {
    auto parsed = net_detail::parse_url(base_url, "lake URL");
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    auto target = net_detail::request_target(*parsed, "/v1/server_info");
    auto response = co_await net_detail::perform_request(net_detail::http_request{
        .method = net_detail::http_method::get,
        .url = *parsed,
        .target = std::move(target),
    });
    if (!response) {
        co_return std::unexpected(response.error());
    }
    if (response->status != 200) {
        co_return std::unexpected(
            lake_error(response->status == 404 ? errc::not_found : errc::unavailable,
                       "server_info answered HTTP " + std::to_string(response->status),
                       base_url));
    }
    try {
        const auto* first = reinterpret_cast<const char*>(response->body.data());
        auto document = json::parse(first, first + response->body.size());
        if (document.is_object() && document.contains("value") &&
            document.at("value").is_object()) {
            document = document.at("value");
        }
        if (!document.is_object()) {
            co_return std::unexpected(
                lake_error(errc::corrupt, "server_info is not a JSON object", base_url));
        }
        auto output = lake_server_info{
            .api = string_field(document, "api"),
            .name = string_field(document, "name"),
            .version = string_field(document, "version"),
            .revision = string_field(document, "revision"),
            .sync_protocol_versions = unsigned_array(document, "sync_protocol_versions"),
            .forge_api_versions = unsigned_array(document, "forge_api_versions"),
        };
        if (const auto range = document.find("liquid_versions");
            range != document.end() && range->is_object()) {
            output.liquid_oldest = string_field(*range, "oldest");
            output.liquid_newest = string_field(*range, "newest");
        }
        co_return output;
    } catch (const json::exception& exception) {
        co_return std::unexpected(lake_error(errc::corrupt, exception.what(), base_url));
    }
}

auto check_lake_compatibility(std::string_view label, const lake_server_info& info)
    -> lake_compatibility {
    const auto ours = current_version();
    const auto liquid_version = parsed_version{ours.major, ours.minor, ours.patch};
    const auto supported = supported_sync_protocol_versions();
    const auto ours_text = std::format(
        "liquid {} supports sync protocol {}", version_string(), versions_text(supported));
    const auto oldest = parse_version(info.liquid_oldest);
    const auto newest = parse_version(info.liquid_newest);
    if (info.sync_protocol_versions.empty() && !oldest && !newest) {
        return {lake_compatibility_kind::unknown,
                std::format("lake '{}' ({}) does not report the sync protocol versions or the "
                            "liquid versions it supports; continuing ({})",
                            label,
                            lake_version_text(info),
                            ours_text),
                info};
    }
    auto lake_text = lake_version_text(info);
    if (!info.sync_protocol_versions.empty()) {
        lake_text += ", sync protocol " + versions_text(info.sync_protocol_versions);
    }
    if (oldest || newest) {
        lake_text += std::format(", liquid {} to {}",
                                 info.liquid_oldest.empty() ? "any" : info.liquid_oldest,
                                 info.liquid_newest.empty() ? "any" : info.liquid_newest);
    }
    const auto shared_protocol =
        info.sync_protocol_versions.empty() ||
        std::ranges::any_of(info.sync_protocol_versions, [&](std::uint32_t value) {
            return std::ranges::find(supported, value) != supported.end();
        });
    auto advice = std::string{};
    if (oldest && liquid_version < *oldest) {
        advice = "upgrade liquid";
    } else if (newest && liquid_version > *newest) {
        advice = "upgrade the lake, or use a liquid it supports";
    } else if (!shared_protocol) {
        advice = std::ranges::max(info.sync_protocol_versions) < std::ranges::min(supported)
                     ? "upgrade the lake"
                     : "upgrade liquid";
    }
    if (!advice.empty()) {
        return {lake_compatibility_kind::unsupported,
                std::format("lake '{}' ({}) does not work with this liquid ({}): {}",
                            label,
                            lake_text,
                            ours_text,
                            advice),
                info};
    }
    return {lake_compatibility_kind::compatible,
            std::format("lake '{}' ({}) works with this liquid ({})", label, lake_text, ours_text),
            info};
}

runtime::task<lake_compatibility> check_lake(const lake_target& target) {
    const auto key = target.base_url;
    {
        auto lock = std::scoped_lock{checked_mutex};
        if (const auto found = checked_lakes.find(key); found != checked_lakes.end()) {
            co_return found->second;
        }
    }
    const auto label = target.named ? target.named->name : target.base_url;
    auto info = co_await fetch_lake_server_info(target.base_url);
    if (!info) {
        co_return lake_compatibility{
            lake_compatibility_kind::unknown,
            std::format("lake '{}' did not answer server_info ({}); continuing with its version "
                        "unknown",
                        label,
                        info.error().message),
            {}};
    }
    auto decided = check_lake_compatibility(label, *info);
    auto lock = std::scoped_lock{checked_mutex};
    checked_lakes.insert_or_assign(key, decided);
    co_return decided;
}

runtime::task<lake_compatibility> require_compatible_lake(const lake_target& target) {
    auto decided = co_await check_lake(target);
    if (!decided) {
        co_return std::unexpected(decided.error());
    }
    if (decided->kind == lake_compatibility_kind::unsupported) {
        co_return std::unexpected(lake_error(errc::unsupported,
                                             decided->message,
                                             target.named ? target.named->name : target.base_url));
    }
    co_return std::move(*decided);
}

} // namespace liquid
