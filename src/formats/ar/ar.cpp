#include "formats/ar/ar.hpp"

#include <charconv>
#include <string_view>

namespace liquid::formats::ar {
namespace {

auto failure(std::string message) -> error {
    return {errc::corrupt, std::move(message), "ar"};
}

auto trim(std::string_view value) -> std::string_view {
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
    }
    return value;
}

auto decimal(std::string_view value) -> result<std::size_t> {
    value = trim(value);
    auto output = std::size_t{};
    const auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), output);
    if (code != std::errc{} || end != value.data() + value.size()) {
        return std::unexpected(failure("invalid member size"));
    }
    return output;
}

auto name_from_table(std::string_view table, std::string_view name) -> result<std::string> {
    auto offset = std::size_t{};
    const auto [end, code] = std::from_chars(name.data() + 1, name.data() + name.size(), offset);
    if (code != std::errc{} || end != name.data() + name.size() || offset >= table.size()) {
        return std::unexpected(failure("invalid GNU ar member name"));
    }
    auto length = table.find("/\n", offset);
    if (length == std::string_view::npos) {
        length = table.find('\n', offset);
    }
    if (length == std::string_view::npos) {
        return std::unexpected(failure("unterminated GNU ar member name"));
    }
    return std::string{table.substr(offset, length - offset)};
}

} // namespace

auto read(std::span<const std::byte> input) -> result<std::vector<member>> {
    constexpr auto magic = std::string_view{"!<arch>\n"};
    if (input.size() < magic.size() ||
        std::string_view{reinterpret_cast<const char*>(input.data()), magic.size()} != magic) {
        return std::unexpected(failure("invalid global header"));
    }
    auto output = std::vector<member>{};
    auto long_names = std::string_view{};
    auto offset = magic.size();
    while (offset < input.size()) {
        if (input.size() - offset < 60) {
            return std::unexpected(failure("truncated member header"));
        }
        const auto header = input.subspan(offset, 60);
        if (header[58] != std::byte{'`'} || header[59] != std::byte{'\n'}) {
            return std::unexpected(failure("invalid member trailer"));
        }
        auto name = trim(std::string_view{reinterpret_cast<const char*>(header.data()), 16});
        auto size =
            decimal(std::string_view{reinterpret_cast<const char*>(header.data() + 48), 10});
        if (!size || *size > input.size() - offset - 60) {
            return std::unexpected(size ? failure("member exceeds archive") : size.error());
        }
        const auto body = input.subspan(offset + 60, *size);
        auto resolved = std::string{};
        if (name == "//") {
            long_names = std::string_view{reinterpret_cast<const char*>(body.data()), body.size()};
        } else if (name.starts_with("#1/")) {
            auto length = decimal(name.substr(3));
            if (!length || *length > body.size()) {
                return std::unexpected(length ? failure("invalid BSD ar member name")
                                              : length.error());
            }
            resolved.assign(reinterpret_cast<const char*>(body.data()), *length);
            output.push_back({std::move(resolved),
                              {body.begin() + static_cast<std::ptrdiff_t>(*length), body.end()}});
        } else if (name != "/") {
            if (name.starts_with('/') && name.size() > 1 && name[1] >= '0' && name[1] <= '9') {
                auto table_name = name_from_table(long_names, name);
                if (!table_name) {
                    return std::unexpected(table_name.error());
                }
                resolved = std::move(*table_name);
            } else {
                if (name.ends_with('/')) {
                    name.remove_suffix(1);
                }
                resolved = std::string{name};
            }
            output.push_back({std::move(resolved), {body.begin(), body.end()}});
        }
        offset += 60 + *size;
        if ((*size & 1U) != 0) {
            if (offset == input.size()) {
                return std::unexpected(failure("missing member padding"));
            }
            ++offset;
        }
    }
    return output;
}

} // namespace liquid::formats::ar
