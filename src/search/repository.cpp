#include "liquid/repository.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <thread>
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

struct search_run {
    std::vector<search_hit> hits;
    liquid::search_plan plan;
};

struct scan_completion {
    result<std::vector<search_hit>> value;
};

boost::capy::io_task<scan_completion> scan_chunk(std::vector<search_detail::document> documents,
                                                 std::string text) {
    auto scanned =
        co_await runtime::offload([documents = std::move(documents), text = std::move(text)] {
            return search_detail::scan(documents, text, 1000);
        });
    if (!scanned) {
        co_return boost::capy::io_result<scan_completion>{
            {}, scan_completion{std::unexpected(scanned.error())}};
    }
    co_return boost::capy::io_result<scan_completion>{{}, scan_completion{std::move(*scanned)}};
}

auto document_statistics(const std::vector<search_detail::document>& documents)
    -> search_detail::index_statistics {
    auto blobs = std::set<std::string>{};
    auto output = search_detail::index_statistics{.paths = documents.size()};
    for (const auto& document : documents) {
        const auto key = to_string(document.blob);
        if (!blobs.insert(key).second) {
            continue;
        }
        if (!document.source.empty()) {
            auto code = std::error_code{};
            output.bytes += std::filesystem::file_size(document.source, code);
            if (code) {
                output.bytes += document.contents.size();
            }
        } else {
            output.bytes += document.contents.size();
        }
    }
    output.unique_blobs = blobs.size();
    return output;
}

runtime::task<std::vector<search_hit>>
parallel_scan(std::shared_ptr<std::vector<search_detail::document>> documents,
              std::string text,
              std::uint64_t limit) {
    if (documents->empty()) {
        co_return std::vector<search_hit>{};
    }
    const auto task_count =
        std::min<std::size_t>(documents->size(), std::max(1U, std::thread::hardware_concurrency()));
    const auto chunk_size = (documents->size() + task_count - 1) / task_count;
    auto tasks = std::vector<boost::capy::io_task<scan_completion>>{};
    for (auto begin = std::size_t{}; begin < documents->size(); begin += chunk_size) {
        const auto end = std::min(documents->size(), begin + chunk_size);
        auto chunk = std::vector<search_detail::document>{};
        chunk.reserve(end - begin);
        for (auto index = begin; index != end; ++index) {
            chunk.push_back(std::move((*documents)[index]));
        }
        tasks.push_back(scan_chunk(std::move(chunk), text));
    }
    auto [wait_error, results] = co_await runtime::when_all(std::move(tasks));
    if (wait_error) {
        co_return std::unexpected(
            error{errc::internal, wait_error.message(), "parallel search scan"});
    }
    auto output = std::vector<search_hit>{};
    for (auto& task_result : results) {
        if (!task_result.value) {
            co_return std::unexpected(task_result.value.error());
        }
        auto& hits = *task_result.value;
        output.insert(output.end(),
                      std::make_move_iterator(hits.begin()),
                      std::make_move_iterator(hits.end()));
    }
    std::ranges::sort(output, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score.value_or(0.0) > right.score.value_or(0.0);
        }
        return left.path < right.path;
    });
    if (output.size() > limit) {
        output.resize(static_cast<std::size_t>(limit));
    }
    co_return output;
}

runtime::task<std::shared_ptr<std::vector<search_detail::document>>>
collect_selected(repository& source, const search_options& options) {
    auto selected = co_await selected_tree(source, options);
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto documents = std::make_shared<std::vector<search_detail::document>>();
    auto collected =
        co_await collect_tree(source, std::move(selected->second), std::string{}, documents);
    if (!collected) {
        co_return std::unexpected(collected.error());
    }
    std::ranges::sort(*documents, {}, &search_detail::document::path);
    co_return documents;
}

using document_list = std::shared_ptr<std::vector<search_detail::document>>;

runtime::task<document_list> load_checkout_documents(const std::filesystem::path& directory,
                                                     bool retain_contents = false) {
    auto loaded = co_await runtime::offload([directory, retain_contents] {
        return search_detail::checkout_documents(directory.parent_path().parent_path(),
                                                 retain_contents);
    });
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    if (!*loaded) {
        co_return std::unexpected(loaded->error());
    }
    co_return std::make_shared<std::vector<search_detail::document>>(std::move(**loaded));
}

