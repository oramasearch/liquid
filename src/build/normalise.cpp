// SPDX-License-Identifier: MIT

#include "build/normalise.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <memory>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "liquid/hash.hpp"

namespace liquid::build_detail {
namespace {

constexpr auto ar_magic = std::string_view{"!<arch>\n"};
constexpr auto ar_header_size = std::size_t{60};

auto bytes_start_with(std::span<const std::byte> contents, std::string_view prefix) -> bool {
    if (contents.size() < prefix.size()) {
        return false;
    }
    return std::equal(prefix.begin(),
                      prefix.end(),
                      contents.begin(),
                      [](char left, std::byte right) {
                          return static_cast<unsigned char>(left) ==
                                 std::to_integer<unsigned char>(right);
                      });
}

auto byte_at(std::span<const std::byte> contents, std::size_t offset) -> unsigned char {
    return std::to_integer<unsigned char>(contents[offset]);
}

auto is_macho(std::span<const std::byte> contents) -> bool {
    if (contents.size() < 4) {
        return false;
    }
    const auto magic = std::array<unsigned char, 4>{byte_at(contents, 0),
                                                   byte_at(contents, 1),
                                                   byte_at(contents, 2),
                                                   byte_at(contents, 3)};
    return magic == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xce} ||
           magic == std::array<unsigned char, 4>{0xce, 0xfa, 0xed, 0xfe} ||
           magic == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xcf} ||
           magic == std::array<unsigned char, 4>{0xcf, 0xfa, 0xed, 0xfe} ||
           magic == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbe} ||
           magic == std::array<unsigned char, 4>{0xbe, 0xba, 0xfe, 0xca};
}

auto text_extension(std::string extension) -> bool {
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    constexpr auto extensions = std::array<std::string_view, 12>{".c",
                                                                 ".cc",
                                                                 ".cmake",
                                                                 ".cpp",
                                                                 ".h",
                                                                 ".hh",
                                                                 ".hpp",
                                                                 ".inc",
                                                                 ".pc",
                                                                 ".txt",
                                                                 ".in",
                                                                 ".cfg"};
    return std::ranges::find(extensions, extension) != extensions.end();
}

auto field(std::vector<std::byte>& contents, std::size_t offset, std::size_t size)
    -> std::span<std::byte> {
    return std::span<std::byte>{contents}.subspan(offset, size);
}

auto field_text(std::span<const std::byte> value) -> std::string {
    auto output = std::string{};
    output.reserve(value.size());
    for (const auto byte : value) {
        output.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    while (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }
    return output;
}

auto parse_number(std::span<const std::byte> value, int base, std::string_view name)
    -> result<std::uint64_t> {
    const auto text = field_text(value);
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output, base);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::unexpected(
            error{errc::corrupt, "invalid ar member " + std::string{name}, text});
    }
    return output;
}

auto write_number(std::span<std::byte> destination, std::uint64_t value, int base)
    -> result<void> {
    std::ranges::fill(destination, std::byte{' '});
    auto buffer = std::array<char, 32>{};
    const auto encoded = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
    if (encoded.ec != std::errc{} ||
        static_cast<std::size_t>(encoded.ptr - buffer.data()) > destination.size()) {
        return std::unexpected(error{errc::corrupt, "ar member field does not fit"});
    }
    std::transform(buffer.data(),
                   encoded.ptr,
                   destination.begin(),
                   [](char character) { return static_cast<std::byte>(character); });
    return {};
}

