// SPDX-License-Identifier: MIT

#include "liquid/install.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto valid_name(std::string_view value) -> bool {
    if (value.empty() || (value.front() != '_' && (value.front() < 'a' || value.front() > 'z'))) {
        return false;
    }
    return std::ranges::all_of(value.substr(1), [](const char item) {
        return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') || item == '_' ||
               item == '-';
    });
}

auto valid_path(const std::filesystem::path& value) -> bool {
    const auto text = value.generic_string();
    // libstdc++'s generic format and path comparison both collapse repeated separators, so
    // "/usr//bin" passes those checks on Linux; the path as written must be checked (#348).
    const auto written = value.string();
    if (text.empty() || text.front() != '/' || text == "/" ||
        written.find("//") != std::string::npos || value.lexically_normal() != value) {
        return false;
    }
    return std::ranges::all_of(text, [](const char item) {
        return std::isalnum(static_cast<unsigned char>(item)) != 0 || item == '/' || item == '.' ||
               item == '_' || item == '+' || item == '-';
    });
}

auto validate_name(std::string_view value, std::string_view field) -> result<void> {
    if (!valid_name(value)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       std::string{field} + " has an invalid name",
                                       std::string{value}));
    }
    return {};
}

auto validate_path(const std::filesystem::path& value, std::string_view field) -> result<void> {
    if (!valid_path(value)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       std::string{field} + " is not a canonical absolute path",
                                       value.generic_string()));
    }
    return {};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto find(const cbor::value& source, std::string_view name) -> const cbor::value* {
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

auto text(const cbor::value* value, std::string_view field) -> result<std::string> {
    const auto* output = value ? std::get_if<std::string>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            failure(errc::corrupt, std::string{field} + " is not text", "install_intent/1"));
    }
    return *output;
}

auto boolean(const cbor::value* value, std::string_view field) -> result<bool> {
    const auto* output = value ? std::get_if<bool>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            failure(errc::corrupt, std::string{field} + " is not boolean", "install_intent/1"));
    }
    return *output;
}

auto unsigned_number(const cbor::value* value, std::string_view field) -> result<std::uint64_t> {
    const auto* output = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            failure(errc::corrupt, std::string{field} + " is not unsigned", "install_intent/1"));
    }
    return *output;
}

auto signed_number(const cbor::value* value, std::string_view field) -> result<std::int64_t> {
    if (const auto* output = value ? std::get_if<std::int64_t>(&value->data) : nullptr) {
        return *output;
    }
    if (const auto* output = value ? std::get_if<std::uint64_t>(&value->data) : nullptr;
        output && *output <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(*output);
    }
    return std::unexpected(
        failure(errc::corrupt, std::string{field} + " is not an integer", "install_intent/1"));
}

auto array(const cbor::value* value, std::string_view field) -> result<const cbor::value::array*> {
    const auto* output = value ? std::get_if<cbor::value::array>(&value->data) : nullptr;
    if (!output) {
        return std::unexpected(
            failure(errc::corrupt, std::string{field} + " is not an array", "install_intent/1"));
    }
    return output;
}

auto encode_strings(std::span<const std::string> values) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.emplace_back(value);
    }
    return cbor::value{std::move(output)};
}

auto decode_strings(const cbor::value* value, std::string_view field)
    -> result<std::vector<std::string>> {
    auto values = array(value, field);
    if (!values) {
        return std::unexpected(values.error());
    }
    auto output = std::vector<std::string>{};
    for (const auto& item : **values) {
        auto item_text = text(&item, field);
        if (!item_text) {
            return std::unexpected(item_text.error());
        }
        output.push_back(std::move(*item_text));
    }
    return output;
}

template <class Value, class Encode>
auto encode_array(const std::vector<Value>& values, Encode encode) -> cbor::value {
    auto output = cbor::value::array{};
    for (const auto& value : values) {
        output.push_back(encode(value));
    }
    return cbor::value{std::move(output)};
}

template <class Value, class Decode>
auto decode_array(const cbor::value* value, std::string_view field, Decode decode)
    -> result<std::vector<Value>> {
    auto values = array(value, field);
    if (!values) {
        return std::unexpected(values.error());
    }
    auto output = std::vector<Value>{};
    for (const auto& item : **values) {
        auto decoded = decode(item);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        output.push_back(std::move(*decoded));
    }
    return output;
}

auto service_value(const install_service& value) -> cbor::value {
    return map({{"name", value.name},
                {"exec", value.executable.generic_string()},
                {"user", value.user},
                {"after", encode_strings(value.after)},
                {"restart", std::string{to_string(value.restart)}}});
}

auto user_value(const install_user& value) -> cbor::value {
    return map(
        {{"name", value.name}, {"system", value.system}, {"home", value.home.generic_string()}});
}

auto group_value(const install_group& value) -> cbor::value {
    return map({{"name", value.name}, {"system", value.system}});
}

auto directory_value(const install_directory& value) -> cbor::value {
    return map({{"path", value.path.generic_string()},
                {"mode", static_cast<std::uint64_t>(value.mode)},
                {"owner", value.owner}});
}

