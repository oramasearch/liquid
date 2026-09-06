#include "liquid/install.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "formats/apk/apk.hpp"
#include "formats/deb/deb.hpp"
#include "formats/package/common.hpp"
#include "formats/rpm/rpm.hpp"
#include "install/faults.hpp"

namespace liquid::install_detail {
namespace {

struct source_entry {
    std::filesystem::path path;
    std::optional<std::filesystem::path> source;
    std::vector<std::byte> bytes;
    std::uint32_t mode{0644};
    bool symlink{};
    std::filesystem::path target;
};

struct staged_source {
    std::vector<source_entry> entries;
    std::vector<install_intent> intents;
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto read_bytes(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read install source", path.string()));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(failure(errc::io, "cannot size install source", path.string()));
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input && !output.empty()) {
        return std::unexpected(failure(errc::io, "cannot read install source", path.string()));
    }
    return output;
}

auto safe_relative(std::filesystem::path value) -> result<std::filesystem::path> {
    while (!value.empty() && (*value.begin() == "." || value.is_absolute())) {
        if (value.is_absolute()) {
            value = value.relative_path();
            break;
        }
        auto next = std::filesystem::path{};
        for (auto iterator = std::next(value.begin()); iterator != value.end(); ++iterator) {
            next /= *iterator;
        }
        value = std::move(next);
    }
    if (value.empty() || std::ranges::find(value, "..") != value.end() ||
        value.lexically_normal() != value) {
        return std::unexpected(
            failure(errc::corrupt, "install source path escapes its root", value.string()));
    }
    return value;
}

auto package_source(const std::filesystem::path& path) -> result<staged_source> {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto payload = std::vector<formats::package::file>{};
    if (path.extension() == ".deb") {
        auto archive = formats::deb::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        payload = std::move(archive->payload);
    } else if (path.extension() == ".rpm") {
        auto archive = formats::rpm::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        payload = std::move(archive->payload);
    } else if (path.extension() == ".apk") {
        auto archive = formats::apk::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        payload = std::move(archive->payload);
    } else {
        return std::unexpected(failure(
            errc::invalid_argument, "install source is not a package or directory", path.string()));
    }
    auto output = staged_source{};
    for (auto& file : payload) {
        auto relative = safe_relative(file.path);
        if (!relative) {
            return std::unexpected(relative.error());
        }
        if (relative->generic_string().starts_with("usr/share/liquid/intents/") &&
            relative->extension() == ".cbor") {
            auto intent = decode_install_intent(file.bytes);
            if (!intent) {
                return std::unexpected(intent.error());
            }
            output.intents.push_back(std::move(*intent));
        }
        output.entries.push_back(
            {.path = std::move(*relative), .bytes = std::move(file.bytes), .mode = file.mode});
    }
    return output;
}

auto directory_source(const std::filesystem::path& path) -> result<staged_source> {
    auto paths = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 path, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (iterator->is_regular_file() || iterator->is_symlink()) {
            paths.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    std::ranges::sort(paths);
    auto output = staged_source{};
    for (const auto& source : paths) {
        auto relative = source.lexically_relative(path);
        auto status = std::filesystem::symlink_status(source, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), source.string()));
        }
        auto entry = source_entry{.path = relative};
        if (std::filesystem::is_symlink(status)) {
            entry.symlink = true;
            entry.target = std::filesystem::read_symlink(source, code);
            if (code) {
                return std::unexpected(failure(errc::io, code.message(), source.string()));
            }
        } else {
            entry.source = source;
            entry.mode = static_cast<std::uint32_t>(status.permissions()) & 0777U;
            if (relative.generic_string().starts_with("usr/share/liquid/intents/") &&
                relative.extension() == ".cbor") {
                auto bytes = read_bytes(source);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                auto intent = decode_install_intent(*bytes);
                if (!intent) {
                    return std::unexpected(intent.error());
                }
                output.intents.push_back(std::move(*intent));
            }
        }
        output.entries.push_back(std::move(entry));
    }
    return output;
}