auto normalise_ar(std::vector<std::byte> contents) -> result<std::vector<std::byte>> {
    auto offset = ar_magic.size();
    while (offset < contents.size()) {
        if (contents.size() - offset < ar_header_size) {
            return std::unexpected(error{errc::corrupt, "truncated ar member header"});
        }
        const auto header = std::span<const std::byte>{contents}.subspan(offset, ar_header_size);
        if (byte_at(header, 58) != '`' || byte_at(header, 59) != '\n') {
            return std::unexpected(error{errc::corrupt, "invalid ar member trailer"});
        }
        auto size = parse_number(header.subspan(48, 10), 10, "size");
        auto mode = parse_number(header.subspan(40, 8), 8, "mode");
        if (!size || !mode) {
            return std::unexpected(!size ? size.error() : mode.error());
        }
        auto written = write_number(field(contents, offset + 16, 12), 0, 10);
        if (written) {
            written = write_number(field(contents, offset + 28, 6), 0, 10);
        }
        if (written) {
            written = write_number(field(contents, offset + 34, 6), 0, 10);
        }
        if (written) {
            written = write_number(field(contents, offset + 40, 8),
                                   normalised_output_mode(static_cast<std::uint32_t>(*mode)),
                                   8);
        }
        if (!written) {
            return std::unexpected(written.error());
        }
        offset += ar_header_size;
        if (*size > contents.size() - offset) {
            return std::unexpected(error{errc::corrupt, "truncated ar member contents"});
        }
        offset += static_cast<std::size_t>(*size);
        if ((offset & 1U) != 0U) {
            ++offset;
        }
    }
    if (offset != contents.size()) {
        return std::unexpected(error{errc::corrupt, "invalid ar archive padding"});
    }
    return contents;
}

auto normalise_pyc(std::vector<std::byte> contents) -> result<std::vector<std::byte>> {
    constexpr auto header_size = std::size_t{16};
    if (contents.size() < header_size) {
        return std::unexpected(error{errc::corrupt, "truncated Python bytecode header"});
    }
    contents[4] = std::byte{1};
    contents[5] = std::byte{};
    contents[6] = std::byte{};
    contents[7] = std::byte{};
    auto payload = std::span<const std::byte>{contents}.subspan(header_size);
    auto payload_hash = hash(algorithm::blake3, payload);
    if (!payload_hash) {
        return std::unexpected(payload_hash.error());
    }
    std::copy_n(payload_hash->bytes.begin(), 8, contents.begin() + 8);
    return contents;
}

#if !defined(_WIN32)

// Owns one POSIX descriptor and closes it on scope exit.
class descriptor {
public:
    descriptor() = default;
    explicit descriptor(int value) : value_(value) {}
    descriptor(descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    auto operator=(descriptor&& other) noexcept -> descriptor& {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    descriptor(const descriptor&) = delete;
    auto operator=(const descriptor&) -> descriptor& = delete;
    ~descriptor() {
        reset();
    }

    [[nodiscard]] auto get() const -> int {
        return value_;
    }
    [[nodiscard]] auto release() -> int {
        return std::exchange(value_, -1);
    }
    [[nodiscard]] explicit operator bool() const {
        return value_ >= 0;
    }

private:
    void reset() {
        if (value_ >= 0) {
            ::close(value_);
            value_ = -1;
        }
    }

    int value_ = -1;
};

enum class output_node : std::uint8_t {
    missing,
    file,
    directory,
};

auto system_failure(std::string_view message, const std::filesystem::path& path) -> error {
    return error{errc::io, std::string{message} + ": " + std::strerror(errno), path.string()};
}

auto symbolic_link(const std::filesystem::path& path) -> error {
    return error{errc::unauthorized, "action output is a symbolic link", path.string()};
}

// Opens `name` below the directory `parent` without following a link and reports what it is.
// The node is classified before it is opened and the opened descriptor is checked against that
// classification, so a link, a special file, or a node swapped in between is refused unread.
auto open_child(int parent,
                const std::string& name,
                const std::filesystem::path& path,
                descriptor& opened) -> result<output_node> {
    struct stat before{};
    if (::fstatat(parent, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return output_node::missing;
        }
        return std::unexpected(system_failure("cannot inspect action output", path));
    }
    if (S_ISLNK(before.st_mode)) {
        return std::unexpected(symbolic_link(path));
    }
    if (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)) {
        return std::unexpected(error{
            errc::unauthorized, "action output is not a regular file or directory", path.string()});
    }
    const auto directory = S_ISDIR(before.st_mode);
    const auto flags =
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (directory ? O_DIRECTORY : 0);
    const auto value = ::openat(parent, name.c_str(), flags);
    if (value < 0) {
        if (errno == ELOOP) {
            return std::unexpected(symbolic_link(path));
        }
        return std::unexpected(system_failure("cannot open action output", path));
    }
    opened = descriptor{value};
    struct stat after{};
    if (::fstat(opened.get(), &after) != 0) {
        return std::unexpected(system_failure("cannot inspect action output", path));
    }
    if (after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        (after.st_mode & S_IFMT) != (before.st_mode & S_IFMT)) {
        return std::unexpected(
            error{errc::conflict, "action output changed while it was collected", path.string()});
    }
    return directory ? output_node::directory : output_node::file;
}

auto read_descriptor(int file, const std::filesystem::path& path)
    -> result<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    auto buffer = std::vector<std::byte>(64U * 1024U);
    while (true) {
        const auto count = ::read(file, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(system_failure("cannot read action output", path));
        }
        if (count == 0) {
            return output;
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + count);
    }
}

