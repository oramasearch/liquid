#include "liquid/git.hpp"

#include <algorithm>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <unordered_map>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <charconv>
#include <fstream>
#elif !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "git/object_store.hpp"
#include "repository/git_access.hpp"

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

constexpr std::size_t memo_limit = 1'000'000;

template <typename value> class lru_map {
public:
    auto get(const std::string& key) -> std::optional<value> {
        const auto found = index_.find(key);
        if (found == index_.end())
            return {};
        entries_.splice(entries_.begin(), entries_, found->second);
        return found->second->second;
    }

    auto put(std::string key, value item) -> void {
        const auto found = index_.find(key);
        if (found != index_.end()) {
            found->second->second = std::move(item);
            entries_.splice(entries_.begin(), entries_, found->second);
            return;
        }
        entries_.emplace_front(std::move(key), std::move(item));
        index_[entries_.front().first] = entries_.begin();
        if (entries_.size() > memo_limit) {
            index_.erase(entries_.back().first);
            entries_.pop_back();
        }
    }

private:
    std::list<std::pair<std::string, value>> entries_;
    std::unordered_map<std::string, typename std::list<std::pair<std::string, value>>::iterator>
        index_;
};

auto node_for(const git_detail::sha1& id) -> node_id {
    node_id node{};
    std::ranges::copy_n(id.begin(), node.size(), node.begin());
    return node;
}

auto import_tree(repository& destination,
                 git_detail::object_store& store,
                 const git_detail::sha1& id,
                 lru_map<digest>& blobs,
                 lru_map<digest>& trees,
                 import_report& report) -> runtime::task<digest> {
    const auto key = git_detail::sha1_hex(id);
    if (const auto cached = trees.get(key))
        co_return *cached;
    const auto source = store.read_tree(id);
    if (!source)
        co_return std::unexpected(source.error());
    tree value{};
    value.entries.reserve(source->size());
    for (const auto& item : *source) {
        tree_entry entry{};
        entry.name = item.name;
        entry.mode = item.mode;
        entry.node = node_for(item.id);
        if (item.mode == 040000) {
            const auto child =
                co_await import_tree(destination, store, item.id, blobs, trees, report);
            if (!child)
                co_return std::unexpected(child.error());
            entry.kind = node_kind::tree;
            entry.id = *child;
            entry.ref = *child;
        } else if (item.mode == 0160000) {
            const auto placeholder = hash(algorithm::blake3, std::span<const std::byte>{item.id});
            if (!placeholder)
                co_return std::unexpected(placeholder.error());
            entry.kind = node_kind::repo;
            entry.id = *placeholder;
            entry.ref = *placeholder;
            entry.nested = repo_ref{{}, change_id{*placeholder}};
        } else {
            const auto blob_key = git_detail::sha1_hex(item.id);
            auto stored = blobs.get(blob_key);
            if (!stored) {
                auto object = store.read(item.id);
                if (!object)
                    co_return std::unexpected(object.error());
                if (object->kind != git_detail::object_kind::blob)
                    co_return std::unexpected(
                        error{errc::corrupt, "Git tree entry is not a blob", item.name});
                const auto written = co_await repository_detail::git_access::put_blob(
                    destination, std::move(object->payload));
                if (!written)
                    co_return std::unexpected(written.error());
                blobs.put(blob_key, *written);
                stored = *written;
                ++report.blobs;
            }
            entry.kind = node_kind::blob;
            entry.id = *stored;
            entry.ref = *stored;
        }
        value.entries.push_back(std::move(entry));
    }
    const auto stored =
        co_await repository_detail::git_access::put_tree(destination, std::move(value));
    if (!stored)
        co_return std::unexpected(stored.error());
    trees.put(key, *stored);
    co_return *stored;
}

auto liquid_ref_name(std::string name) -> std::string {
    if (name.starts_with("refs/heads/"))
        return name.substr(11);
    if (name.starts_with("refs/tags/"))
        return "tags/" + name.substr(10);
    return name;
}

auto head_ref(const std::filesystem::path& directory) -> std::optional<std::string> {
    auto input = std::ifstream{directory / "HEAD"};
    auto value = std::string{};
    std::getline(input, value);
    constexpr auto prefix = std::string_view{"ref: "};
    if (!value.starts_with(prefix))
        return {};
    return value.substr(prefix.size());
}

