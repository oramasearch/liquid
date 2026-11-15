// SPDX-License-Identifier: MIT

#include "projection/projection.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <utility>

#include "formats/apk/apk.hpp"
#include "formats/deb/deb.hpp"
#include "formats/rpm/rpm.hpp"

namespace liquid::projection_detail {
namespace {

struct ownership {
    std::string package;
    std::string path;
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto canonical_package_path(std::string value) -> std::string {
    while (value.starts_with("./")) {
        value.erase(0, 2);
    }
    if (!value.starts_with('/')) {
        value.insert(value.begin(), '/');
    }
    return std::filesystem::path{value}.lexically_normal().generic_string();
}

auto prepared_ownership(const prepared_projection& prepared) -> std::vector<ownership> {
    auto output = std::vector<ownership>{};
    for (const auto& file : prepared.files) {
        output.push_back({prepared.metadata.name, canonical_package_path("usr/" + file.path)});
    }
    return output;
}

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read conflict inventory", path.string()));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(failure(errc::io, "cannot size conflict inventory", path.string()));
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input && !output.empty()) {
        return std::unexpected(failure(errc::io, "cannot read conflict inventory", path.string()));
    }
    return output;
}

auto metadata_field(std::string_view text, std::string_view name) -> std::string {
    const auto prefix = std::string{name} + ":";
    auto input = std::istringstream{std::string{text}};
    auto line = std::string{};
    while (std::getline(input, line)) {
        if (line.starts_with(prefix)) {
            line.erase(0, prefix.size());
            line.erase(0, line.find_first_not_of(" \t"));
            return line;
        }
    }
    return {};
}

auto apk_field(std::string_view text, std::string_view name) -> std::string {
    const auto prefix = std::string{name} + " = ";
    auto input = std::istringstream{std::string{text}};
    auto line = std::string{};
    while (std::getline(input, line)) {
        if (line.starts_with(prefix)) {
            return line.substr(prefix.size());
        }
    }
    return {};
}

auto read_package(const std::filesystem::path& path) -> result<std::vector<ownership>> {
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto package = std::string{};
    auto files = std::vector<std::string>{};
    if (path.extension() == ".deb") {
        auto archive = formats::deb::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        package = metadata_field(archive->control, "Package");
        files = std::move(archive->files);
    } else if (path.extension() == ".rpm") {
        auto archive = formats::rpm::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        package = std::move(archive->name);
        files = std::move(archive->files);
    } else if (path.extension() == ".apk") {
        auto archive = formats::apk::read(*bytes);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        package = apk_field(archive->pkginfo, "pkgname");
        files = std::move(archive->files);
    } else {
        return std::unexpected(
            failure(errc::invalid_argument, "unsupported package inventory file", path.string()));
    }
    if (package.empty()) {
        return std::unexpected(
            failure(errc::corrupt, "package inventory has no package name", path.string()));
    }
    auto output = std::vector<ownership>{};
    for (auto& file : files) {
        output.push_back({package, canonical_package_path(std::move(file))});
    }
    return output;
}

auto read_package_directory(const std::filesystem::path& path) -> result<std::vector<ownership>> {
    auto packages = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator = std::filesystem::recursive_directory_iterator{path, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (!iterator->is_regular_file()) {
            continue;
        }
        const auto extension = iterator->path().extension();
        if (extension == ".deb" || extension == ".rpm" || extension == ".apk") {
            packages.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    std::ranges::sort(packages);
    auto output = std::vector<ownership>{};
    for (const auto& package : packages) {
        auto entries = read_package(package);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        output.insert(output.end(),
                      std::make_move_iterator(entries->begin()),
                      std::make_move_iterator(entries->end()));
    }
    if (packages.empty()) {
        return std::unexpected(
            failure(errc::invalid_argument, "package inventory directory is empty", path.string()));
    }
    return output;
}

auto read_dpkg_status(const std::filesystem::path& path) -> result<std::vector<ownership>> {
    const auto info = path.parent_path() / "info";
    auto code = std::error_code{};
    if (!std::filesystem::is_directory(info, code)) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "dpkg status inventory requires the sibling info directory",
                                       info.string()));
    }
    auto lists = std::vector<std::filesystem::path>{};
    for (const auto& entry : std::filesystem::directory_iterator{info, code}) {
        if (entry.is_regular_file() && entry.path().extension() == ".list") {
            lists.push_back(entry.path());
        }
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), info.string()));
    }
    std::ranges::sort(lists);
    auto output = std::vector<ownership>{};
    for (const auto& list : lists) {
        auto package = list.stem().string();
        if (const auto colon = package.find(':'); colon != std::string::npos) {
            package.erase(colon);
        }
        auto input = std::ifstream{list};
        auto line = std::string{};
        while (std::getline(input, line)) {
            if (!line.empty() && line != "/.") {
                output.push_back({package, canonical_package_path(std::move(line))});
            }
        }
        if (!input.eof()) {
            return std::unexpected(failure(errc::io, "cannot read dpkg file list", list.string()));
        }
    }
    return output;
}

