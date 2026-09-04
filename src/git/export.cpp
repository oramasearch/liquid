#include "liquid/git.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "git/object_store.hpp"

namespace liquid {
namespace {

auto add(std::vector<std::byte>& output, std::string_view value) -> void {
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

auto write_text(const std::filesystem::path& path, std::string_view value) -> result<void> {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code)
        return std::unexpected(error{errc::io, code.message(), path.parent_path().string()});
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output << value;
    if (!output)
        return std::unexpected(error{errc::io, "cannot write Git ref", path.string()});
    return {};
}

auto octal(std::uint32_t value) -> std::string {
    auto output = std::ostringstream{};
    output << std::oct << value;
    return output.str();
}

auto write_tree(repository& source,
                const tree& value,
                const std::filesystem::path& target,
                std::set<std::string>& degradations) -> runtime::task<git_detail::sha1> {
    auto entries = std::vector<tree_entry>{value.entries.begin(), value.entries.end()};
    std::ranges::sort(entries, {}, &tree_entry::name);
    std::vector<std::byte> payload;
    for (const auto& entry : entries) {
        git_detail::sha1 id{};
        auto mode = entry.mode;
        if (entry.kind == node_kind::tree) {
            const auto nested = co_await source.tree(entry.ref);
            if (!nested)
                co_return std::unexpected(nested.error());
            const auto written = co_await write_tree(source, *nested, target, degradations);
            if (!written)
                co_return std::unexpected(written.error());
            id = *written;
            mode = 040000;
        } else if (entry.kind == node_kind::blob) {
            const auto blob = co_await source.blob(entry.ref);
            if (!blob)
                co_return std::unexpected(blob.error());
            const auto written =
                git_detail::write_loose_object(target, git_detail::object_kind::blob, *blob);
            if (!written)
                co_return std::unexpected(written.error());
            id = *written;
        } else {
            const auto empty = std::vector<std::byte>{};
            const auto written =
                git_detail::write_loose_object(target, git_detail::object_kind::tree, empty);
            if (!written)
                co_return std::unexpected(written.error());
            id = *written;
            mode = 040000;
            degradations.insert("nested repositories flattened as empty Git subtrees");
        }
        add(payload, octal(mode));
        add(payload, " ");
        add(payload, entry.name);
        payload.push_back(std::byte{});
        payload.insert(payload.end(), id.begin(), id.end());
    }
    const auto written =
        git_detail::write_loose_object(target, git_detail::object_kind::tree, payload);
    if (!written)
        co_return std::unexpected(written.error());
    co_return *written;
}

auto git_author(const actor& actor, std::int64_t at) -> std::string {
    const auto prefix = std::string_view{"git:"};
    const auto identity = actor.taap_id.starts_with(prefix) ? actor.taap_id.substr(prefix.size())
                                                            : "liquid <liquid@local>";
    return identity + " " + std::to_string(at) + " +0000";
}

} // namespace

runtime::task<export_report>
export_git(repository& source, std::filesystem::path target, std::string ref) {
    export_report report{};
    const auto refs = co_await source.refs();
    if (!refs)
        co_return std::unexpected(refs.error());
    const auto selected =
        std::ranges::find_if(*refs, [&](const auto& value) { return value.name == ref; });
    if (selected == refs->end())
        co_return std::unexpected(error{errc::not_found, "liquid ref is not present", ref});
    std::error_code code;
    std::filesystem::create_directories(target / "objects", code);
    if (code)
        co_return std::unexpected(error{errc::io, code.message(), target.string()});
    const auto head = write_text(target / "HEAD", "ref: refs/heads/" + ref + "\n");
    if (!head)
        co_return std::unexpected(head.error());
    const auto history = co_await source.log({.from = selected->id});
    if (!history)
        co_return std::unexpected(history.error());
    auto ordered = *history;
    std::ranges::reverse(ordered);
    std::map<std::string, git_detail::sha1> ids;
    std::set<std::string> degradations;
    for (const auto& change : ordered) {
        const auto snapshot = co_await source.at(change.id);
        if (!snapshot)
            co_return std::unexpected(snapshot.error());
        const auto tree = co_await write_tree(source, snapshot->root, target, degradations);
        if (!tree)
            co_return std::unexpected(tree.error());
        std::string message = change.message;
        if (!message.ends_with('\n'))
            message.push_back('\n');
        message += "\nLiquid-Change: " + to_string(change.id.value) + "\n";
        const auto author = git_author(change.author, change.at.time_since_epoch().count());
        auto body = std::string{"tree "} + git_detail::sha1_hex(*tree) + "\n";
        for (const auto& parent : change.parents) {
            const auto found = ids.find(to_string(parent.id.value));
            if (found == ids.end()) {
                degradations.insert("cross-repository parents dropped");
                continue;
            }
            body += "parent " + git_detail::sha1_hex(found->second) + "\n";
        }
        body += "author " + author + "\n";
        body += "committer " + author + "\n\n";
        body += message;
        const auto payload =
            std::vector<std::byte>{reinterpret_cast<const std::byte*>(body.data()),
                                   reinterpret_cast<const std::byte*>(body.data() + body.size())};
        const auto written =
            git_detail::write_loose_object(target, git_detail::object_kind::commit, payload);
        if (!written)
            co_return std::unexpected(written.error());
        ids.emplace(to_string(change.id.value), *written);
        ++report.commits;
        if (!change.conflicts.empty())
            degradations.insert("conflict values dropped");
        if (!change.sig.empty())
            degradations.insert("liquid signatures are not representable in Git");
    }
    const auto final = ids.find(to_string(selected->id.value));
    if (final == ids.end())
        co_return std::unexpected(error{errc::internal, "exported ref is missing", ref});
    const auto saved =
        write_text(target / "refs" / "heads" / ref, git_detail::sha1_hex(final->second) + "\n");
    if (!saved)
        co_return std::unexpected(saved.error());
    report.degradations.assign(degradations.begin(), degradations.end());
    co_return report;
}

} // namespace liquid