auto anonymous_memory_bytes() -> std::uint64_t {
#if defined(__APPLE__)
    auto info = task_vm_info_data_t{};
    auto count = mach_msg_type_number_t{TASK_VM_INFO_COUNT};
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
#elif defined(__linux__)
    auto input = std::ifstream{"/proc/self/status"};
    auto line = std::string{};
    while (std::getline(input, line)) {
        constexpr auto prefix = std::string_view{"RssAnon:"};
        if (!line.starts_with(prefix))
            continue;
        auto value = std::uint64_t{};
        const auto first = line.find_first_of("0123456789");
        if (first == std::string::npos)
            return 0;
        const auto [end, code] =
            std::from_chars(line.data() + first, line.data() + line.size(), value);
        if (code != std::errc{} || end == line.data() + first)
            return 0;
        return value * 1024U;
    }
    return 0;
#else
    return 0;
#endif
}

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
    const auto symbolic_head = head_ref(*directory);
    auto selected_head = std::optional<git_detail::sha1>{};
    auto selected_ref = std::optional<std::string>{};
    if (symbolic_head) {
        const auto found = refs->find(*symbolic_head);
        if (found != refs->end()) {
            selected_head = found->second;
            selected_ref = liquid_ref_name(*symbolic_head);
        }
    }
    if (!selected_head) {
        for (const auto& [name, id] : *refs) {
            if (!name.starts_with("refs/heads/"))
                continue;
            selected_head = id;
            selected_ref = liquid_ref_name(name);
            break;
        }
    }
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
    auto blobs = lru_map<digest>{};
    auto trees = lru_map<digest>{};
    for (const auto* commit : ordered) {
        const auto name = git_detail::sha1_hex(commit->id);
        const auto existing = co_await destination.metadata("git.commit." + name);
        if (!existing)
            co_return std::unexpected(existing.error());
        if (*existing) {
            ++report.skipped_commits;
            continue;
        }
        const auto root =
            co_await import_tree(destination, store, commit->tree, blobs, trees, report);
        if (!root)
            co_return std::unexpected(root.error());
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
        if (selected_head && selected_ref && commit->id == *selected_head)
            input.ref = *selected_ref;
        const auto recorded = co_await destination.record_from_tree(std::move(input), *root);
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
        report.peak_anonymous_bytes =
            std::max(report.peak_anonymous_bytes, anonymous_memory_bytes());
    }
    for (const auto& [name, id] : *refs) {
        const auto peeled = store.peel(id);
        if (!peeled)
            co_return std::unexpected(peeled.error());
        const auto mapped =
            co_await mapping(destination, "git.commit." + git_detail::sha1_hex(*peeled));
        if (!mapped || !*mapped)
            continue;
        const auto liquid_name = liquid_ref_name(name);
        const auto saved = co_await destination.set_ref(liquid_name, change_id{**mapped});
        if (!saved)
            co_return std::unexpected(saved.error());
        report.refs.push_back(liquid_name);
    }
    if (selected_head) {
        const auto commit = store.read_commit(*selected_head);
        if (!commit)
            co_return std::unexpected(commit.error());
        std::map<std::string, git_file> files;
        const auto flattened = git_files(store, commit->tree, {}, files);
        if (!flattened)
            co_return std::unexpected(flattened.error());
        const auto applied = co_await apply_tree(destination, store, files, report.blobs);
        if (!applied)
            co_return std::unexpected(applied.error());
        const auto mapped =
            co_await mapping(destination, "git.commit." + git_detail::sha1_hex(*selected_head));
        if (!mapped || !*mapped)
            co_return std::unexpected(mapped ? error{errc::corrupt,
                                                     "Git head was not imported",
                                                     git_detail::sha1_hex(*selected_head)}
                                             : mapped.error());
        const auto checkpointed =
            co_await destination.set_ref(selected_ref.value_or("main"), change_id{**mapped});
        if (!checkpointed)
            co_return std::unexpected(checkpointed.error());
    }
    report.peak_anonymous_bytes = std::max(report.peak_anonymous_bytes, anonymous_memory_bytes());
    co_return report;
}

} // namespace liquid
