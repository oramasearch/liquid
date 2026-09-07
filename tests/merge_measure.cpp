#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "liquid/frontend.hpp"
#include "liquid/git.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "repository/merge.hpp"

namespace {

auto key(const liquid::change_id& id) -> std::string {
    return liquid::to_string(id.value);
}

auto common_ancestor(const std::map<std::string, liquid::change>& changes,
                     liquid::change_id left,
                     liquid::change_id right) -> std::optional<liquid::change_id> {
    auto left_ancestors = std::set<std::string>{};
    auto pending = std::vector<liquid::change_id>{left};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto current_key = key(current);
        if (!left_ancestors.insert(current_key).second) {
            continue;
        }
        const auto found = changes.find(current_key);
        if (found == changes.end()) {
            continue;
        }
        for (const auto& parent : found->second.parents) {
            pending.push_back(parent.id);
        }
    }
    pending.push_back(right);
    auto visited = std::set<std::string>{};
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.erase(pending.begin());
        const auto current_key = key(current);
        if (!visited.insert(current_key).second) {
            continue;
        }
        if (left_ancestors.contains(current_key)) {
            return current;
        }
        const auto found = changes.find(current_key);
        if (found == changes.end()) {
            continue;
        }
        for (const auto& parent : found->second.parents) {
            pending.push_back(parent.id);
        }
    }
    return std::nullopt;
}

auto parse(liquid::frontend_graph& graph,
           std::string language,
           const std::vector<std::byte>& source) -> liquid::result<liquid::parse_tree> {
    auto id = liquid::hash(liquid::algorithm::blake3, source);
    if (!id) {
        return std::unexpected(id.error());
    }
    return liquid::runtime::block_on(
        graph.parse({.language = std::move(language), .blob = *id, .source = source}));
}

auto symbols(liquid::frontend_graph& graph,
             std::string language,
             std::string path,
             const std::vector<std::byte>& source) -> liquid::result<liquid::symbol_record> {
    return liquid::runtime::block_on(graph.symbols(
        {.language = std::move(language), .source_path = std::move(path), .source = source}));
}

auto structural_fallback(const liquid::error& value) -> bool {
    return value.code == liquid::errc::unsupported || value.code == liquid::errc::not_found;
}

using edit_map = std::map<std::string, liquid::tree_diff_entry>;

auto edits_from(liquid::repository& repository, liquid::change_id base, liquid::change_id side)
    -> liquid::result<edit_map> {
    auto differences = liquid::runtime::block_on(repository.diff(base, side));
    if (!differences) {
        return std::unexpected(differences.error());
    }
    auto output = edit_map{};
    for (auto& difference : *differences) {
        const auto found = output.find(difference.path);
        if (found == output.end()) {
            output.emplace(difference.path, std::move(difference));
            continue;
        }
        if (difference.old_blob) {
            found->second.old_blob = difference.old_blob;
        }
        if (difference.new_blob) {
            found->second.new_blob = difference.new_blob;
        }
    }
    return output;
}

auto same_blob(const std::optional<liquid::digest>& left,
               const std::optional<liquid::digest>& right) -> bool {
    if (!left || !right) {
        return left.has_value() == right.has_value();
    }
    return left->alg == right->alg && left->bytes == right->bytes;
}

struct counts {
    std::uint64_t merges{};
    std::uint64_t identical{};
    std::uint64_t different{};
    std::uint64_t conflict{};
    std::uint64_t suspicious{};
};