auto load_source(const std::filesystem::path& path) -> result<staged_source> {
    auto code = std::error_code{};
    if (std::filesystem::is_directory(path, code)) {
        return directory_source(path);
    }
    if (code || !std::filesystem::is_regular_file(path, code)) {
        return std::unexpected(failure(errc::not_found, "install source is absent", path.string()));
    }
    return package_source(path);
}

auto numeric_name(const std::filesystem::path& value) -> std::optional<std::uint64_t> {
    const auto text = value.filename().string();
    auto output = std::uint64_t{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), output);
    if (code != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return output;
}

auto current_number(const std::filesystem::path& root) -> result<std::optional<std::uint64_t>> {
    auto code = std::error_code{};
    const auto current = root / "current";
    if (!std::filesystem::is_symlink(current, code)) {
        if (!code || code == std::errc::no_such_file_or_directory) {
            return std::optional<std::uint64_t>{};
        }
        return std::unexpected(failure(errc::io, code.message(), current.string()));
    }
    auto target = std::filesystem::read_symlink(current, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), current.string()));
    }
    auto number = numeric_name(target);
    if (!number) {
        return std::unexpected(
            failure(errc::corrupt, "current generation target is invalid", target.string()));
    }
    return number;
}

auto generation_list(const std::filesystem::path& root) -> result<std::vector<install_generation>> {
    auto current = current_number(root);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto generations = root / ".liquid/generations";
    auto code = std::error_code{};
    auto output = std::vector<install_generation>{};
    if (!std::filesystem::exists(generations, code)) {
        return output;
    }
    for (const auto& entry : std::filesystem::directory_iterator{generations, code}) {
        if (!entry.is_directory()) {
            continue;
        }
        auto number = numeric_name(entry.path());
        if (number) {
            output.push_back({*number, entry.path(), current && *current == number});
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), generations.string()));
    }
    std::ranges::sort(output, {}, &install_generation::number);
    return output;
}

auto make_directories(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    return {};
}

auto write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes,
                 std::uint32_t mode) -> result<void> {
    auto created = make_directories(path.parent_path());
    if (!created) {
        return created;
    }
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        return std::unexpected(failure(errc::io, "cannot write staged file", path.string()));
    }
    auto code = std::error_code{};
    std::filesystem::permissions(path,
                                 static_cast<std::filesystem::perms>(mode),
                                 std::filesystem::perm_options::replace,
                                 code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    visit_fault({fault_operation::staged_file, path});
    return {};
}

auto stage_entry(const source_entry& entry,
                 const std::filesystem::path& staging,
                 std::uint64_t& linked) -> result<void> {
    const auto destination = staging / entry.path;
    auto created = make_directories(destination.parent_path());
    if (!created) {
        return created;
    }
    auto code = std::error_code{};
    if (entry.symlink) {
        std::filesystem::create_symlink(entry.target, destination, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), destination.string()));
        }
        visit_fault({fault_operation::staged_file, destination});
        return {};
    }
    if (entry.source) {
        std::filesystem::create_hard_link(*entry.source, destination, code);
        if (!code) {
            ++linked;
            visit_fault({fault_operation::staged_file, destination});
            return {};
        }
        code.clear();
        std::filesystem::copy_file(
            *entry.source, destination, std::filesystem::copy_options::none, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), destination.string()));
        }
        visit_fault({fault_operation::staged_file, destination});
        return {};
    }
    return write_bytes(destination, entry.bytes, entry.mode);
}

auto generation_path(const std::filesystem::path& root, const std::filesystem::path& absolute)
    -> std::filesystem::path {
    return root / absolute.relative_path();
}

