#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "liquid/graph.hpp"
#include "liquid/runtime.hpp"

namespace {

auto as_bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto as_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto fingerprint_file(liquid::query_context& context, std::vector<std::byte> arguments)
    -> liquid::runtime::task<std::vector<std::byte>> {
    auto input = co_await context.read(
        liquid::graph_input_key{liquid::graph_input_kind::working_tree_blob, as_text(arguments)});
    if (!input) {
        co_return std::unexpected(input.error());
    }
    co_return as_bytes(liquid::to_string(input->fingerprint));
}

auto source_files(const std::filesystem::path& root)
    -> liquid::result<std::vector<std::filesystem::path>> {
    auto files = std::vector<std::filesystem::path>{};
    auto error = std::error_code{};
    auto iterator = std::filesystem::recursive_directory_iterator{
        root, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        return std::unexpected(liquid::error{liquid::errc::io, error.message(), root.string()});
    }
    for (auto end = std::filesystem::recursive_directory_iterator{}; iterator != end;
         iterator.increment(error)) {
        if (error) {
            return std::unexpected(liquid::error{liquid::errc::io, error.message(), root.string()});
        }
        const auto name = iterator->path().filename().string();
        if (iterator->is_directory() &&
            (name == ".git" || name == ".liquid-build" || name.starts_with("build"))) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_regular_file()) {
            files.push_back(std::filesystem::relative(iterator->path(), root));
        }
    }
    std::ranges::sort(files);
    return files;
}

auto registry() -> liquid::result<liquid::query_registry> {
    auto output = liquid::query_registry{};
    auto added = output.add({.name = "file-fingerprint", .compute = fingerprint_file});
    if (!added) {
        return std::unexpected(added.error());
    }
    return output;
}

auto populate(const std::filesystem::path& root,
              const std::filesystem::path& state,
              const std::vector<std::filesystem::path>& files) -> liquid::runtime::task<void> {
    auto kinds = registry();
    if (!kinds) {
        co_return std::unexpected(kinds.error());
    }
    auto opened = co_await liquid::graph::open(state, std::move(*kinds), {.working_tree = root});
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    for (const auto& path : files) {
        auto value = co_await opened->demand({"file-fingerprint", as_bytes(path.generic_string())});
        if (!value) {
            co_return std::unexpected(value.error());
        }
    }
    co_return liquid::result<void>{};
}

auto validate(const std::filesystem::path& root,
              const std::filesystem::path& state,
              const std::vector<std::filesystem::path>& files)
    -> liquid::runtime::task<liquid::graph_statistics> {
    auto kinds = registry();
    if (!kinds) {
        co_return std::unexpected(kinds.error());
    }
    auto opened = co_await liquid::graph::open(state, std::move(*kinds), {.working_tree = root});
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    for (const auto& path : files) {
        auto value = co_await opened->demand({"file-fingerprint", as_bytes(path.generic_string())});
        if (!value) {
            co_return std::unexpected(value.error());
        }
    }
    co_return opened->statistics();
}

auto validate_clean(const std::filesystem::path& root, const std::filesystem::path& state)
    -> liquid::runtime::task<liquid::graph_validation_report> {
    auto kinds = registry();
    if (!kinds) {
        co_return std::unexpected(kinds.error());
    }
    auto opened = co_await liquid::graph::open(state, std::move(*kinds), {.working_tree = root});
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return co_await opened->validate({.all = true});
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: liquid-graph-measure <source-root> <state-directory>\n";
        return 2;
    }
    const auto root = std::filesystem::canonical(argv[1]);
    const auto state = std::filesystem::absolute(argv[2]);
    auto files = source_files(root);
    if (!files) {
        std::cerr << files.error().message << '\n';
        return 1;
    }
    auto populated = liquid::runtime::block_on(populate(root, state, *files));
    if (!populated) {
        std::cerr << populated.error().message << '\n';
        return 1;
    }
    const auto start = std::chrono::steady_clock::now();
    auto statistics = liquid::runtime::block_on(validate(root, state, *files));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (!statistics) {
        std::cerr << statistics.error().message << '\n';
        return 1;
    }
    std::cout << "graph cold-start validation: files=" << files->size()
              << " fingerprint_reads=" << statistics->fingerprint_reads
              << " executions=" << statistics->executions << " hits=" << statistics->hits
              << " elapsed_ms=" << elapsed.count() << '\n';
    auto clean = liquid::runtime::block_on(validate_clean(root, state));
    if (!clean) {
        std::cerr << clean.error().message << '\n';
        return 1;
    }
    std::cout << "graph clean-rebuild validation: targets=" << clean->validated
              << " divergences=" << clean->divergences << " backlog=" << clean->backlog
              << " elapsed_ms=" << clean->elapsed.count() << '\n';
    return 0;
}
