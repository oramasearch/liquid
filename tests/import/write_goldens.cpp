#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "liquid/import.hpp"
#include "liquid/runtime.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 3) {
        std::cerr << "usage: liquid-spack-write-goldens <source-root> <work-directory>\n";
        return 2;
    }
    const auto source_root = std::filesystem::path{argv[1]};
    const auto work_directory = std::filesystem::path{argv[2]};
    const auto fixture = source_root / "tests/import/fixtures/spack-20.json";
    const auto golden = source_root / "tests/import/fixtures/golden";
    auto code = std::error_code{};
    std::filesystem::remove_all(work_directory, code);
    if (code) {
        std::cerr << "cannot clear work directory: " << code.message() << '\n';
        return 1;
    }
    auto options = liquid::spack_import_options{
        .patch_contents = {{"https://example.invalid/zlib-relocation.patch",
                            bytes("diff --git a/zlib.c b/zlib.c\n")}},
    };
    auto imported = liquid::runtime::block_on(
        liquid::import_spack(fixture, work_directory, std::move(options)));
    if (!imported) {
        std::cerr << "import failed: " << imported.error().message << '\n';
        return 1;
    }
    std::filesystem::remove_all(golden, code);
    if (code) {
        std::cerr << "cannot clear golden directory: " << code.message() << '\n';
        return 1;
    }
    std::filesystem::create_directories(golden, code);
    std::filesystem::copy(work_directory / "packages",
                          golden / "packages",
                          std::filesystem::copy_options::recursive,
                          code);
    if (code) {
        std::cerr << "cannot copy golden packages: " << code.message() << '\n';
        return 1;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{golden / "packages"}) {
        if (entry.is_regular_file() && entry.path().extension() == ".cbor") {
            std::filesystem::remove(entry.path(), code);
            if (code) {
                std::cerr << "cannot remove generated package record: " << code.message() << '\n';
                return 1;
            }
        }
    }
    std::filesystem::copy_file(work_directory / "import-report.json",
                               golden / "import-report.json",
                               std::filesystem::copy_options::overwrite_existing,
                               code);
    if (code) {
        std::cerr << "cannot copy golden report: " << code.message() << '\n';
        return 1;
    }
    return 0;
}