auto append_line_if_absent(const std::filesystem::path& path,
                           std::string_view prefix,
                           std::string line) -> result<void> {
    auto existing = std::string{};
    {
        auto input = std::ifstream{path};
        existing.assign(std::istreambuf_iterator<char>{input}, {});
    }
    auto input = std::istringstream{existing};
    auto selected = std::string{};
    while (std::getline(input, selected)) {
        if (selected.starts_with(prefix)) {
            return {};
        }
    }
    if (!existing.empty() && !existing.ends_with('\n')) {
        existing.push_back('\n');
    }
    existing += std::move(line);
    existing.push_back('\n');
    return write_bytes(
        path, {reinterpret_cast<const std::byte*>(existing.data()), existing.size()}, 0644);
}

auto apply_intent(const install_intent& intent, const std::filesystem::path& staging)
    -> result<void> {
    for (const auto& directory : intent.directories) {
        const auto path = generation_path(staging, directory.path);
        auto created = make_directories(path);
        if (!created) {
            return created;
        }
        auto code = std::error_code{};
        std::filesystem::permissions(path,
                                     static_cast<std::filesystem::perms>(directory.mode),
                                     std::filesystem::perm_options::replace,
                                     code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.string()));
        }
    }
    auto group_id = std::uint32_t{60000};
    for (const auto& group : intent.groups) {
        const auto path = staging / "etc/group";
        auto created = make_directories(path.parent_path());
        if (!created) {
            return created;
        }
        auto appended = append_line_if_absent(
            path, group.name + ":", group.name + ":x:" + std::to_string(group_id++) + ":");
        if (!appended) {
            return appended;
        }
    }
    auto user_id = std::uint32_t{60000};
    for (const auto& user : intent.users) {
        const auto path = staging / "etc/passwd";
        auto created = make_directories(path.parent_path());
        if (!created) {
            return created;
        }
        auto appended = append_line_if_absent(
            path,
            user.name + ":",
            user.name + ":x:" + std::to_string(user_id) + ":" + std::to_string(user_id) +
                "::" + user.home.generic_string() + ":/usr/sbin/nologin");
        if (!appended) {
            return appended;
        }
        ++user_id;
    }
    for (const auto& symlink : intent.symlinks) {
        const auto path = generation_path(staging, symlink.path);
        auto created = make_directories(path.parent_path());
        if (!created) {
            return created;
        }
        auto code = std::error_code{};
        if (std::filesystem::is_symlink(path, code) &&
            std::filesystem::read_symlink(path, code) == symlink.target) {
            continue;
        }
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::create_symlink(symlink.target, path, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.string()));
        }
        visit_fault({fault_operation::staged_file, path});
    }
    for (const auto& alternative : intent.alternatives) {
        const auto path = staging / "usr/bin" / alternative.name;
        auto created = make_directories(path.parent_path());
        if (!created) {
            return created;
        }
        auto code = std::error_code{};
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::create_symlink(alternative.path, path, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.string()));
        }
        visit_fault({fault_operation::staged_file, path});
    }
    for (const auto& service : intent.services) {
        const auto wants = staging / "etc/systemd/system/multi-user.target.wants";
        auto created = make_directories(wants);
        if (!created) {
            return created;
        }
        const auto path = wants / (service.name + ".service");
        auto code = std::error_code{};
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::create_symlink(
            "/usr/lib/systemd/system/" + service.name + ".service", path, code);
        if (code) {
            return std::unexpected(failure(errc::io, code.message(), path.string()));
        }
        visit_fault({fault_operation::staged_file, path});
    }
    return {};
}

auto switch_current(const std::filesystem::path& root, std::uint64_t number) -> result<void> {
    const auto current = root / "current";
    const auto temporary = root / ".liquid/current.next";
    auto code = std::error_code{};
    std::filesystem::remove(temporary, code);
    code.clear();
    std::filesystem::create_symlink(
        std::filesystem::path{".liquid/generations"} / std::to_string(number), temporary, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), temporary.string()));
    }
    visit_fault({fault_operation::switch_generation, current});
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(),
                     current.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return std::unexpected(
            failure(errc::io, "cannot switch current generation", std::to_string(GetLastError())));
    }
