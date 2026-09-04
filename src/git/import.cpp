#include "liquid/git.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "git/object_store.hpp"

namespace liquid {
namespace {

struct git_file {
    git_detail::sha1 id;
    std::uint32_t mode{};
};
struct liquid_file {
    digest id;
    std::uint32_t mode{};
};

auto text_bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto git_files(git_detail::object_store& store,
               const git_detail::sha1& id,
               std::string path,
               std::map<std::string, git_file>& out) -> result<void> {
    const auto entries = store.read_tree(id);
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        const auto child = path.empty() ? entry.name : path + "/" + entry.name;
        if (entry.mode == 040000) {
            auto result = git_files(store, entry.id, child, out);
            if (!result)
                return std::unexpected(result.error());
        } else if (!out.emplace(child, git_file{entry.id, entry.mode}).second) {
            return std::unexpected(error{errc::corrupt, "duplicate Git tree path", child});
        }
    }
    return {};
}

auto liquid_files(repository& repository,
                  const tree& value,
                  std::string path,
                  std::map<std::string, liquid_file>& out) -> runtime::task<void> {
    for (const auto& entry : value.entries) {
        const auto child = path.empty() ? entry.name : path + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            const auto nested = co_await repository.tree(entry.ref);
            if (!nested)
                co_return std::unexpected(nested.error());
            const auto result = co_await liquid_files(repository, *nested, child, out);
            if (!result)
                co_return std::unexpected(result.error());
        } else {
            out.emplace(child, liquid_file{entry.ref, entry.mode});
        }
    }
    co_return result<void>{};
}

auto mapping(repository& repository, std::string key) -> runtime::task<std::optional<digest>> {
    const auto value = co_await repository.metadata(std::move(key));
    if (!value)
        co_return std::unexpected(value.error());
    if (!*value)
        co_return std::optional<digest>{};
    const auto parsed = parse_digest(
        std::string{reinterpret_cast<const char*>((*value)->data()), (*value)->size()});
    if (!parsed)
        co_return std::unexpected(parsed.error());
    co_return std::optional<digest>{*parsed};
}

auto apply_tree(repository& repository,
                git_detail::object_store& store,
                const std::map<std::string, git_file>& wanted,
                std::uint64_t& blobs) -> runtime::task<void> {
    const auto root = co_await repository.root();
    if (!root)
        co_return std::unexpected(root.error());
    std::map<std::string, liquid_file> current;
    const auto listed = co_await liquid_files(repository, *root, {}, current);
    if (!listed)
        co_return std::unexpected(listed.error());
    std::set<std::string> moved;
    for (const auto& [path, entry] : wanted) {
        if (current.contains(path) || entry.mode == 0160000)
            continue;
        const auto mapped =
            co_await mapping(repository, "git.blob." + git_detail::sha1_hex(entry.id));
        if (!mapped || !*mapped)
            continue;
        const auto source = std::ranges::find_if(current, [&](const auto& value) {
            return !wanted.contains(value.first) && !moved.contains(value.first) &&
                   value.second.id.alg == (*mapped)->alg &&
                   value.second.id.bytes == (*mapped)->bytes;
        });
        if (source == current.end())
            continue;
        const auto renamed = co_await repository.move(source->first, path);
        if (renamed)
            moved.insert(source->first);
    }
    for (const auto& [path, entry] : wanted) {
        if (entry.mode == 0160000) {
            if (!current.contains(path)) {
                const auto hashed = hash(algorithm::blake3, std::span<const std::byte>{entry.id});
                if (!hashed)
                    co_return std::unexpected(hashed.error());
                const auto nested =
                    co_await repository.nest_ref(path, repo_ref{{}, change_id{*hashed}});
                if (!nested)
                    co_return std::unexpected(nested.error());
            }
            continue;
        }
        const auto object = store.read(entry.id);
        if (!object)
            co_return std::unexpected(object.error());
        if (object->kind != git_detail::object_kind::blob)
            co_return std::unexpected(error{errc::corrupt, "Git tree entry is not a blob", path});
        const auto written = co_await repository.write(path, object->payload, {.mode = entry.mode});
        if (!written)
            co_return std::unexpected(written.error());
        ++blobs;
    }
    for (const auto& [path, entry] : current) {
        static_cast<void>(entry);
        if (!wanted.contains(path) && !moved.contains(path)) {
            const auto removed = co_await repository.remove(path);
            if (!removed)
                co_return std::unexpected(removed.error());
        }
    }
    const auto refreshed = co_await repository.root();
    if (!refreshed)
        co_return std::unexpected(refreshed.error());
    current.clear();
    const auto refreshed_files = co_await liquid_files(repository, *refreshed, {}, current);
    if (!refreshed_files)
        co_return std::unexpected(refreshed_files.error());
    for (const auto& [path, entry] : wanted) {
        if (entry.mode == 0160000)
            continue;
        const auto found = current.find(path);
        if (found == current.end())
            co_return std::unexpected(error{errc::internal, "imported Git path is missing", path});
        const auto saved = co_await repository.set_metadata(
            "git.blob." + git_detail::sha1_hex(entry.id), text_bytes(to_string(found->second.id)));
        if (!saved)
            co_return std::unexpected(saved.error());
    }
    co_return result<void>{};
}

} // namespace

