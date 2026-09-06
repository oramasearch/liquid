#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    const auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

auto fail(const liquid::error& error) -> int {
    std::cerr << error.message << " (" << error.context << ")\n";
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: format-diff-fixture <repository> <fixtures>\n";
        return 2;
    }
    const auto fixtures = std::filesystem::path{argv[2]};
    auto created = liquid::runtime::block_on(liquid::repository::create(argv[1]));
    if (!created) {
        return fail(created.error());
    }
    auto repository = std::move(*created);
    auto written = liquid::runtime::block_on(
        repository.write("archive.zip", read_file(fixtures / "archive-a.zip")));
    if (!written) {
        return fail(written.error());
    }
    written = liquid::runtime::block_on(
        repository.write("measurements.h5", read_file(fixtures / "sample-a.h5")));
    if (!written) {
        return fail(written.error());
    }
    auto before = liquid::runtime::block_on(repository.cut("before"));
    if (!before) {
        return fail(before.error());
    }
    auto referenced = liquid::runtime::block_on(repository.set_ref("before", *before));
    if (!referenced) {
        return fail(referenced.error());
    }
    written = liquid::runtime::block_on(
        repository.write("archive.zip", read_file(fixtures / "archive-b.zip")));
    if (!written) {
        return fail(written.error());
    }
    written = liquid::runtime::block_on(
        repository.write("measurements.h5", read_file(fixtures / "sample-b.h5")));
    if (!written) {
        return fail(written.error());
    }
    auto after = liquid::runtime::block_on(repository.cut("after"));
    if (!after) {
        return fail(after.error());
    }
    referenced = liquid::runtime::block_on(repository.set_ref("after", *after));
    if (!referenced) {
        return fail(referenced.error());
    }
    return 0;
}