#else
    std::filesystem::rename(temporary, current, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), current.string()));
    }
#endif
    return {};
}

auto remove_staging(const std::filesystem::path& generations) -> result<void> {
    auto code = std::error_code{};
    if (!std::filesystem::exists(generations, code)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator{generations, code}) {
        if (entry.path().filename().string().ends_with(".staging")) {
            std::filesystem::remove_all(entry.path(), code);
            if (code) {
                return std::unexpected(failure(errc::io, code.message(), entry.path().string()));
            }
        }
    }
    return {};
}

auto install_sync(const std::filesystem::path& source, const std::filesystem::path& root)
    -> result<install_report> {
    auto loaded = load_source(source);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    const auto generations = root / ".liquid/generations";
    auto created = make_directories(generations);
    if (!created) {
        return std::unexpected(created.error());
    }
    auto cleaned = remove_staging(generations);
    if (!cleaned) {
        return std::unexpected(cleaned.error());
    }
    auto existing = generation_list(root);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    const auto number = existing->empty() ? std::uint64_t{1} : existing->back().number + 1;
    const auto staging = generations / (std::to_string(number) + ".staging");
    auto made = make_directories(staging);
    if (!made) {
        return std::unexpected(made.error());
    }
    auto linked = std::uint64_t{};
    for (const auto& entry : loaded->entries) {
        auto staged = stage_entry(entry, staging, linked);
        if (!staged) {
            return std::unexpected(staged.error());
        }
    }
    for (const auto& intent : loaded->intents) {
        auto applied = apply_intent(intent, staging);
        if (!applied) {
            return std::unexpected(applied.error());
        }
    }
    const auto destination = generations / std::to_string(number);
    auto code = std::error_code{};
    std::filesystem::rename(staging, destination, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), destination.string()));
    }
    auto switched = switch_current(root, number);
    if (!switched) {
        return std::unexpected(switched.error());
    }
    return install_report{{number, destination, true}, loaded->entries.size(), linked};
}

auto rollback_sync(const std::filesystem::path& root) -> result<install_generation> {
    auto generations = generation_list(root);
    if (!generations) {
        return std::unexpected(generations.error());
    }
    const auto current = std::ranges::find(*generations, true, &install_generation::current);
    if (current == generations->end()) {
        return std::unexpected(
            failure(errc::not_found, "there is no current generation", root.string()));
    }
    auto previous = generations->end();
    for (auto iterator = generations->begin(); iterator != current; ++iterator) {
        previous = iterator;
    }
    if (previous == generations->end()) {
        return std::unexpected(
            failure(errc::not_found, "there is no previous generation", root.string()));
    }
    auto switched = switch_current(root, previous->number);
    if (!switched) {
        return std::unexpected(switched.error());
    }
    previous->current = true;
    return *previous;
}

} // namespace
} // namespace liquid::install_detail

namespace liquid::installation {

runtime::task<install_report> install(std::filesystem::path source, std::filesystem::path root) {
    auto installed =
        co_await runtime::offload([source = std::move(source), root = std::move(root)] {
            return install_detail::install_sync(source, root);
        });
    if (!installed) {
        co_return std::unexpected(installed.error());
    }
    co_return std::move(*installed);
}

runtime::task<install_generation> rollback(std::filesystem::path root) {
    auto rolled_back = co_await runtime::offload(
        [root = std::move(root)] { return install_detail::rollback_sync(root); });
    if (!rolled_back) {
        co_return std::unexpected(rolled_back.error());
    }
    co_return std::move(*rolled_back);
}

runtime::task<std::vector<install_generation>> list(std::filesystem::path root) {
    auto generations = co_await runtime::offload(
        [root = std::move(root)] { return install_detail::generation_list(root); });
    if (!generations) {
        co_return std::unexpected(generations.error());
    }
    co_return std::move(*generations);
}

} // namespace liquid::installation
