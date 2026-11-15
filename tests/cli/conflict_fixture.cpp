// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto entry_reference(liquid::repository& source, std::string_view name)
    -> liquid::result<liquid::digest> {
    auto tree = liquid::runtime::block_on(source.root());
    if (!tree) {
        return std::unexpected(tree.error());
    }
    const auto entry = std::ranges::find(tree->entries, name, &liquid::tree_entry::name);
    if (entry == tree->entries.end()) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "fixture entry is absent", std::string{name}});
    }
    return entry->ref;
}

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 2) {
        std::cerr << "usage: liquid-conflict-cli-fixture <repository>\n";
        return 2;
    }
    auto created = liquid::runtime::block_on(liquid::repository::create(argv[1]));
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto source = std::move(*created);
    auto references = std::vector<liquid::digest>{};
    for (const auto value : {"base", "left", "right"}) {
        auto written = liquid::runtime::block_on(source.write("document.txt", bytes(value)));
        if (!written) {
            std::cerr << written.error().message << '\n';
            return 1;
        }
        auto reference = entry_reference(source, "document.txt");
        if (!reference) {
            std::cerr << reference.error().message << '\n';
            return 1;
        }
        references.push_back(*reference);
    }
    auto conflict = liquid::conflict{
        .base = references[0],
        .left = references[1],
        .right = references[2],
        .region = {.path = "document.txt", .bytes = liquid::conflict_byte_range{0, 5}},
        .kind = liquid::conflict_kind::crdt_suspicious,
        .explanation = "test double reported a suspicious CRDT merge"};
    auto stored = liquid::runtime::block_on(source.record_conflict(std::move(conflict)));
    auto proposal_references = std::vector<liquid::digest>{};
    for (const auto value : {"base", "left", "right", "merged proposal", "right"}) {
        auto written = liquid::runtime::block_on(source.write("proposal.cpp", bytes(value)));
        if (!written) {
            std::cerr << written.error().message << '\n';
            return 1;
        }
        auto reference = entry_reference(source, "proposal.cpp");
        if (!reference) {
            std::cerr << reference.error().message << '\n';
            return 1;
        }
        proposal_references.push_back(*reference);
    }
    auto suspicious =
        liquid::conflict{.base = proposal_references[0],
                         .left = proposal_references[1],
                         .right = proposal_references[2],
                         .region = {.path = "proposal.cpp", .nodes = liquid::conflict_node_range{}},
                         .kind = liquid::conflict_kind::suspicious_merge,
                         .explanation = "same definition was introduced by both sides",
                         .proposal = proposal_references[3],
                         .reasons = {"same definition was introduced by both sides"},
                         .node = "duplicate"};
    auto suspicious_stored =
        liquid::runtime::block_on(source.record_conflict(std::move(suspicious)));
    auto cut = liquid::runtime::block_on(source.cut("record CLI conflict"));
    if (!stored || !suspicious_stored || !cut) {
        std::cerr << (!stored              ? stored.error().message
                      : !suspicious_stored ? suspicious_stored.error().message
                                           : cut.error().message)
                  << '\n';
        return 1;
    }
    return 0;
}
