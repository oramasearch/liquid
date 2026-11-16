#include "objects/entry_name.hpp"

#include <cstdint>

namespace liquid::objects {
namespace {

auto lower(char value) -> char {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

auto equals_ignoring_case(std::string_view left, std::string_view right) -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (auto index = std::size_t{}; index < left.size(); ++index) {
        if (lower(left[index]) != lower(right[index])) {
            return false;
        }
    }
    return true;
}

// Code points HFS+ ignores when comparing names, as listed by Git's `is_hfs_dotgit`.
auto hfs_ignorable(std::uint32_t code_point) -> bool {
    return (code_point >= 0x200C && code_point <= 0x200F) ||
           (code_point >= 0x202A && code_point <= 0x202E) ||
           (code_point >= 0x206A && code_point <= 0x206F) || code_point == 0xFEFF;
}

// The name with HFS+ ignorable code points removed. A name that is not valid UTF-8 is returned
// unchanged: HFS+ refuses it, and the byte comparison still applies.
auto without_hfs_ignorables(std::string_view name) -> std::string {
    auto output = std::string{};
    for (auto index = std::size_t{}; index < name.size();) {
        const auto lead = static_cast<unsigned char>(name[index]);
        auto length = std::size_t{1};
        auto code_point = std::uint32_t{lead};
        if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code_point = lead & 0x07U;
        } else if (lead >= 0xE0) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if (lead >= 0xC2 && lead < 0xE0) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if (lead >= 0x80) {
            return std::string{name};
        }
        if (index + length > name.size()) {
            return std::string{name};
        }
        for (auto next = std::size_t{1}; next < length; ++next) {
            const auto continuation = static_cast<unsigned char>(name[index + next]);
            if ((continuation & 0xC0U) != 0x80U) {
                return std::string{name};
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (!hfs_ignorable(code_point)) {
            output.append(name.substr(index, length));
        }
        index += length;
    }
    return output;
}

// Windows strips trailing dots and spaces, treats `:` as the start of a stream name, and gives
// `.git` the 8.3 short name `GIT~1`.
auto ntfs_dot_git(std::string_view name) -> bool {
    if (equals_ignoring_case(name, "git~1")) {
        return true;
    }
    if (name.size() < 4 || !equals_ignoring_case(name.substr(0, 4), ".git")) {
        return false;
    }
    const auto rest = name.substr(4);
    if (rest.starts_with(':')) {
        return true;
    }
    return rest.find_first_not_of(". ") == std::string_view::npos;
}

} // namespace

auto entry_name_problem(std::string_view name) -> std::optional<std::string_view> {
    if (name.empty()) {
        return "tree entry name is empty";
    }
    if (name == "." || name == "..") {
        return "tree entry name is a relative directory reference";
    }
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
        return "tree entry name contains a path separator";
    }
    if (name.find('\0') != std::string_view::npos) {
        return "tree entry name contains NUL";
    }
    if (ntfs_dot_git(name) || equals_ignoring_case(without_hfs_ignorables(name), ".git")) {
        return "tree entry name is reserved for Git";
    }
    return std::nullopt;
}

auto validate_entry_name(std::string_view name, errc code) -> result<void> {
    if (const auto problem = entry_name_problem(name)) {
        return std::unexpected(error{code, std::string{*problem}, printable_entry_name(name)});
    }
    return {};
}

auto printable_entry_name(std::string_view name) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto item : name) {
        const auto byte = static_cast<unsigned char>(item);
        if (byte < 0x20 || byte >= 0x7F) {
            output += "\\x";
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0FU]);
        } else {
            output.push_back(item);
        }
    }
    return output;
}

} // namespace liquid::objects