auto measure_merge(liquid::repository& repository,
                   liquid::frontend_graph& graph,
                   const std::map<std::string, liquid::change>& changes,
                   const liquid::change& human) -> liquid::result<std::string> {
    const auto base = common_ancestor(changes, human.parents[0].id, human.parents[1].id);
    if (!base) {
        return std::string{"conflict"};
    }
    auto left_edits = edits_from(repository, *base, human.parents[0].id);
    auto right_edits = edits_from(repository, *base, human.parents[1].id);
    auto human_edits = edits_from(repository, *base, human.id);
    if (!left_edits || !right_edits || !human_edits) {
        return std::unexpected(!left_edits    ? left_edits.error()
                               : !right_edits ? right_edits.error()
                                              : human_edits.error());
    }
    auto paths = std::set<std::string>{};
    const auto append_paths = [&](const edit_map& edits) {
        for (const auto& [path, value] : edits) {
            static_cast<void>(value);
            paths.insert(path);
        }
    };
    append_paths(*left_edits);
    append_paths(*right_edits);
    append_paths(*human_edits);
    auto classification = std::string{"identical"};
    for (const auto& path : paths) {
        const auto find = [&](const edit_map& edits) -> const liquid::tree_diff_entry* {
            const auto found = edits.find(path);
            return found == edits.end() ? nullptr : &found->second;
        };
        const auto* left_edit = find(*left_edits);
        const auto* right_edit = find(*right_edits);
        const auto* human_edit = find(*human_edits);
        auto base_blob = std::optional<liquid::digest>{};
        for (const auto* edit : {left_edit, right_edit, human_edit}) {
            if (edit && edit->old_blob) {
                base_blob = edit->old_blob;
                break;
            }
        }
        const auto current_blob = [&](const liquid::tree_diff_entry* edit) {
            return edit ? edit->new_blob : base_blob;
        };
        const auto left_blob = current_blob(left_edit);
        const auto right_blob = current_blob(right_edit);
        const auto human_blob = current_blob(human_edit);
        auto selected = std::optional<liquid::digest>{};
        if (same_blob(left_blob, right_blob)) {
            selected = left_blob;
        } else if (same_blob(left_blob, base_blob)) {
            selected = right_blob;
        } else if (same_blob(right_blob, base_blob)) {
            selected = left_blob;
        } else if (!base_blob || !left_blob || !right_blob) {
            return std::string{"conflict"};
        } else {
            auto language = graph.language_for_path(path);
            if (!language) {
                return std::string{"conflict"};
            }
            auto base_source = liquid::runtime::block_on(repository.blob(*base_blob));
            auto left_source = liquid::runtime::block_on(repository.blob(*left_blob));
            auto right_source = liquid::runtime::block_on(repository.blob(*right_blob));
            if (!base_source || !left_source || !right_source) {
                return std::unexpected(!base_source   ? base_source.error()
                                       : !left_source ? left_source.error()
                                                      : right_source.error());
            }
            auto base_tree = parse(graph, language->language, *base_source);
            auto left_tree = parse(graph, language->language, *left_source);
            auto right_tree = parse(graph, language->language, *right_source);
            if (!base_tree || !left_tree || !right_tree) {
                const auto& failure = !base_tree   ? base_tree.error()
                                      : !left_tree ? left_tree.error()
                                                   : right_tree.error();
                if (structural_fallback(failure)) {
                    return std::string{"conflict"};
                }
                return std::unexpected(!base_tree   ? base_tree.error()
                                       : !left_tree ? left_tree.error()
                                                    : right_tree.error());
            }
            auto base_symbols = symbols(graph, language->language, path, *base_source);
            auto left_symbols = symbols(graph, language->language, path, *left_source);
            auto right_symbols = symbols(graph, language->language, path, *right_source);
            if (!base_symbols || !left_symbols || !right_symbols) {
                const auto& failure = !base_symbols   ? base_symbols.error()
                                      : !left_symbols ? left_symbols.error()
                                                      : right_symbols.error();
                if (structural_fallback(failure)) {
                    return std::string{"conflict"};
                }
                return std::unexpected(!base_symbols   ? base_symbols.error()
                                       : !left_symbols ? left_symbols.error()
                                                       : right_symbols.error());
            }
            auto merged = liquid::repository_detail::merge_structural(*base_tree,
                                                                      *left_tree,
                                                                      *right_tree,
                                                                      *base_source,
                                                                      *left_source,
                                                                      *right_source,
                                                                      &*base_symbols,
                                                                      &*left_symbols,
                                                                      &*right_symbols);
            if (!merged) {
                return std::unexpected(merged.error());
            }
            if (merged->outcome == liquid::repository_detail::structural_merge_outcome::conflict) {
                return std::string{"conflict"};
            }
            auto proposal_tree = parse(graph, language->language, merged->proposal);
            if (!proposal_tree) {
                return std::unexpected(proposal_tree.error());
            }
            if (merged->outcome ==
                    liquid::repository_detail::structural_merge_outcome::suspicious ||
                liquid::repository_detail::has_recovery_node(proposal_tree->root)) {
                return std::string{"suspicious"};
            }
            if (!human_blob) {
                classification = "different";
                continue;
            }
            auto human_source = liquid::runtime::block_on(repository.blob(*human_blob));
            if (!human_source) {
                return std::unexpected(human_source.error());
            }
            if (merged->proposal != *human_source) {
                classification = "different";
            }
            continue;
        }
        if (!same_blob(selected, human_blob)) {
            classification = "different";
        }
    }
    return classification;
}