runtime::task<import_report> import_git(repository& destination, std::filesystem::path source) {
    const auto directory = git_detail::git_directory(source);
    if (!directory)
        co_return std::unexpected(directory.error());
    auto store = git_detail::object_store{*directory};
    const auto refs = store.refs();
    if (!refs)
        co_return std::unexpected(refs.error());
    std::map<std::string, git_detail::commit> commits;
    std::vector<git_detail::sha1> pending;
    for (const auto& [name, id] : *refs) {
        static_cast<void>(name);
        const auto peeled = store.peel(id);
        if (!peeled)
            co_return std::unexpected(peeled.error());
        pending.push_back(*peeled);
    }
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto name = git_detail::sha1_hex(id);
        if (commits.contains(name))
            continue;
        const auto commit = store.read_commit(id);
        if (!commit)
            co_return std::unexpected(commit.error());
        commits.emplace(name, *commit);
        pending.insert(pending.end(), commit->parents.begin(), commit->parents.end());
    }
    auto earlier = [](const auto* left, const auto* right) {
        if (left->author_time != right->author_time)
            return left->author_time < right->author_time;
        return git_detail::sha1_hex(left->id) < git_detail::sha1_hex(right->id);
    };
    std::map<std::string, std::uint64_t> parents;
    std::map<std::string, std::vector<const git_detail::commit*>> children;
    std::vector<const git_detail::commit*> ready;
    for (const auto& [name, commit] : commits) {
        auto& count = parents[name];
        for (const auto& parent : commit.parents) {
            const auto parent_name = git_detail::sha1_hex(parent);
            if (commits.contains(parent_name)) {
                ++count;
                children[parent_name].push_back(&commit);
            }
        }
        if (count == 0)
            ready.push_back(&commit);
    }
    std::ranges::sort(ready, earlier);
    std::vector<const git_detail::commit*> ordered;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.erase(ready.begin());
        ordered.push_back(current);
        for (const auto* child : children[git_detail::sha1_hex(current->id)]) {
            auto& count = parents[git_detail::sha1_hex(child->id)];
            if (--count == 0) {
                ready.push_back(child);
                std::ranges::sort(ready, earlier);
            }
        }
    }
    import_report report{};
    for (const auto* commit : ordered) {
        const auto name = git_detail::sha1_hex(commit->id);
        const auto existing = co_await destination.metadata("git.commit." + name);
        if (!existing)
            co_return std::unexpected(existing.error());
        if (*existing) {
            ++report.skipped_commits;
            continue;
        }
        std::map<std::string, git_file> files;
        const auto flattened = git_files(store, commit->tree, {}, files);
        if (!flattened)
            co_return std::unexpected(flattened.error());
        const auto applied = co_await apply_tree(destination, store, files, report.blobs);
        if (!applied)
            co_return std::unexpected(applied.error());
        change_input input{};
        for (const auto& parent : commit->parents) {
            const auto mapped =
                co_await mapping(destination, "git.commit." + git_detail::sha1_hex(parent));
            if (!mapped || !*mapped)
                co_return std::unexpected(mapped ? error{errc::corrupt,
                                                         "Git parent was not imported",
                                                         git_detail::sha1_hex(parent)}
                                                 : mapped.error());
            input.parents.push_back(change_ref{{}, change_id{**mapped}});
        }
        input.author = actor{"git:" + commit->author};
        input.at = std::chrono::sys_seconds{std::chrono::seconds{commit->author_time}};
        input.message = commit->message;
        input.metadata.emplace("git_commit", name);
        const auto recorded = co_await destination.record(std::move(input));
        if (!recorded)
            co_return std::unexpected(recorded.error());
        const auto saved = co_await destination.set_metadata(
            "git.commit." + name, text_bytes(to_string(recorded->value)));
        if (!saved)
            co_return std::unexpected(saved.error());
        const auto reverse = co_await destination.set_metadata(
            "git.change." + to_string(recorded->value), text_bytes(name));
        if (!reverse)
            co_return std::unexpected(reverse.error());
        ++report.commits;
    }
    for (const auto& [name, id] : *refs) {
        const auto peeled = store.peel(id);
        if (!peeled)
            co_return std::unexpected(peeled.error());
        const auto mapped =
            co_await mapping(destination, "git.commit." + git_detail::sha1_hex(*peeled));
        if (!mapped || !*mapped)
            continue;
        auto liquid_name = name;
        if (liquid_name.starts_with("refs/heads/"))
            liquid_name.erase(0, 11);
        if (liquid_name.starts_with("refs/tags/"))
            liquid_name = "tags/" + liquid_name.substr(10);
        const auto saved = co_await destination.set_ref(liquid_name, change_id{**mapped});
        if (!saved)
            co_return std::unexpected(saved.error());
        report.refs.push_back(liquid_name);
    }
    co_return report;
}

} // namespace liquid