auto symlink_value(const install_symlink& value) -> cbor::value {
    return map({{"path", value.path.generic_string()}, {"target", value.target.generic_string()}});
}

auto alternative_value(const install_alternative& value) -> cbor::value {
    return map({{"name", value.name},
                {"path", value.path.generic_string()},
                {"priority", static_cast<std::int64_t>(value.priority)}});
}

auto decode_service(const cbor::value& value) -> result<install_service> {
    auto name = text(find(value, "name"), "service name");
    auto executable = text(find(value, "exec"), "service executable");
    auto user = text(find(value, "user"), "service user");
    auto after = decode_strings(find(value, "after"), "service after");
    auto restart = text(find(value, "restart"), "service restart");
    if (!name || !executable || !user || !after || !restart) {
        return std::unexpected(!name         ? name.error()
                               : !executable ? executable.error()
                               : !user       ? user.error()
                               : !after      ? after.error()
                                             : restart.error());
    }
    auto restart_value = service_restart::on_failure;
    if (*restart == "no") {
        restart_value = service_restart::no;
    } else if (*restart == "always") {
        restart_value = service_restart::always;
    } else if (*restart != "on-failure") {
        return std::unexpected(failure(errc::corrupt, "unknown service restart policy", *restart));
    }
    return install_service{std::move(*name),
                           std::move(*executable),
                           std::move(*user),
                           std::move(*after),
                           restart_value};
}

auto decode_user(const cbor::value& value) -> result<install_user> {
    auto name = text(find(value, "name"), "user name");
    auto system = boolean(find(value, "system"), "user system");
    auto home = text(find(value, "home"), "user home");
    if (!name || !system || !home) {
        return std::unexpected(!name ? name.error() : !system ? system.error() : home.error());
    }
    return install_user{std::move(*name), *system, std::move(*home)};
}

auto decode_group(const cbor::value& value) -> result<install_group> {
    auto name = text(find(value, "name"), "group name");
    auto system = boolean(find(value, "system"), "group system");
    if (!name || !system) {
        return std::unexpected(!name ? name.error() : system.error());
    }
    return install_group{std::move(*name), *system};
}

auto decode_directory(const cbor::value& value) -> result<install_directory> {
    auto path = text(find(value, "path"), "directory path");
    auto mode = unsigned_number(find(value, "mode"), "directory mode");
    auto owner = text(find(value, "owner"), "directory owner");
    if (!path || !mode || !owner || *mode > 0777) {
        return std::unexpected(!path    ? path.error()
                               : !mode  ? mode.error()
                               : !owner ? owner.error()
                                        : failure(errc::corrupt,
                                                  "directory mode is outside 0000..0777",
                                                  "install_intent/1"));
    }
    return install_directory{
        std::move(*path), static_cast<std::uint32_t>(*mode), std::move(*owner)};
}

auto decode_symlink(const cbor::value& value) -> result<install_symlink> {
    auto path = text(find(value, "path"), "symlink path");
    auto target = text(find(value, "target"), "symlink target");
    if (!path || !target) {
        return std::unexpected(!path ? path.error() : target.error());
    }
    return install_symlink{std::move(*path), std::move(*target)};
}

auto decode_alternative(const cbor::value& value) -> result<install_alternative> {
    auto name = text(find(value, "name"), "alternative name");
    auto path = text(find(value, "path"), "alternative path");
    auto priority = signed_number(find(value, "priority"), "alternative priority");
    if (!name || !path || !priority || *priority < std::numeric_limits<std::int32_t>::min() ||
        *priority > std::numeric_limits<std::int32_t>::max()) {
        return std::unexpected(!name       ? name.error()
                               : !path     ? path.error()
                               : !priority ? priority.error()
                                           : failure(errc::corrupt,
                                                     "alternative priority is outside int32",
                                                     "install_intent/1"));
    }
    return install_alternative{
        std::move(*name), std::move(*path), static_cast<std::int32_t>(*priority)};
}

auto cache_from_text(std::string_view value) -> result<install_cache> {
    for (const auto kind : {install_cache::ldconfig,
                            install_cache::desktop_database,
                            install_cache::mime_database,
                            install_cache::man_db,
                            install_cache::icon_cache,
                            install_cache::systemd_daemon_reload,
                            install_cache::fonts}) {
        if (to_string(kind) == value) {
            return kind;
        }
    }
    return std::unexpected(failure(errc::corrupt, "unknown install cache", std::string{value}));
}

} // namespace

std::string_view to_string(service_restart value) noexcept {
    switch (value) {
    case service_restart::no:
        return "no";
    case service_restart::on_failure:
        return "on-failure";
    case service_restart::always:
        return "always";
    }
    return "no";
}

std::string_view to_string(install_cache value) noexcept {
    switch (value) {
    case install_cache::ldconfig:
        return "ldconfig";
    case install_cache::desktop_database:
        return "desktop-database";
    case install_cache::mime_database:
        return "mime-database";
    case install_cache::man_db:
        return "man-db";
    case install_cache::icon_cache:
        return "icon-cache";
    case install_cache::systemd_daemon_reload:
        return "systemd-daemon-reload";
    case install_cache::fonts:
        return "fonts";
    }
    return "ldconfig";
}

