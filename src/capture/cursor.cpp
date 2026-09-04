#include "capture/cursor.hpp"

#include <fstream>
#include <system_error>

namespace liquid::capture_detail {
namespace {

auto io_error(std::string message, const std::filesystem::path& path) -> error {
    return {errc::io, std::move(message), path.string()};
}

} // namespace

auto load_cursor(const std::filesystem::path& path) -> result<capture_cursor> {
    auto input = std::ifstream{path};
    if (!input) {
        if (!std::filesystem::exists(path))
            return capture_cursor{};
        return std::unexpected(io_error("cannot open capture cursor", path));
    }
    auto value = capture_cursor{};
    input >> value.journal.offset >> value.platform_token;
    if (!input)
        return std::unexpected(error{errc::corrupt, "invalid capture cursor", path.string()});
    return value;
}

auto save_cursor(const std::filesystem::path& path, capture_cursor value) -> result<void> {
    auto temporary = path;
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::trunc};
        if (!output)
            return std::unexpected(io_error("cannot write capture cursor", temporary));
        output << value.journal.offset << ' ' << value.platform_token << '\n';
        output.flush();
        if (!output)
            return std::unexpected(io_error("cannot flush capture cursor", temporary));
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

} // namespace liquid::capture_detail
