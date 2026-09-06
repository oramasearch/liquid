#include "settings.hpp"

#include <filesystem>
#include <string_view>
#include <utility>

#include "encoding/cbor.hpp"

namespace liquid::drive_detail {
namespace {

namespace cbor = encoding::cbor;

auto settings_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto field(const cbor::value& value, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto encode(const stored_settings& settings) -> result<std::vector<std::byte>> {
    auto states = cbor::value::array{};
    for (const auto& [path, state] : settings.states) {
        states.push_back(cbor::value{cbor::value::map{
            {"path", path},
            {"state", static_cast<std::uint64_t>(state)},
        }});
    }
    auto hydrated = cbor::value::array{};
    for (const auto& path : settings.hydrated) {
        hydrated.push_back(path);
    }
    return cbor::encode(cbor::value{cbor::value::map{
        {"v", std::uint64_t{1}},
        {"hydration_size_threshold", settings.policy.hydration_size_threshold},
        {"pin_folders", settings.policy.pin_folders},
        {"states", cbor::value{std::move(states)}},
        {"hydrated", cbor::value{std::move(hydrated)}},
        {"offline_reason", settings.offline_reason},
    }});
}

auto decode(std::span<const std::byte> bytes) -> result<stored_settings> {
    auto value = cbor::decode(bytes);
    if (!value) {
        return std::unexpected(value.error());
    }
    const auto* version_value = field(*value, "v");
    const auto* threshold_value = field(*value, "hydration_size_threshold");
    const auto* folders_value = field(*value, "pin_folders");
    const auto* states_value = field(*value, "states");
    const auto* hydrated_value = field(*value, "hydrated");
    const auto* reason_value = field(*value, "offline_reason");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* threshold =
        threshold_value ? std::get_if<std::uint64_t>(&threshold_value->data) : nullptr;
    const auto* folders = folders_value ? std::get_if<bool>(&folders_value->data) : nullptr;
    const auto* states =
        states_value ? std::get_if<cbor::value::array>(&states_value->data) : nullptr;
    const auto* hydrated =
        hydrated_value ? std::get_if<cbor::value::array>(&hydrated_value->data) : nullptr;
    const auto* reason = reason_value ? std::get_if<std::string>(&reason_value->data) : nullptr;
    if (!version || *version != 1 || !threshold || !folders || !states || !hydrated || !reason) {
        return std::unexpected(settings_error(errc::corrupt, "invalid drive settings"));
    }
    auto output =
        stored_settings{.policy = {.hydration_size_threshold = *threshold, .pin_folders = *folders},
                        .offline_reason = *reason};
    for (const auto& item : *states) {
        const auto* path_value = field(item, "path");
        const auto* state_value = field(item, "state");
        const auto* path = path_value ? std::get_if<std::string>(&path_value->data) : nullptr;
        const auto* state = state_value ? std::get_if<std::uint64_t>(&state_value->data) : nullptr;
        if (!path || !state || *state < 1 || *state > 3) {
            return std::unexpected(settings_error(errc::corrupt, "invalid drive path state"));
        }
        auto normalized = normalize_path(*path);
        if (!normalized) {
            return std::unexpected(
                settings_error(errc::corrupt, "invalid persisted drive path", *path));
        }
        output.states.insert_or_assign(*normalized, static_cast<drive_path_state>(*state));
    }
    for (const auto& item : *hydrated) {
        const auto* path = std::get_if<std::string>(&item.data);
        if (!path) {
            return std::unexpected(settings_error(errc::corrupt, "invalid hydration record"));
        }
        auto normalized = normalize_path(*path);
        if (!normalized) {
            return std::unexpected(
                settings_error(errc::corrupt, "invalid persisted hydration path", *path));
        }
        output.hydrated.insert(std::move(*normalized));
    }
    return output;
}

} // namespace

auto normalize_path(std::string path) -> result<std::string> {
    auto value = std::filesystem::path{std::move(path)}.lexically_normal();
    if (value.is_absolute()) {
        return std::unexpected(
            settings_error(errc::invalid_argument, "drive path must be relative"));
    }
    auto output = value.generic_string();
    if (output == ".") {
        output.clear();
    }
    if (output == ".." || output.starts_with("../")) {
        return std::unexpected(
            settings_error(errc::invalid_argument, "drive path leaves the repository"));
    }
    return output;
}

auto state_name(drive_path_state state) -> std::string_view {
    switch (state) {
    case drive_path_state::online_only:
        return "online-only";
    case drive_path_state::available_offline:
        return "available-offline";
    case drive_path_state::not_synced_here:
        return "not-synced-here";
    }
    return "online-only";
}

auto parse_state(std::string_view value) -> result<drive_path_state> {
    if (value == "online-only") {
        return drive_path_state::online_only;
    }
    if (value == "available-offline") {
        return drive_path_state::available_offline;
    }
    if (value == "not-synced-here") {
        return drive_path_state::not_synced_here;
    }
    return std::unexpected(
        settings_error(errc::invalid_argument, "invalid drive state", std::string{value}));
}

auto effective_state(const stored_settings& settings, std::string_view path) -> drive_path_state {
    auto candidate = std::string{path};
    for (;;) {
        const auto found = settings.states.find(candidate);
        if (found != settings.states.end()) {
            if (candidate == path || found->second == drive_path_state::not_synced_here ||
                settings.policy.pin_folders) {
                return found->second;
            }
        }
        if (candidate.empty()) {
            break;
        }
        candidate = std::filesystem::path{candidate}.parent_path().generic_string();
    }
    return drive_path_state::online_only;
}

auto should_hydrate_fully(const stored_settings& settings,
                          std::string_view path,
                          std::uint64_t size) -> bool {
    return effective_state(settings, path) == drive_path_state::available_offline ||
           size <= settings.policy.hydration_size_threshold;
}

runtime::task<stored_settings> load_settings(repository& source) {
    auto bytes = co_await source.metadata(settings_metadata_key);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    if (!*bytes) {
        co_return stored_settings{};
    }
    co_return decode(**bytes);
}

runtime::task<void> save_settings(repository& source, const stored_settings& settings) {
    auto bytes = encode(settings);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    co_return co_await source.set_metadata(settings_metadata_key, std::move(*bytes));
}

runtime::task<stored_settings>
set_path_state(repository& source, std::string path, drive_path_state state) {
    auto normalized = normalize_path(std::move(path));
    if (!normalized) {
        co_return std::unexpected(normalized.error());
    }
    auto settings = co_await load_settings(source);
    if (!settings) {
        co_return std::unexpected(settings.error());
    }
    settings->states.insert_or_assign(*normalized, state);
    if (state == drive_path_state::not_synced_here) {
        settings->hydrated.erase(*normalized);
    }
    auto saved = co_await save_settings(source, *settings);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    co_return std::move(*settings);
}

runtime::task<void> record_hydrated(repository& source, std::string path) {
    auto normalized = normalize_path(std::move(path));
    if (!normalized) {
        co_return std::unexpected(normalized.error());
    }
    auto settings = co_await load_settings(source);
    if (!settings) {
        co_return std::unexpected(settings.error());
    }
    settings->hydrated.insert(std::move(*normalized));
    settings->offline_reason.clear();
    co_return co_await save_settings(source, *settings);
}

runtime::task<void> record_offline_reason(repository& source, std::string reason) {
    auto settings = co_await load_settings(source);
    if (!settings) {
        co_return std::unexpected(settings.error());
    }
    settings->offline_reason = std::move(reason);
    co_return co_await save_settings(source, *settings);
}

} // namespace liquid::drive_detail
