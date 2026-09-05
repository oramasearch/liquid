#include "liquid/repository.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "search/index.hpp"
#include "search/repository_access.hpp"

namespace liquid {
namespace {

constexpr auto working_slot = std::string_view{"working"};

runtime::task<void> collect_tree(repository& source,
                                 liquid::tree root,
                                 std::string prefix,
                                 std::shared_ptr<std::vector<search_detail::document>> output) {
    for (const auto& entry : root.entries) {
        auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            auto nested = co_await source.tree(entry.ref);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            auto collected =
                co_await collect_tree(source, std::move(*nested), std::move(path), output);
            if (!collected) {
                co_return std::unexpected(collected.error());
            }
            continue;
        }
        if (entry.kind != node_kind::blob) {
            continue;
        }
        auto contents = co_await source.blob(entry.ref);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        auto id = hash(algorithm::blake3, *contents);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        output->push_back({std::move(path), *id, std::move(*contents)});
    }
    co_return result<void>{};
}

runtime::task<std::pair<std::string, liquid::tree>> selected_tree(repository& source,
                                                                  const search_options& options) {
    if (!options.ref) {
        auto root = co_await source.root();
        if (!root) {
            co_return std::unexpected(root.error());
        }
        co_return std::pair{std::string{working_slot}, std::move(*root)};
    }
    auto selected = std::optional<change_id>{};
    auto references = co_await source.refs();
    if (!references) {
        co_return std::unexpected(references.error());
    }
    const auto named = std::ranges::find(*references, *options.ref, &ref::name);
    if (named != references->end()) {
        selected = named->id;
    } else {
        auto parsed = parse_digest(*options.ref);
        if (!parsed) {
            co_return std::unexpected(
                error{errc::not_found, "search reference is absent", *options.ref});
        }
        selected = change_id{*parsed};
    }
    auto snapshot = co_await source.at(*selected);
    if (!snapshot) {
        co_return std::unexpected(snapshot.error());
    }
    co_return std::pair{"ref:" + *options.ref, std::move(snapshot->root)};
}

auto generation_for(const std::vector<search_detail::document>& documents) -> result<std::string> {
    auto encoded = std::string{};
    for (const auto& value : documents) {
        encoded.append(value.path);
        encoded.push_back('\0');
        encoded.append(to_string(value.blob));
        encoded.push_back('\n');
    }
    auto id = hash(algorithm::blake3,
                   std::span{reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    return to_string(*id);
}

} // namespace

runtime::task<std::vector<search_hit>> repository::search(std::string text,
                                                          search_options options) {
    auto directory = repository_detail::search_access::index_path(*this);
    if (!directory) {
        co_return std::unexpected(directory.error());
    }
    const auto expected_slot = options.ref ? "ref:" + *options.ref : std::string{working_slot};
    auto present = co_await runtime::offload([directory = *directory, expected_slot] {
        return search_detail::has_slot(directory, expected_slot);
    });
    if (!present) {
        co_return std::unexpected(present.error());
    }
    if (!*present) {
        co_return std::unexpected(present->error());
    }
    if (!(**present)) {
        auto selected = co_await selected_tree(*this, options);
        if (!selected) {
            co_return std::unexpected(selected.error());
        }
        auto documents = std::make_shared<std::vector<search_detail::document>>();
        auto collected =
            co_await collect_tree(*this, std::move(selected->second), std::string{}, documents);
        if (!collected) {
            co_return std::unexpected(collected.error());
        }
        std::ranges::sort(*documents, {}, &search_detail::document::path);
        auto generation = generation_for(*documents);
        if (!generation) {
            co_return std::unexpected(generation.error());
        }
        auto published = co_await runtime::offload([directory = *directory,
                                                    slot = std::move(selected->first),
                                                    generation = std::move(*generation),
                                                    documents] {
            return search_detail::publish_full(directory, slot, generation, *documents);
        });
        if (!published) {
            co_return std::unexpected(published.error());
        }
        if (!*published) {
            co_return std::unexpected(published->error());
        }
    }
    auto searched = co_await runtime::offload(
        [directory = *directory,
         slot = expected_slot,
         text = std::move(text),
         limit = options.limit] { return search_detail::query(directory, slot, text, limit); });
    if (!searched) {
        co_return std::unexpected(searched.error());
    }
    if (!*searched) {
        co_return std::unexpected(searched->error());
    }
    co_return std::move(**searched);
}

} // namespace liquid