// Writes `contents` to a new private file in `staging`; the name is unique and never reused.
auto write_staged(const std::filesystem::path& staging, std::span<const std::byte> contents)
    -> result<std::filesystem::path> {
    auto name = (staging / "output-XXXXXX").string();
    auto file = descriptor{::mkostemp(name.data(), O_CLOEXEC)};
    if (!file) {
        return std::unexpected(system_failure("cannot stage action output", staging));
    }
    auto remaining = contents;
    while (!remaining.empty()) {
        const auto count = ::write(file.get(), remaining.data(), remaining.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(system_failure("cannot stage action output", name));
        }
        remaining = remaining.subspan(static_cast<std::size_t>(count));
    }
    if (::close(file.release()) != 0) {
        return std::unexpected(system_failure("cannot stage action output", name));
    }
    return std::filesystem::path{name};
}

auto stage_file(int file,
                std::string relative,
                const std::filesystem::path& path,
                const std::filesystem::path& staging,
                bool normalise) -> result<staged_output> {
    struct stat status{};
    if (::fstat(file, &status) != 0) {
        return std::unexpected(system_failure("cannot inspect action output", path));
    }
    auto contents = read_descriptor(file, path);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    if (normalise) {
        const auto kind = output_kind_for(std::filesystem::path{relative}, *contents);
        contents = normalise_output(kind, std::move(*contents));
        if (!contents) {
            return std::unexpected(contents.error());
        }
    }
    auto staged = write_staged(staging, *contents);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    return staged_output{.path = std::move(relative),
                         .mode = static_cast<std::uint32_t>(status.st_mode) & 0777U,
                         .staged = std::move(*staged)};
}

auto stage_directory(int directory,
                     const std::string& relative,
                     const std::filesystem::path& path,
                     const std::filesystem::path& staging,
                     bool normalise,
                     std::vector<staged_output>& output) -> result<void> {
    auto listing = descriptor{::fcntl(directory, F_DUPFD_CLOEXEC, 0)};
    if (!listing) {
        return std::unexpected(system_failure("cannot list action output", path));
    }
    const auto stream =
        std::unique_ptr<DIR, decltype(&::closedir)>{::fdopendir(listing.get()), &::closedir};
    if (!stream) {
        return std::unexpected(system_failure("cannot list action output", path));
    }
    static_cast<void>(listing.release());
    auto names = std::vector<std::string>{};
    while (true) {
        errno = 0;
        const auto* entry = ::readdir(stream.get());
        if (entry == nullptr) {
            if (errno != 0) {
                return std::unexpected(system_failure("cannot list action output", path));
            }
            break;
        }
        const auto name = std::string_view{entry->d_name};
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
    }
    std::ranges::sort(names);
    for (const auto& name : names) {
        const auto child_path = path / name;
        auto child_relative = relative + "/" + name;
        auto child = descriptor{};
        auto node = open_child(directory, name, child_path, child);
        if (!node) {
            return std::unexpected(node.error());
        }
        if (*node == output_node::missing) {
            return std::unexpected(error{errc::conflict,
                                         "action output changed while it was collected",
                                         child_path.string()});
        }
        if (*node == output_node::directory) {
            auto nested = stage_directory(
                child.get(), child_relative, child_path, staging, normalise, output);
            if (!nested) {
                return std::unexpected(nested.error());
            }
            continue;
        }
        auto staged =
            stage_file(child.get(), std::move(child_relative), child_path, staging, normalise);
        if (!staged) {
            return std::unexpected(staged.error());
        }
        output.push_back(std::move(*staged));
    }
    return {};
}

