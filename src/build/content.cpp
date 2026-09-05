#include "build/content.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "build/normalise.hpp"
#include "build/repository_access.hpp"
#include "identity/taap.hpp"
#include "objects/records.hpp"
#include "store/references.hpp"
#include "vfs/repository_access.hpp"

namespace liquid::build_detail {
namespace {

struct output_node {
    std::map<std::string, output_node> directories;
    std::optional<action_output> file;
};

auto content_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto store_tree(repository& source, tree value) -> runtime::task<digest> {
    auto encoded = objects::encode(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    co_return co_await repository_detail::build_access::put_record(source, std::move(*encoded));
}

auto insert_output(output_node& root, const action_output& output) -> result<void> {
    auto path = std::filesystem::path{output.path}.lexically_normal();
    if (path.empty() || path.is_absolute()) {
        return std::unexpected(
            content_error(errc::invalid_argument, "output path is not relative", output.path));
    }
    auto* current = &root;
    auto components = std::vector<std::string>{};
    for (const auto& component : path) {
        const auto text = component.string();
        if (text.empty() || text == "." || text == "..") {
            return std::unexpected(content_error(
                errc::invalid_argument, "output path is not normalized", output.path));
        }
        components.push_back(text);
    }
    for (auto index = std::size_t{}; index + 1 < components.size(); ++index) {
        current = &current->directories[components[index]];
        if (current->file) {
            return std::unexpected(
                content_error(errc::conflict, "output path traverses an output file", output.path));
        }
    }
    auto& leaf = current->directories[components.back()];
    if (leaf.file || !leaf.directories.empty()) {
        return std::unexpected(content_error(errc::conflict, "duplicate output path", output.path));
    }
    leaf.file = output;
    return {};
}

auto store_output_node(repository& source, const output_node& node) -> runtime::task<digest> {
    auto value = tree{};
    for (const auto& [name, child] : node.directories) {
        if (child.file) {
            value.entries.push_back({.name = name,
                                     .mode = 0100000U | normalised_output_mode(child.file->mode),
                                     .kind = node_kind::blob,
                                     .id = child.file->blob,
                                     .ref = child.file->blob});
            continue;
        }
        auto nested = co_await store_output_node(source, child);
        if (!nested) {
            co_return std::unexpected(nested.error());
        }
        value.entries.push_back({.name = name,
                                 .mode = 0040755,
                                 .kind = node_kind::tree,
                                 .id = *nested,
                                 .ref = *nested});
    }
    co_return co_await store_tree(source, std::move(value));
}

auto signed_result_is_valid(const action_result& value) -> result<bool> {
    auto signature = identity_detail::parse_signature(value.sig);
    if (!signature) {
        return false;
    }
    auto body = objects::encode_result_body(value);
    if (!body) {
        return std::unexpected(body.error());
    }
    return verify(value.producer, "result", *body, *signature);
}

auto resolve_input_tree(repository& source, digest input) -> runtime::task<digest> {
    auto bytes = co_await repository_detail::build_access::get(source, input);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    if (objects::decode_tree(*bytes)) {
        co_return input;
    }
    auto result_record = objects::decode_result(*bytes);
    if (!result_record) {
        co_return std::unexpected(content_error(
            errc::invalid_argument, "named input is not a result or tree", to_string(input)));
    }
    auto valid = signed_result_is_valid(*result_record);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    if (!*valid) {
        co_return std::unexpected(content_error(
            errc::unauthorized, "named input result signature is invalid", to_string(input)));
    }
    if (!result_record->output_tree) {
        co_return std::unexpected(content_error(
            errc::unsupported, "named input result has no output tree", to_string(input)));
    }
    co_return *result_record->output_tree;
}

auto valid_input_name(std::string_view name) -> bool {
    if (name.empty() || name == "." || name == ".." || name == "deps") {
        return false;
    }
    const auto path = std::filesystem::path{name};
    return !path.is_absolute() && std::distance(path.begin(), path.end()) == 1;
}

auto copy_object_graph(repository& source,
                       repository& destination,
                       digest root,
                       std::set<std::string>& copied) -> runtime::task<void> {
    const auto key = to_string(root);
    if (!copied.insert(key).second) {
        co_return result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, root);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    const auto kind = store_detail::detect_object_kind(*bytes);
    const auto references = store_detail::reference_registry{}.extract(kind, *bytes);
    for (const auto& reference : references.digests) {
        auto copied_reference = co_await copy_object_graph(source, destination, reference, copied);
        if (!copied_reference) {
            co_return std::unexpected(copied_reference.error());
        }
    }
    auto stored = co_await repository_detail::build_access::put_record(
        destination, std::move(*bytes), root.alg);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!same_digest(*stored, root)) {
        co_return std::unexpected(
            content_error(errc::corrupt, "copied object digest changed", key));
    }
    co_return result<void>{};
}

auto copy_named_input(repository& source, repository& destination, digest input)
    -> runtime::task<void> {
    auto bytes = co_await repository_detail::build_access::get(source, input);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    if (objects::decode_tree(*bytes)) {
        auto copied = std::set<std::string>{};
        co_return co_await copy_object_graph(source, destination, input, copied);
    }
    auto result_record = objects::decode_result(*bytes);
    if (!result_record) {
        co_return std::unexpected(content_error(
            errc::invalid_argument, "named input is not a result or tree", to_string(input)));
    }
    auto valid = signed_result_is_valid(*result_record);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    if (!*valid || !result_record->output_tree) {
        co_return std::unexpected(content_error(!*valid ? errc::unauthorized : errc::unsupported,
                                                !*valid ? "named input result signature is invalid"
                                                        : "named input result has no output tree",
                                                to_string(input)));
    }
    auto copied = std::set<std::string>{};
    auto output =
        co_await copy_object_graph(source, destination, *result_record->output_tree, copied);
    if (!output) {
        co_return std::unexpected(output.error());
    }
    auto action_bytes =
        co_await repository_detail::build_access::get(source, result_record->action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = co_await repository_detail::build_access::put_record(
        destination, std::move(*action_bytes), result_record->action.alg);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    if (!same_digest(*action, result_record->action)) {
        co_return std::unexpected(content_error(
            errc::corrupt, "copied action digest changed", to_string(result_record->action)));
    }
    auto stored = co_await repository_detail::build_access::put_record(
        destination, std::move(*bytes), input.alg);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!same_digest(*stored, input)) {
        co_return std::unexpected(
            content_error(errc::corrupt, "copied result digest changed", to_string(input)));
    }
    co_return result<void>{};
}

auto sibling_repositories(const std::filesystem::path& cache_directory)
    -> result<std::vector<std::filesystem::path>> {
    auto output = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    const auto base = cache_directory.parent_path();
    for (std::filesystem::directory_iterator iterator{base, code};
         !code && iterator != std::filesystem::directory_iterator{};
         iterator.increment(code)) {
        if (!iterator->is_directory(code)) {
            continue;
        }
        const auto repository = iterator->path() / "repository";
        if (std::filesystem::is_regular_file(repository / "LIQUID_REPO", code)) {
            output.push_back(repository);
        }
        code.clear();
    }
    if (code) {
        return std::unexpected(content_error(errc::io, code.message(), base.string()));
    }
    std::ranges::sort(output);
    return output;
}

} // namespace

auto store_output_tree(repository& source, std::span<const action_output> outputs)
    -> runtime::task<digest> {
    auto root = output_node{};
    for (const auto& output : outputs) {
        auto inserted = insert_output(root, output);
        if (!inserted) {
            co_return std::unexpected(inserted.error());
        }
    }
    co_return co_await store_output_node(source, root);
}

auto fold_dependency_inputs(repository& source, digest base, std::span<const named_input> inputs)
    -> runtime::task<digest> {
    if (inputs.empty()) {
        co_return base;
    }
    auto base_bytes = co_await repository_detail::build_access::get(source, base);
    auto root = base_bytes ? objects::decode_tree(*base_bytes)
                           : result<tree>{std::unexpected(base_bytes.error())};
    if (!root) {
        co_return std::unexpected(root.error());
    }
    if (std::ranges::find(root->entries, "deps", &tree_entry::name) != root->entries.end()) {
        co_return std::unexpected(content_error(
            errc::conflict, "input tree already contains the reserved deps directory"));
    }
    auto names = std::set<std::string>{};
    auto dependencies = tree{};
    for (const auto& input : inputs) {
        if (!valid_input_name(input.name) || !names.insert(input.name).second) {
            co_return std::unexpected(content_error(errc::invalid_argument,
                                                    "named input has an invalid or duplicate name",
                                                    input.name));
        }
        auto resolved = co_await resolve_input_tree(source, input.source);
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        dependencies.entries.push_back({.name = input.name,
                                        .mode = 0040555,
                                        .kind = node_kind::tree,
                                        .id = *resolved,
                                        .ref = *resolved});
    }
    std::ranges::sort(dependencies.entries, {}, &tree_entry::name);
    auto dependency_tree = co_await store_tree(source, std::move(dependencies));
    if (!dependency_tree) {
        co_return std::unexpected(dependency_tree.error());
    }
    root->entries.push_back({.name = "deps",
                             .mode = 0040555,
                             .kind = node_kind::tree,
                             .id = *dependency_tree,
                             .ref = *dependency_tree});
    std::ranges::sort(root->entries, {}, &tree_entry::name);
    co_return co_await store_tree(source, std::move(*root));
}

auto import_wrap_inputs(repository& destination,
                        const std::filesystem::path& cache_directory,
                        std::span<const named_input> inputs) -> runtime::task<void> {
    if (inputs.empty()) {
        co_return result<void>{};
    }
    auto repositories = co_await runtime::offload(
        [cache_directory] { return sibling_repositories(cache_directory); });
    if (!repositories) {
        co_return std::unexpected(repositories.error());
    }
    if (!*repositories) {
        co_return std::unexpected(repositories->error());
    }
    for (const auto& input : inputs) {
        auto present =
            co_await repository_detail::build_access::contains(destination, input.source);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (*present) {
            continue;
        }
        auto imported = false;
        for (const auto& path : **repositories) {
            auto candidate = co_await repository::open(path);
            if (!candidate) {
                continue;
            }
            auto found =
                co_await repository_detail::build_access::contains(*candidate, input.source);
            if (!found || !*found) {
                continue;
            }
            auto graph = co_await copy_named_input(*candidate, destination, input.source);
            if (!graph) {
                co_return std::unexpected(graph.error());
            }
            imported = true;
            break;
        }
        if (!imported) {
            co_return std::unexpected(
                content_error(errc::not_found,
                              "named input is absent from sibling wrap caches",
                              to_string(input.source)));
        }
    }
    co_return result<void>{};
}

auto normalise_repository_outputs(repository& source, const std::set<std::string>& paths)
    -> runtime::task<void> {
    for (const auto& path : paths) {
        auto contents = co_await source.read(path);
        if (!contents) {
            if (contents.error().code == errc::not_found) {
                continue;
            }
            co_return std::unexpected(contents.error());
        }
        auto normalised =
            co_await runtime::offload([path, contents = std::move(*contents)]() mutable {
                const auto kind = output_kind_for(path, contents);
                return normalise_output(kind, std::move(contents));
            });
        if (!normalised) {
            co_return std::unexpected(normalised.error());
        }
        if (!*normalised) {
            co_return std::unexpected(normalised->error());
        }
        auto current = co_await source.read(path);
        if (!current) {
            co_return std::unexpected(current.error());
        }
        if (*current == **normalised) {
            continue;
        }
        auto entry = co_await repository_detail::vfs_access::entry(source, path);
        if (!entry) {
            co_return std::unexpected(entry.error());
        }
        auto written =
            co_await source.write(path,
                                  std::move(**normalised),
                                  {.mode = 0100000U | normalised_output_mode(entry->mode)});
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    co_return result<void>{};
}

} // namespace liquid::build_detail

namespace liquid::build {

runtime::task<std::vector<known_output>> known_outputs(repository& source) {
    auto output = std::vector<known_output>{};
    auto filter = result_filter{.page = {.limit = 1'000}};
    while (true) {
        auto page = co_await source.results(filter);
        if (!page) {
            co_return std::unexpected(page.error());
        }
        for (const auto& record : page->records) {
            if (!record.value.output_tree) {
                continue;
            }
            auto valid = build_detail::signed_result_is_valid(record.value);
            if (!valid) {
                co_return std::unexpected(valid.error());
            }
            if (!*valid) {
                continue;
            }
            auto action = co_await source.action(record.value.action);
            if (!action) {
                co_return std::unexpected(action.error());
            }
            const auto duplicate = std::ranges::any_of(output, [&](const known_output& candidate) {
                return build_detail::same_digest(candidate.input_tree, action->inputs) &&
                       build_detail::same_digest(candidate.output_tree, *record.value.output_tree);
            });
            if (!duplicate) {
                output.push_back({action->inputs, *record.value.output_tree});
            }
        }
        if (page->done) {
            break;
        }
        filter.page.cursor = page->next;
    }
    std::ranges::sort(output, [](const known_output& left, const known_output& right) {
        if (to_string(left.input_tree) != to_string(right.input_tree)) {
            return to_string(left.input_tree) < to_string(right.input_tree);
        }
        return to_string(left.output_tree) < to_string(right.output_tree);
    });
    co_return output;
}

} // namespace liquid::build