runtime::task<void> publish_documents(const std::filesystem::path& directory,
                                      std::string slot,
                                      std::string generation,
                                      document_list documents) {
    auto published = co_await runtime::offload(
        [directory, slot = std::move(slot), generation = std::move(generation), documents] {
            return search_detail::publish_full(directory, slot, generation, *documents);
        });
    if (!published) {
        co_return std::unexpected(published.error());
    }
    if (!*published) {
        co_return std::unexpected(published->error());
    }
    co_return result<void>{};
}

runtime::task<document_list> prepare_missing_index(repository& source,
                                                   const search_options& options,
                                                   const std::filesystem::path& directory,
                                                   const std::string& slot,
                                                   search_query_kind kind) {
    auto documents = document_list{};
    if (slot == working_slot && kind != search_query_kind::ranked_text) {
        auto loaded = co_await load_checkout_documents(directory);
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        documents = std::move(*loaded);
    } else {
        auto collected = co_await collect_selected(source, options);
        if (!collected) {
            co_return std::unexpected(collected.error());
        }
        documents = std::move(*collected);
    }
    if (kind == search_query_kind::ranked_text) {
        auto generation =
            options.ref ? generation_for(*documents) : search_detail::current_generation(directory);
        if (!generation) {
            co_return std::unexpected(generation.error());
        }
        auto published =
            co_await publish_documents(directory, slot, std::move(*generation), documents);
        if (!published) {
            co_return std::unexpected(published.error());
        }
    }
    co_return documents;
}

struct planner_state {
    search_detail::planner_inputs inputs;
    document_list documents;
};

runtime::task<planner_state> measure_plan_inputs(const std::filesystem::path& directory,
                                                 const std::string& slot,
                                                 const std::string& text,
                                                 bool index_available,
                                                 document_list documents) {
    auto output = planner_state{
        .inputs = {.scan_parallelism = std::max(1U, std::thread::hardware_concurrency()),
                   .index_available = index_available},
        .documents = std::move(documents)};
    if (!index_available) {
        output.inputs.working_set = document_statistics(*output.documents);
    } else {
        auto measured = co_await runtime::offload(
            [directory, slot] { return search_detail::statistics(directory, slot); });
        if (!measured) {
            co_return std::unexpected(measured.error());
        }
        if (!*measured) {
            co_return std::unexpected(measured->error());
        }
        output.inputs.working_set = **measured;
        auto stale = co_await runtime::offload(
            [directory, slot] { return search_detail::freshness(directory, slot); });
        if (!stale) {
            co_return std::unexpected(stale.error());
        }
        if (!*stale) {
            co_return std::unexpected(stale->error());
        }
        output.inputs.drips_since_index_update = **stale;
        if (output.inputs.drips_since_index_update != 0 && slot == working_slot) {
            auto loaded = co_await load_checkout_documents(directory);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            output.documents = std::move(*loaded);
            output.inputs.working_set = document_statistics(*output.documents);
        } else {
            auto selected = co_await runtime::offload([directory, slot, text] {
                return search_detail::selectivity(directory, slot, text);
            });
            if (!selected) {
                co_return std::unexpected(selected.error());
            }
            if (!*selected) {
                co_return std::unexpected(selected->error());
            }
            output.inputs.trigram_selectivity = **selected;
        }
    }
    if (!std::filesystem::exists(directory / "scan-calibration") && !output.documents &&
        index_available) {
        auto loaded = co_await runtime::offload(
            [directory, slot] { return search_detail::indexed_documents(directory, slot); });
        if (!loaded) {
            co_return std::unexpected(loaded.error());
        }
        if (!*loaded) {
            co_return std::unexpected(loaded->error());
        }
        output.documents =
            std::make_shared<std::vector<search_detail::document>>(std::move(**loaded));
    }
    auto sample = output.documents ? *output.documents : std::vector<search_detail::document>{};
    auto calibrated = co_await runtime::offload([directory, sample = std::move(sample)] {
        return search_detail::calibration(directory, sample);
    });
    if (!calibrated) {
        co_return std::unexpected(calibrated.error());
    }
    if (!*calibrated) {
        co_return std::unexpected(calibrated->error());
    }
    output.inputs.scan_nanoseconds_per_blob = **calibrated;
    co_return output;
}