auto measure(const std::filesystem::path& git_repository,
             const std::filesystem::path& state_directory) -> liquid::result<counts> {
    const auto existing = std::filesystem::exists(state_directory / "LIQUID_REPO");
    auto repository =
        liquid::runtime::block_on(existing ? liquid::repository::open(state_directory)
                                           : liquid::repository::create(state_directory));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    if (!existing) {
        auto imported = liquid::runtime::block_on(liquid::import_git(*repository, git_repository));
        if (!imported) {
            return std::unexpected(imported.error());
        }
    }
    auto module_value = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    if (!module_value) {
        return std::unexpected(module_value.error());
    }
    auto module = std::make_shared<liquid::frontend_module>(std::move(*module_value));
    auto graph = liquid::runtime::block_on(liquid::frontend_graph::open(
        state_directory / "measure-graph",
        git_repository,
        std::move(module),
        [](std::vector<std::byte>) -> liquid::runtime::task<std::vector<std::byte>> {
            co_return std::unexpected(liquid::error{
                liquid::errc::unsupported, "merge measurement does not run actions", {}});
        }));
    if (!graph) {
        return std::unexpected(graph.error());
    }
    auto history = liquid::runtime::block_on(repository->log());
    if (!history) {
        return std::unexpected(history.error());
    }
    auto changes = std::map<std::string, liquid::change>{};
    for (const auto& change : *history) {
        changes.emplace(key(change.id), change);
    }
    auto output = counts{};
    for (const auto& change : *history) {
        if (change.parents.size() < 2) {
            continue;
        }
        ++output.merges;
        auto classification = measure_merge(*repository, *graph, changes, change);
        if (!classification) {
            return std::unexpected(classification.error());
        }
        if (*classification == "identical") {
            ++output.identical;
        } else if (*classification == "different") {
            ++output.different;
        } else if (*classification == "suspicious") {
            ++output.suspicious;
        } else {
            ++output.conflict;
        }
    }
    return output;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: liquid-merge-measure <git-repository> <state-directory>\n";
        return 2;
    }
    auto result = measure(argv[1], argv[2]);
    if (!result) {
        std::cerr << result.error().message << ": " << result.error().context << '\n';
        return 1;
    }
    auto load = std::array<double, 3>{};
    getloadavg(load.data(), static_cast<int>(load.size()));
    std::cout << "repository,merges,clean_identical,clean_different,conflict,suspicious,load_1m\n";
    std::cout << argv[1] << ',' << result->merges << ',' << result->identical << ','
              << result->different << ',' << result->conflict << ',' << result->suspicious << ','
              << load[0] << '\n';
    return 0;
}