auto read_rpm_dump(const std::filesystem::path& path) -> result<std::vector<ownership>> {
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read rpm database dump", path.string()));
    }
    auto output = std::vector<ownership>{};
    auto line = std::string{};
    while (std::getline(input, line)) {
        const auto separator = line.find_first_of("\t ");
        if (separator == std::string::npos) {
            continue;
        }
        auto package = line.substr(0, separator);
        const auto file_start = line.find_first_not_of("\t ", separator);
        if (file_start == std::string::npos) {
            continue;
        }
        auto file = line.substr(file_start);
        if (!package.empty() && !file.empty() && file.front() == '/') {
            output.push_back({std::move(package), canonical_package_path(std::move(file))});
        }
    }
    if (!input.eof()) {
        return std::unexpected(failure(errc::io, "cannot read rpm database dump", path.string()));
    }
    if (output.empty()) {
        return std::unexpected(failure(errc::invalid_argument,
                                       "rpm database dump contains no package-path rows",
                                       path.string()));
    }
    return output;
}

auto read_inventory(const std::filesystem::path& path) -> result<std::vector<ownership>> {
    auto code = std::error_code{};
    if (std::filesystem::is_directory(path, code)) {
        return read_package_directory(path);
    }
    if (code || !std::filesystem::is_regular_file(path, code)) {
        return std::unexpected(
            failure(errc::not_found, "conflict inventory is absent", path.string()));
    }
    if (path.extension() == ".deb" || path.extension() == ".rpm" || path.extension() == ".apk") {
        return read_package(path);
    }
    auto bytes = read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto text = std::string_view{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    if (text.find("Package:") != std::string_view::npos &&
        text.find("Status:") != std::string_view::npos) {
        return read_dpkg_status(path);
    }
    return read_rpm_dump(path);
}

auto add_relationship(prepared_projection& replacer,
                      projection_target target,
                      std::string package,
                      std::vector<std::string> paths) -> void {
    if (!std::ranges::contains(replacer.metadata.replaces, package)) {
        replacer.metadata.replaces.push_back(package);
    }
    if (target != projection_target::apk &&
        !std::ranges::contains(replacer.metadata.conflicts, package)) {
        replacer.metadata.conflicts.push_back(package);
    }
    auto existing =
        std::ranges::find(replacer.replacements, package, &projection_replacement::package);
    if (existing == replacer.replacements.end()) {
        replacer.replacements.push_back({std::move(package), std::move(paths)});
    } else {
        existing->paths.insert(existing->paths.end(), paths.begin(), paths.end());
        std::ranges::sort(existing->paths);
        existing->paths.erase(std::unique(existing->paths.begin(), existing->paths.end()),
                              existing->paths.end());
    }
}

auto resolve(std::span<prepared_projection> prepared,
             std::span<const projection_target> targets,
             std::span<const ownership> existing) -> result<void> {
    auto owners = std::map<std::string, std::vector<std::pair<std::string, std::size_t>>>{};
    for (const auto& entry : existing) {
        owners[entry.path].push_back({entry.package, std::numeric_limits<std::size_t>::max()});
    }
    for (std::size_t index = 0; index < prepared.size(); ++index) {
        for (const auto& entry : prepared_ownership(prepared[index])) {
            owners[entry.path].push_back({entry.package, index});
        }
    }
    auto refused = std::map<std::pair<std::string, std::string>, std::vector<std::string>>{};
    for (auto& [path, path_owners] : owners) {
        std::ranges::sort(path_owners);
        path_owners.erase(std::unique(path_owners.begin(), path_owners.end()), path_owners.end());
        for (std::size_t left = 0; left < path_owners.size(); ++left) {
            for (std::size_t right = left + 1; right < path_owners.size(); ++right) {
                if (path_owners[left].first == path_owners[right].first) {
                    continue;
                }
                auto replacer = std::optional<std::size_t>{};
                const auto left_index = path_owners[left].second;
                const auto right_index = path_owners[right].second;
                if (left_index < prepared.size() &&
                    std::ranges::contains(prepared[left_index].metadata.replaces,
                                          path_owners[right].first)) {
                    replacer = left_index;
                } else if (right_index < prepared.size() &&
                           std::ranges::contains(prepared[right_index].metadata.replaces,
                                                 path_owners[left].first)) {
                    replacer = right_index;
                }
                if (replacer) {
                    const auto replaced = *replacer == left_index ? path_owners[right].first
                                                                  : path_owners[left].first;
                    add_relationship(prepared[*replacer], targets[*replacer], replaced, {path});
                    continue;
                }
                refused[{path_owners[left].first, path_owners[right].first}].push_back(path);
            }
        }
    }
    if (refused.empty()) {
        return {};
    }
    auto message = std::string{"package file conflicts:"};
    for (const auto& [packages, paths] : refused) {
        message += " " + packages.first + " and " + packages.second + " claim ";
        for (std::size_t index = 0; index < paths.size(); ++index) {
            if (index != 0) {
                message += ",";
            }
            message += paths[index];
        }
        message.push_back(';');
    }
    return std::unexpected(failure(errc::conflict, std::move(message), "projection ownership"));
}

} // namespace

result<void> resolve_projection_conflicts(std::span<prepared_projection> prepared,
                                          std::span<const projection_target> targets) {
    return resolve(prepared, targets, {});
}

runtime::task<void> resolve_projection_conflicts(prepared_projection& prepared,
                                                 projection_target target,
                                                 const std::filesystem::path& against) {
    auto inventory = co_await runtime::offload([against] { return read_inventory(against); });
    if (!inventory) {
        co_return std::unexpected(inventory.error());
    }
    if (!*inventory) {
        co_return std::unexpected(inventory->error());
    }
    auto targets = std::array{target};
    auto prepared_values = std::span{&prepared, std::size_t{1}};
    co_return resolve(prepared_values, targets, **inventory);
}

} // namespace liquid::projection_detail
