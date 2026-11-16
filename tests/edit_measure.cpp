// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/edit.hpp"
#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

struct indexed_source {
    std::string path;
    std::vector<std::byte> bytes;
    liquid::symbol_record symbols;
};

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    const auto characters =
        std::vector<char>{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto output = std::vector<std::byte>(characters.size());
    std::ranges::copy(characters, reinterpret_cast<char*>(output.data()));
    return output;
}

auto source_file(const std::filesystem::path& path) -> bool {
    const auto extension = path.extension().string();
    return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
           extension == ".h" || extension == ".hh" || extension == ".hpp" || extension == ".hxx";
}

auto language_for(const std::filesystem::path& path) -> std::string_view {
    return path.extension() == ".c" ? "c" : "c++";
}

void print_error(const liquid::error& error) {
    std::cerr << error.message << ": " << error.context << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: liquid-edit-measure <source-root> <empty-state-directory>\n";
        return 2;
    }
    const auto source_root = std::filesystem::path{argv[1]};
    const auto state_directory = std::filesystem::path{argv[2]};
    std::filesystem::remove_all(state_directory);
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    if (!module) {
        print_error(module.error());
        return 1;
    }
    auto indexed = std::vector<indexed_source>{};
    auto counts = std::map<std::string, std::uint64_t>{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator{source_root / "src"}) {
        if (!entry.is_regular_file() || !source_file(entry.path())) {
            continue;
        }
        auto bytes = read_file(entry.path());
        auto subject = liquid::hash(liquid::algorithm::blake3, bytes);
        auto encoded = module->symbols(language_for(entry.path()), bytes);
        if (!subject || !encoded) {
            print_error(subject ? encoded.error() : subject.error());
            return 1;
        }
        auto symbols = liquid::decode_frontend_symbols(*subject, *encoded);
        if (!symbols) {
            print_error(symbols.error());
            return 1;
        }
        for (const auto& occurrence : symbols->occurrences) {
            if (occurrence.role == liquid::symbol_role::reference) {
                ++counts[occurrence.symbol];
            }
        }
        indexed.push_back({std::filesystem::relative(entry.path(), source_root).generic_string(),
                           std::move(bytes),
                           std::move(*symbols)});
    }
    const auto selected =
        std::ranges::max_element(counts, {}, [](const auto& entry) { return entry.second; });
    if (selected == counts.end() || selected->second < 50) {
        std::cerr << "own-tree symbol set has fewer than 50 references\n";
        return 1;
    }
    auto repository = liquid::runtime::block_on(liquid::repository::create(state_directory));
    if (!repository) {
        print_error(repository.error());
        return 1;
    }
    auto indexed_files = std::uint64_t{};
    for (auto& source : indexed) {
        const auto has_selected =
            std::ranges::any_of(source.symbols.occurrences,
                                [&](const auto& item) { return item.symbol == selected->first; });
        if (!has_selected) {
            continue;
        }
        auto written = liquid::runtime::block_on(repository->write(source.path, source.bytes));
        if (!written) {
            print_error(written.error());
            return 1;
        }
        auto stored = liquid::runtime::block_on(repository->store_symbols(source.symbols));
        if (!stored) {
            print_error(stored.error());
            return 1;
        }
        ++indexed_files;
    }
    const auto started = std::chrono::steady_clock::now();
    auto preview = liquid::runtime::block_on(repository->preview_edit(
        liquid::rename_request{selected->first, "liquid_issue_205_measurement_name"},
        {"liquid-edit-measure", "rename", std::nullopt}));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!preview) {
        print_error(preview.error());
        return 1;
    }
    auto changes = std::uint64_t{};
    for (const auto& file : preview->changes) {
        changes += file.changes.size();
    }
    std::cout << "structural rename: symbol=" << selected->first
              << " references=" << selected->second << " indexed_files=" << indexed_files
              << " files_touched=" << preview->changes.size() << " changes=" << changes
              << " elapsed_ms=" << elapsed.count() << '\n';
    return changes >= 50 ? 0 : 1;
}
