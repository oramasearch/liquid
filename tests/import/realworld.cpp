#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "build/repository_access.hpp"
#include "liquid/configuration.hpp"
#include "liquid/hash.hpp"
#include "liquid/import.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "vfs/repository_access.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto read_text(const std::filesystem::path& path) -> liquid::result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read imported document", path.string()});
    }
    return std::string{std::istreambuf_iterator<char>{input}, {}};
}

auto run(const std::filesystem::path& root) -> liquid::runtime::task<void> {
    auto repository = co_await liquid::repository::create(root);
    if (!repository) {
        co_return std::unexpected(repository.error());
    }
    auto written = co_await repository->write_file("bin/sh", LIQUID_BUILD_FIXTURE);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto mode =
        co_await liquid::repository_detail::vfs_access::set_mode(*repository, "bin/sh", 0100755);
    if (!mode) {
        co_return std::unexpected(mode.error());
    }
    auto cut = co_await repository->cut("Spack build subset toolchain");
    if (!cut) {
        co_return std::unexpected(cut.error());
    }
    auto toolchain = co_await repository->root_digest();
    if (!toolchain) {
        co_return std::unexpected(toolchain.error());
    }
    const auto names = std::vector<std::string>{"zlib",
                                                "bzip2",
                                                "xz",
                                                "expat",
                                                "libffi",
                                                "pcre2",
                                                "libpng",
                                                "sqlite",
                                                "ncurses",
                                                "readline"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto source = names[index] + "-1." + std::to_string(index + 1) + ".0";
        auto stored = co_await liquid::repository_detail::build_access::put_record(
            *repository, bytes(source), liquid::algorithm::sha256);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    auto options = liquid::spack_import_options{
        .patch_contents = {{"https://example.invalid/zlib-relocation.patch",
                            bytes("diff --git a/zlib.c b/zlib.c\n")}},
        .toolchain = *toolchain,
    };
    const auto fixture =
        std::filesystem::path{LIQUID_SOURCE_ROOT} / "tests/import/fixtures/spack-20.json";
    const auto universe_path = root / "universe";
    auto imported = co_await liquid::import_spack(fixture, universe_path, std::move(options));
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    for (const auto& name : names) {
        const auto document_path =
            universe_path / "packages" / name.substr(0, 1) / (name + ".liquid");
        auto document_text = read_text(document_path);
        if (!document_text) {
            co_return std::unexpected(document_text.error());
        }
        auto document = liquid::parse_configuration(*document_text, document_path);
        if (!document) {
            co_return std::unexpected(document.error());
        }
        auto configuration = liquid::lower(*document);
        if (!configuration) {
            co_return std::unexpected(configuration.error());
        }
        auto result = co_await liquid::build_configuration(*repository, *configuration, name);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        std::cout << "built " << name << " result=" << liquid::to_string(result->value)
                  << " relocation=passed\n";
    }
    co_return liquid::result<void>{};
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        std::cerr << "usage: liquid-spack-realworld <work-directory>\n";
        return 2;
    }
    const auto root = std::filesystem::path{argv[1]};
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    if (code) {
        std::cerr << "cannot clear work directory: " << code.message() << '\n';
        return 1;
    }
    auto result = liquid::runtime::block_on(run(root));
    if (!result) {
        std::cerr << "error: " << result.error().message << " (" << result.error().context << ")\n";
        return 1;
    }
    return 0;
}