result<void> validate_install_intent(const install_intent& value) {
    if (value.v != 1) {
        return std::unexpected(failure(
            errc::unsupported, "unsupported install intent version", std::to_string(value.v)));
    }
    for (const auto& service : value.services) {
        if (auto valid = validate_name(service.name, "service"); !valid) {
            return valid;
        }
        if (auto valid = validate_path(service.executable, "service executable"); !valid) {
            return valid;
        }
        if (auto valid = validate_name(service.user, "service user"); !valid) {
            return valid;
        }
        for (const auto& dependency : service.after) {
            if (auto valid = validate_name(dependency, "service dependency"); !valid) {
                return valid;
            }
        }
    }
    for (const auto& user : value.users) {
        if (auto valid = validate_name(user.name, "user"); !valid) {
            return valid;
        }
        if (auto valid = validate_path(user.home, "user home"); !valid) {
            return valid;
        }
    }
    for (const auto& group : value.groups) {
        if (auto valid = validate_name(group.name, "group"); !valid) {
            return valid;
        }
    }
    for (const auto& directory : value.directories) {
        if (auto valid = validate_path(directory.path, "directory"); !valid) {
            return valid;
        }
        if (directory.mode > 0777) {
            return std::unexpected(
                failure(errc::invalid_argument, "directory mode is outside 0000..0777"));
        }
        if (auto valid = validate_name(directory.owner, "directory owner"); !valid) {
            return valid;
        }
    }
    for (const auto& symlink : value.symlinks) {
        if (auto valid = validate_path(symlink.path, "symlink path"); !valid) {
            return valid;
        }
        if (auto valid = validate_path(symlink.target, "symlink target"); !valid) {
            return valid;
        }
    }
    for (const auto& alternative : value.alternatives) {
        if (auto valid = validate_name(alternative.name, "alternative"); !valid) {
            return valid;
        }
        if (auto valid = validate_path(alternative.path, "alternative path"); !valid) {
            return valid;
        }
    }
    return {};
}

result<std::vector<std::byte>> encode_install_intent(const install_intent& value) {
    auto valid = validate_install_intent(value);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    auto caches = cbor::value::array{};
    for (const auto cache : value.caches) {
        caches.emplace_back(std::string{to_string(cache)});
    }
    return cbor::encode(
        map({{"v", value.v},
             {"kind", "install_intent"},
             {"services", encode_array(value.services, service_value)},
             {"users", encode_array(value.users, user_value)},
             {"groups", encode_array(value.groups, group_value)},
             {"directories", encode_array(value.directories, directory_value)},
             {"symlinks", encode_array(value.symlinks, symlink_value)},
             {"caches", cbor::value{std::move(caches)}},
             {"alternatives", encode_array(value.alternatives, alternative_value)}}));
}

result<install_intent> decode_install_intent(std::span<const std::byte> bytes) {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto canonical = cbor::encode(*decoded);
    if (!canonical || !std::ranges::equal(*canonical, bytes)) {
        return std::unexpected(
            failure(errc::corrupt, "install intent is not canonical CBOR", "install_intent/1"));
    }
    auto version = unsigned_number(find(*decoded, "v"), "version");
    auto kind = text(find(*decoded, "kind"), "kind");
    if (!version || !kind || *version != 1 || *kind != "install_intent") {
        return std::unexpected(
            failure(errc::corrupt, "unsupported install intent record", "install_intent/1"));
    }
    auto output = install_intent{};
    auto services =
        decode_array<install_service>(find(*decoded, "services"), "services", decode_service);
    auto users = decode_array<install_user>(find(*decoded, "users"), "users", decode_user);
    auto groups = decode_array<install_group>(find(*decoded, "groups"), "groups", decode_group);
    auto directories = decode_array<install_directory>(
        find(*decoded, "directories"), "directories", decode_directory);
    auto symlinks =
        decode_array<install_symlink>(find(*decoded, "symlinks"), "symlinks", decode_symlink);
    auto alternatives = decode_array<install_alternative>(
        find(*decoded, "alternatives"), "alternatives", decode_alternative);
    auto cache_values = decode_strings(find(*decoded, "caches"), "caches");
    if (!services || !users || !groups || !directories || !symlinks || !alternatives ||
        !cache_values) {
        return std::unexpected(!services       ? services.error()
                               : !users        ? users.error()
                               : !groups       ? groups.error()
                               : !directories  ? directories.error()
                               : !symlinks     ? symlinks.error()
                               : !alternatives ? alternatives.error()
                                               : cache_values.error());
    }
    output.services = std::move(*services);
    output.users = std::move(*users);
    output.groups = std::move(*groups);
    output.directories = std::move(*directories);
    output.symlinks = std::move(*symlinks);
    output.alternatives = std::move(*alternatives);
    for (const auto& value : *cache_values) {
        auto cache = cache_from_text(value);
        if (!cache) {
            return std::unexpected(cache.error());
        }
        output.caches.push_back(*cache);
    }
    auto valid = validate_install_intent(output);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    return output;
}

} // namespace liquid
