#include "capture/reconcile.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <system_error>

namespace liquid::capture_detail {
namespace {

auto io_error(std::string message, const std::filesystem::path& path) -> error {
    return {errc::io, std::move(message), path.string()};
}

auto ticks(std::filesystem::file_time_type value) -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

auto from_ticks(std::int64_t value) -> std::filesystem::file_time_type {
    return std::filesystem::file_time_type{std::chrono::nanoseconds{value}};
}

} // namespace

auto load_workspace_state(const std::filesystem::path& path) -> result<workspace_state> {
    auto input = std::ifstream{path};
    if (!input) {
        if (!std::filesystem::exists(path))
            return workspace_state{};
        return std::unexpected(io_error("cannot open capture state", path));
    }
    auto output = workspace_state{};
    std::string encoded;
    std::uintmax_t size{};
    std::int64_t modified{};
    while (input >> std::quoted(encoded) >> size >> modified)
        output.emplace(std::filesystem::path{encoded}, file_state{size, from_ticks(modified)});
    if (!input.eof())
        return std::unexpected(error{errc::corrupt, "invalid capture state", path.string()});
    return output;
}

auto save_workspace_state(const std::filesystem::path& path, const workspace_state& state)
    -> result<void> {
    auto temporary = path;
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::trunc};
        if (!output)
            return std::unexpected(io_error("cannot write capture state", temporary));
        for (const auto& [file, value] : state)
            output << std::quoted(file.generic_string()) << ' ' << value.size << ' '
                   << ticks(value.modified) << '\n';
        output.flush();
        if (!output)
            return std::unexpected(io_error("cannot flush capture state", temporary));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code)
        return std::unexpected(io_error(code.message(), path));
    return {};
}

auto reconcile(const std::filesystem::path& workspace,
               const workspace_state& previous,
               const std::function<bool(const std::filesystem::path&)>& excluded)
    -> result<reconcile_result> {
    auto output = reconcile_result{};
    auto code = std::error_code{};
    for (std::filesystem::recursive_directory_iterator
             it(workspace, std::filesystem::directory_options::skip_permission_denied, code),
         end;
         !code && it != end;
         it.increment(code)) {
        const auto& entry = *it;
        const auto relative = entry.path().lexically_relative(workspace);
        if (excluded(relative)) {
            if (entry.is_directory(code))
                it.disable_recursion_pending();
            code.clear();
            continue;
        }
        if (!entry.is_regular_file(code)) {
            code.clear();
            continue;
        }
        const auto size = entry.file_size(code);
        if (code)
            continue;
        const auto modified = entry.last_write_time(code);
        if (code)
            continue;
        const auto current = file_state{size, modified};
        output.state.emplace(relative, current);
        const auto old = previous.find(relative);
        if (old == previous.end() || old->second.size != current.size ||
            old->second.modified != current.modified)
            output.changes.push_back({watch_event_kind::write, relative, 0});
    }
    if (code)
        return std::unexpected(io_error(code.message(), workspace));
    for (const auto& [file, value] : previous) {
        if (!output.state.contains(file))
            output.changes.push_back({watch_event_kind::remove, file, 0});
    }
    return output;
}

} // namespace liquid::capture_detail