#endif

} // namespace

auto output_kind_for(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> output_kind {
    if (bytes_start_with(contents, ar_magic)) {
        return output_kind::ar_archive;
    }
    if (bytes_start_with(contents, std::string_view{"\x7f" "ELF", 4})) {
        return output_kind::elf_object;
    }
    if (is_macho(contents)) {
        return output_kind::macho_object;
    }
    if (bytes_start_with(contents, "MZ")) {
        return output_kind::pe_object;
    }
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension == ".pyc") {
        return output_kind::python_bytecode;
    }
    if (text_extension(extension) || path.filename().string().ends_with("Config.cmake")) {
        return output_kind::text;
    }
    return output_kind::opaque;
}

auto normalise_output(output_kind kind, std::vector<std::byte> contents)
    -> result<std::vector<std::byte>> {
    if (kind == output_kind::ar_archive) {
        return normalise_ar(std::move(contents));
    }
    if (kind == output_kind::python_bytecode) {
        return normalise_pyc(std::move(contents));
    }
    return contents;
}

auto stage_output(const std::filesystem::path& root,
                  std::string_view relative,
                  output_shape shape,
                  const std::filesystem::path& staging,
                  bool normalise) -> result<std::vector<staged_output>> {
    const auto declared = std::filesystem::path{relative};
    auto names = std::vector<std::string>{};
    for (const auto& component : declared) {
        names.push_back(component.string());
    }
    const auto valid = !names.empty() && declared.is_relative() &&
                       std::ranges::none_of(names, [](const std::string& name) {
                           return name.empty() || name == "." || name == ".." ||
                                  name.find('/') != std::string::npos;
                       });
    if (!valid) {
        return std::unexpected(error{errc::invalid_argument,
                                     "action output must be a normalized relative path",
                                     std::string{relative}});
    }
#if defined(_WIN32)
    static_cast<void>(root);
    static_cast<void>(shape);
    static_cast<void>(staging);
    static_cast<void>(normalise);
    return std::unexpected(error{errc::unsupported,
                                 "action output collection requires POSIX descriptors",
                                 std::string{relative}});
#else
    auto current =
        descriptor{::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!current) {
        return std::unexpected(system_failure("cannot open action output root", root));
    }
    auto path = root;
    auto node = output_node::directory;
    for (auto index = std::size_t{}; index < names.size(); ++index) {
        path /= names[index];
        auto child = descriptor{};
        auto opened = open_child(current.get(), names[index], path, child);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        node = *opened;
        if (node == output_node::missing ||
            (index + 1 < names.size() && node != output_node::directory)) {
            return std::vector<staged_output>{};
        }
        current = std::move(child);
    }
    auto output = std::vector<staged_output>{};
    const auto generic = declared.generic_string();
    if (node == output_node::directory) {
        if (shape == output_shape::file) {
            return output;
        }
        auto staged = stage_directory(current.get(), generic, path, staging, normalise, output);
        if (!staged) {
            return std::unexpected(staged.error());
        }
        return output;
    }
    if (shape == output_shape::directory) {
        return std::unexpected(
            error{errc::io, "declared output directory is not a directory", path.string()});
    }
    auto staged = stage_file(current.get(), generic, path, staging, normalise);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    output.push_back(std::move(*staged));
    return output;
#endif
}

auto normalised_output_mode(std::uint32_t mode) -> std::uint32_t {
    return (mode & 0111U) != 0U ? 0755U : 0644U;
}

auto output_normaliser_rules() -> std::vector<std::string> {
    return {"tree-mode-v1", "gnu-ar-d-v1", "python-hash-pyc-v1", "stable-path-binary-v1",
            "source-date-epoch-text-v1"};
}

} // namespace liquid::build_detail