runtime::task<search_run> run_plan(const std::filesystem::path& directory,
                                   std::string slot,
                                   std::string text,
                                   std::uint64_t limit,
                                   search_plan plan,
                                   document_list documents) {
    const auto started = std::chrono::steady_clock::now();
    auto hits = result<std::vector<search_hit>>{};
    if (plan.executor == search_executor::scan) {
        if (!documents) {
            auto loaded = co_await runtime::offload(
                [directory, slot] { return search_detail::indexed_documents(directory, slot); });
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            if (!*loaded) {
                co_return std::unexpected(loaded->error());
            }
            documents = std::make_shared<std::vector<search_detail::document>>(std::move(**loaded));
        }
        hits = co_await parallel_scan(documents, text, limit);
    } else {
        auto indexed =
            co_await runtime::offload([directory, slot, text, limit, executor = plan.executor] {
                return search_detail::query(directory, slot, text, limit, executor);
            });
        if (!indexed) {
            co_return std::unexpected(indexed.error());
        }
        hits = std::move(*indexed);
    }
    if (!hits) {
        co_return std::unexpected(hits.error());
    }
    plan.actual_cost_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    for (auto& alternative : plan.alternatives) {
        if (alternative.executor == plan.executor) {
            alternative.actual_cost_us = plan.actual_cost_us;
        }
    }
    co_return search_run{std::move(*hits), std::move(plan)};
}

runtime::task<search_run>
execute_search(repository& source, std::string text, search_options options) {
    auto directory = repository_detail::search_access::index_path(source);
    if (!directory) {
        co_return std::unexpected(directory.error());
    }
    auto kind = search_detail::classify(text);
    if (!kind) {
        co_return std::unexpected(kind.error());
    }
    const auto slot = options.ref ? "ref:" + *options.ref : std::string{working_slot};
    auto present = co_await runtime::offload(
        [directory = *directory, slot] { return search_detail::has_slot(directory, slot); });
    if (!present) {
        co_return std::unexpected(present.error());
    }
    if (!*present) {
        co_return std::unexpected(present->error());
    }
    auto index_available = **present;
    auto documents = document_list{};
    if (!index_available) {
        auto prepared = co_await prepare_missing_index(source, options, *directory, slot, *kind);
        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
        documents = std::move(*prepared);
        index_available = *kind == search_query_kind::ranked_text;
    }
    auto measured =
        co_await measure_plan_inputs(*directory, slot, text, index_available, documents);
    if (!measured) {
        co_return std::unexpected(measured.error());
    }
    auto inputs = measured->inputs;
    documents = std::move(measured->documents);
    auto plan = search_detail::make_plan(inputs, *kind);
    const auto observed_drips = inputs.drips_since_index_update;
    if (*kind == search_query_kind::ranked_text && inputs.drips_since_index_update != 0) {
        auto current = co_await load_checkout_documents(*directory, true);
        if (!current) {
            co_return std::unexpected(current.error());
        }
        documents = std::move(*current);
        auto generation = search_detail::current_generation(*directory);
        if (!generation) {
            co_return std::unexpected(generation.error());
        }
        auto published =
            co_await publish_documents(*directory, slot, std::move(*generation), documents);
        if (!published) {
            co_return std::unexpected(published.error());
        }
        inputs.working_set = document_statistics(*documents);
        inputs.drips_since_index_update = 0;
        plan = search_detail::make_plan(inputs, *kind);
        plan.drips_since_index_update = observed_drips;
        plan.alternatives.front().reason = "ranked index rebuilt to current cursor";
    }
    co_return co_await run_plan(
        *directory, slot, std::move(text), options.limit, std::move(plan), std::move(documents));
}

} // namespace

runtime::task<std::vector<search_hit>> repository::search(std::string text,
                                                          search_options options) {
    auto executed = co_await execute_search(*this, std::move(text), std::move(options));
    if (!executed) {
        co_return std::unexpected(executed.error());
    }
    co_return std::move(executed->hits);
}

runtime::task<liquid::search_plan> repository::plan_search(std::string text,
                                                           search_options options) {
    auto executed = co_await execute_search(*this, std::move(text), std::move(options));
    if (!executed) {
        co_return std::unexpected(executed.error());
    }
    co_return std::move(executed->plan);
}

} // namespace liquid
