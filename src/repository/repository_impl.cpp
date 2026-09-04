#include "liquid/repository.hpp"
#include "liquid/store.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <string_view>
#include <utility>

#include "crdt/document.hpp"
#include "encoding/cbor.hpp"
#include "objects/records.hpp"
#include "repository/merge.hpp"
#include "repository/testing.hpp"
#include "store/pack.hpp"

namespace liquid {
namespace {
namespace cbor = encoding::cbor;
enum class entry_kind : std::uint8_t { file, directory, nested_repository };
struct working_entry {
  entry_kind kind{entry_kind::file};
  node_id node{};
  std::uint32_t mode{0100644};
  digest object{algorithm::blake3, {}};
  std::map<std::string, working_entry> entries;
};
struct operation_record {
  op_id id{};
  std::string kind;
  std::string from;
  std::string to;
  std::optional<working_entry> prior;
};
struct repository_document {
  node_id repository{};
  working_entry root{entry_kind::directory, {}, 0040755, {algorithm::blake3, {}}, {}};
  std::map<std::string, change_id> refs;
  std::map<node_id, digest> dirs;
  std::optional<change_id> head;
  std::vector<operation_record> operations;
  std::vector<std::string> locks;
  std::vector<conflict> conflict_values;
  std::uint64_t pending_drips{};
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
  return {code, std::move(message), std::move(context)};
}
auto bytes(std::span<const std::byte> value) -> cbor::value {
  return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}
auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
  return cbor::value{cbor::value::map{values}};
}
auto digest_value(const digest &value) -> cbor::value {
  return map(
      {{"alg", cbor::value{static_cast<std::uint64_t>(value.alg)}}, {"bytes", bytes(value.bytes)}});
}
auto find(const cbor::value &value, std::string_view key) -> const cbor::value * {
  const auto *fields = std::get_if<cbor::value::map>(&value.data);
  if (!fields)
    return nullptr;
  for (const auto &[name, item] : *fields)
    if (const auto *text = std::get_if<std::string>(&name.data); text && *text == key)
      return &item;
  return nullptr;
}
auto decode_digest(const cbor::value &value) -> result<digest> {
  const auto *alg = find(value, "alg");
  const auto *raw = find(value, "bytes");
  const auto *number = alg ? std::get_if<std::uint64_t>(&alg->data) : nullptr;
  const auto *data = raw ? std::get_if<cbor::value::bytes>(&raw->data) : nullptr;
  if (!number || !data || data->size() != 32 || *number < 1 || *number > 3)
    return std::unexpected(failure(errc::corrupt, "invalid digest", "repository document"));
  digest output{static_cast<algorithm>(*number), {}};
  std::ranges::copy(*data, output.bytes.begin());
  return output;
}
auto encode_entry(const working_entry &entry) -> cbor::value {
  auto children = cbor::value::array{};
  for (const auto &[name, child] : entry.entries)
    children.push_back(map({{"name", cbor::value{name}}, {"entry", encode_entry(child)}}));
  return map({{"kind", cbor::value{static_cast<std::uint64_t>(entry.kind)}},
              {"node", bytes(entry.node)},
              {"mode", cbor::value{std::uint64_t{entry.mode}}},
              {"object", digest_value(entry.object)},
              {"entries", cbor::value{std::move(children)}}});
}
auto decode_entry(const cbor::value &value) -> result<working_entry> {
  const auto *kind = find(value, "kind");
  const auto *node = find(value, "node");
  const auto *mode = find(value, "mode");
  const auto *object = find(value, "object");
  const auto *children = find(value, "entries");
  const auto *type = kind ? std::get_if<std::uint64_t>(&kind->data) : nullptr;
  const auto *identity = node ? std::get_if<cbor::value::bytes>(&node->data) : nullptr;
  const auto *bits = mode ? std::get_if<std::uint64_t>(&mode->data) : nullptr;
  const auto *items = children ? std::get_if<cbor::value::array>(&children->data) : nullptr;
  if (!type || *type > 2 || !identity || identity->size() != 16 || !bits || *bits > UINT32_MAX ||
      !object || !items)
    return std::unexpected(
        failure(errc::corrupt, "invalid directory entry", "repository document"));
  auto reference = decode_digest(*object);
  if (!reference)
    return std::unexpected(reference.error());
  working_entry output{
      static_cast<entry_kind>(*type), {}, static_cast<std::uint32_t>(*bits), *reference, {}};
  std::ranges::copy(*identity, output.node.begin());
  for (const auto &child : *items) {
    const auto *name = find(child, "name");
    const auto *nested = find(child, "entry");
    const auto *text = name ? std::get_if<std::string>(&name->data) : nullptr;
    if (!text || !nested || output.entries.contains(*text))
      return std::unexpected(
          failure(errc::corrupt, "invalid directory child", "repository document"));
    auto decoded = decode_entry(*nested);
    if (!decoded)
      return std::unexpected(decoded.error());
    output.entries.emplace(*text, std::move(*decoded));
  }
  return output;
}
auto encode_repository_document(const repository_document &value)
    -> result<std::vector<std::byte>> {
  auto refs = cbor::value::array{};
  for (const auto &[name, id] : value.refs)
    refs.push_back(map({{"name", cbor::value{name}}, {"id", digest_value(id.value)}}));
  auto dirs = cbor::value::array{};
  for (const auto &[node, document] : value.dirs)
    dirs.push_back(map({{"node", bytes(node)}, {"document", digest_value(document)}}));
  auto ops = cbor::value::array{};
  for (const auto &operation : value.operations)
    ops.push_back(map(
        {{"id", digest_value(operation.id.value)},
         {"kind", cbor::value{operation.kind}},
         {"from", cbor::value{operation.from}},
         {"to", cbor::value{operation.to}},
         {"prior", operation.prior ? encode_entry(*operation.prior) : cbor::value{cbor::null{}}}}));
  auto locks = cbor::value::array{};
  for (const auto &lock : value.locks)
    locks.push_back(cbor::value{lock});
  auto head = value.head ? digest_value(value.head->value) : cbor::value{cbor::null{}};
  return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                           {"kind", "repository-document"},
                           {"repo", bytes(value.repository)},
                           {"root", encode_entry(value.root)},
                           {"refs", cbor::value{std::move(refs)}},
                           {"dirs", cbor::value{std::move(dirs)}},
                           {"ops", cbor::value{std::move(ops)}},
                           {"head", std::move(head)},
                           {"locks", cbor::value{std::move(locks)}},
                           {"pending", cbor::value{value.pending_drips}}}));
}
auto decode_repository_document(std::span<const std::byte> input) -> result<repository_document> {
  auto decoded = cbor::decode(input);
  if (!decoded)
    return std::unexpected(decoded.error());
  const auto *version = find(*decoded, "v");
  const auto *kind = find(*decoded, "kind");
  const auto *repo = find(*decoded, "repo");
  const auto *root = find(*decoded, "root");
  const auto *refs = find(*decoded, "refs");
  const auto *dirs = find(*decoded, "dirs");
  const auto *ops = find(*decoded, "ops");
  const auto *head = find(*decoded, "head");
  const auto *locks = find(*decoded, "locks");
  const auto *pending = find(*decoded, "pending");
  const auto *v = version ? std::get_if<std::uint64_t>(&version->data) : nullptr;
  const auto *title = kind ? std::get_if<std::string>(&kind->data) : nullptr;
  const auto *id = repo ? std::get_if<cbor::value::bytes>(&repo->data) : nullptr;
  const auto *ref_values = refs ? std::get_if<cbor::value::array>(&refs->data) : nullptr;
  const auto *dir_values = dirs ? std::get_if<cbor::value::array>(&dirs->data) : nullptr;
  const auto *op_values = ops ? std::get_if<cbor::value::array>(&ops->data) : nullptr;
  const auto *lock_values = locks ? std::get_if<cbor::value::array>(&locks->data) : nullptr;
  const auto *drips = pending ? std::get_if<std::uint64_t>(&pending->data) : nullptr;
  if (!v || *v != 1 || !title || *title != "repository-document" || !id || id->size() != 16 ||
      !root || !ref_values || !dir_values || !op_values || !head || !lock_values || !drips)
    return std::unexpected(failure(errc::corrupt, "invalid repository document", "LIQUID_REPO"));
  auto root_entry = decode_entry(*root);
  if (!root_entry)
    return std::unexpected(root_entry.error());
  repository_document output{};
  std::ranges::copy(*id, output.repository.begin());
  output.root = std::move(*root_entry);
  output.pending_drips = *drips;
  for (const auto &item : *ref_values) {
    const auto *name = find(item, "name");
    const auto *reference = find(item, "id");
    const auto *text = name ? std::get_if<std::string>(&name->data) : nullptr;
    if (!text || !reference || output.refs.contains(*text))
      return std::unexpected(failure(errc::corrupt, "invalid reference", "repository document"));
    auto digest = decode_digest(*reference);
    if (!digest)
      return std::unexpected(digest.error());
    output.refs.emplace(*text, change_id{*digest});
  }
  for (const auto &item : *dir_values) {
    const auto *node = find(item, "node");
    const auto *document = find(item, "document");
    const auto *raw = node ? std::get_if<cbor::value::bytes>(&node->data) : nullptr;
    if (!raw || raw->size() != 16 || !document)
      return std::unexpected(
          failure(errc::corrupt, "invalid directory reference", "repository document"));
    node_id identity{};
    std::ranges::copy(*raw, identity.begin());
    auto digest = decode_digest(*document);
    if (!digest)
      return std::unexpected(digest.error());
    output.dirs.emplace(identity, *digest);
  }
  for (const auto &item : *op_values) {
    const auto *operation_id = find(item, "id");
    const auto *operation_kind = find(item, "kind");
    const auto *from = find(item, "from");
    const auto *to = find(item, "to");
    const auto *prior = find(item, "prior");
    const auto *kind_text =
        operation_kind ? std::get_if<std::string>(&operation_kind->data) : nullptr;
    const auto *from_text = from ? std::get_if<std::string>(&from->data) : nullptr;
    const auto *to_text = to ? std::get_if<std::string>(&to->data) : nullptr;
    if (!operation_id || !kind_text || !from_text || !to_text || !prior)
      return std::unexpected(failure(errc::corrupt, "invalid operation", "repository document"));
    auto digest = decode_digest(*operation_id);
    if (!digest)
      return std::unexpected(digest.error());
    auto previous = std::optional<working_entry>{};
    if (!std::holds_alternative<cbor::null>(prior->data)) {
      auto entry = decode_entry(*prior);
      if (!entry)
        return std::unexpected(entry.error());
      previous = std::move(*entry);
    }
    output.operations.push_back(
        {op_id{*digest}, *kind_text, *from_text, *to_text, std::move(previous)});
  }
  if (!std::holds_alternative<cbor::null>(head->data)) {
    auto digest = decode_digest(*head);
    if (!digest)
      return std::unexpected(digest.error());
    output.head = change_id{*digest};
  }
  for (const auto &item : *lock_values) {
    const auto *text = std::get_if<std::string>(&item.data);
    if (!text)
      return std::unexpected(failure(errc::corrupt, "invalid lock", "repository document"));
    output.locks.push_back(*text);
  }
  return output;
}
auto random_id() -> node_id {
  auto result = node_id{};
  auto random = std::random_device{};
  std::ranges::generate(result, [&] { return std::byte{static_cast<unsigned char>(random())}; });
  return result;
}
auto split_path(std::string_view path) -> result<std::vector<std::string>> {
  if (path.empty() || path.front() == '/' || path.back() == '/')
    return std::unexpected(
        failure(errc::invalid_argument, "path must be relative and non-empty", std::string{path}));
  std::vector<std::string> result;
  for (std::size_t start = 0; start < path.size();) {
    const auto end = path.find('/', start);
    const auto part =
        path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
    if (part.empty() || part == "." || part == "..")
      return std::unexpected(
          failure(errc::invalid_argument, "invalid path component", std::string{path}));
    result.emplace_back(part);
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return result;
}
auto parent_for(working_entry &root, const std::vector<std::string> &path, bool create)
    -> result<working_entry *> {
  working_entry *current = &root;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    auto found = current->entries.find(path[i]);
    if (found == current->entries.end()) {
      if (!create)
        return std::unexpected(failure(errc::not_found, "directory does not exist", path[i]));
      found = current->entries
                  .emplace(
                      path[i],
                      working_entry{
                          entry_kind::directory, random_id(), 0040755, {algorithm::blake3, {}}, {}})
                  .first;
    }
    if (found->second.kind != entry_kind::directory)
      return std::unexpected(
          failure(errc::invalid_argument, "path component is not a directory", path[i]));
    current = &found->second;
  }
  return current;
}
auto lookup(working_entry &root, const std::vector<std::string> &path) -> working_entry * {
  working_entry *current = &root;
  for (const auto &part : path) {
    auto found = current->entries.find(part);
    if (found == current->entries.end())
      return nullptr;
    current = &found->second;
  }
  return current;
}
auto same_path_or_descendant(std::string_view path, std::string_view root) -> bool {
  return path == root ||
         (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/');
}
} // namespace

struct repository::impl {
  std::filesystem::path root_path;
  store objects;
  repository_document current;
  std::uint64_t checkpoint_interval{64};
  std::uint64_t directory_load_count{};
  std::map<node_id, crdt::directory_document> directory_cache;
  impl(std::filesystem::path path, store value, repository_document document,
       std::uint64_t interval = 64)
      : root_path(std::move(path)), objects(std::move(value)), current(std::move(document)),
        checkpoint_interval(std::max(std::uint64_t{1}, interval)) {}
  auto marker() const -> std::filesystem::path { return root_path / "LIQUID_REPO"; }
  auto journal_marker() const -> std::filesystem::path { return root_path / "LIQUID_JOURNAL"; }
  runtime::task<crdt::directory_document *> directory(node_id id) {
    auto found = directory_cache.find(id);
    if (found != directory_cache.end())
      co_return &found->second;
    if (auto saved = current.dirs.find(id); saved != current.dirs.end()) {
      auto raw = co_await objects.get(saved->second);
      if (!raw)
        co_return std::unexpected(raw.error());
      auto loaded = crdt::directory_document::load(*raw, "local", "repository");
      if (!loaded)
        co_return std::unexpected(loaded.error());
      auto inserted = directory_cache.emplace(id, std::move(*loaded));
      ++directory_load_count;
      co_return &inserted.first->second;
    }
    crdt::repo_id repository{};
    std::ranges::copy(current.repository, repository.begin());
    auto created = crdt::directory_document::create(repository, "local", "repository");
    if (!created)
      co_return std::unexpected(created.error());
    auto inserted = directory_cache.emplace(id, std::move(*created));
    co_return &inserted.first->second;
  }
  runtime::task<void> persist() {
    for (auto &[node, document] : directory_cache) {
      auto saved_document = document.save();
      if (!saved_document)
        co_return std::unexpected(saved_document.error());
      auto saved_id = co_await objects.put(std::move(*saved_document));
      if (!saved_id)
        co_return std::unexpected(saved_id.error());
      current.dirs[node] = *saved_id;
    }
    auto encoded = encode_repository_document(current);
    if (!encoded)
      co_return std::unexpected(encoded.error());
    auto stored = co_await objects.put(std::move(*encoded));
    if (!stored)
      co_return std::unexpected(stored.error());
    auto temporary = marker();
    temporary += ".tmp";
    const auto text = to_string(*stored) + "\n";
    auto marker_bytes =
        std::vector<std::byte>{reinterpret_cast<const std::byte *>(text.data()),
                               reinterpret_cast<const std::byte *>(text.data() + text.size())};
    auto written = co_await store_detail::write_file(temporary, std::move(marker_bytes));
    if (!written)
      co_return std::unexpected(written.error());
    auto code = std::error_code{};
    std::filesystem::rename(temporary, marker(), code);
    if (code) {
      std::filesystem::remove(marker(), code);
      code.clear();
      std::filesystem::rename(temporary, marker(), code);
    }
    if (code)
      co_return std::unexpected(failure(errc::io, code.message(), marker().string()));
    co_return result<void>{};
  }
  runtime::task<void> checkpoint_or_journal() {
    if (current.pending_drips >= checkpoint_interval)
      co_return co_await persist();
    auto encoded = encode_repository_document(current);
    if (!encoded)
      co_return std::unexpected(encoded.error());
    co_return co_await store_detail::write_file(journal_marker(), std::move(*encoded));
  }
  runtime::task<digest> snapshot(working_entry &entry) {
    tree value{};
    for (auto &[name, child] : entry.entries) {
      digest reference = child.object;
      node_kind kind = node_kind::blob;
      if (child.kind == entry_kind::directory) {
        auto nested = co_await snapshot(child);
        if (!nested)
          co_return std::unexpected(nested.error());
        reference = *nested;
        child.object = reference;
        kind = node_kind::tree;
      }
      if (child.kind == entry_kind::nested_repository)
        kind = node_kind::repo;
      value.entries.push_back({name, child.mode, kind, reference, child.node, reference});
    }
    auto encoded = objects::encode(value);
    if (!encoded)
      co_return std::unexpected(encoded.error());
    co_return co_await objects.put(std::move(*encoded));
  }
  runtime::task<tree> root_tree() {
    auto snapshot_id = co_await snapshot(current.root);
    if (!snapshot_id)
      co_return std::unexpected(snapshot_id.error());
    auto saved = co_await objects.get(*snapshot_id);
    if (!saved)
      co_return std::unexpected(saved.error());
    co_return objects::decode_tree(*saved);
  }
  auto locked(std::string_view path) const -> bool {
    return std::ranges::any_of(
        current.locks, [&](const auto &pattern) { return same_path_or_descendant(path, pattern); });
  }
};
repository::repository() noexcept = default;
repository::~repository() = default;
repository::repository(repository &&) noexcept = default;
auto repository::operator=(repository &&) noexcept -> repository & = default;
runtime::task<repository> repository::create(std::filesystem::path path,
                                             repository_options options) {
  auto code = std::error_code{};
  std::filesystem::create_directories(path, code);
  if (code)
    co_return std::unexpected(failure(errc::io, code.message(), path.string()));
  if (std::filesystem::exists(path / "LIQUID_REPO"))
    co_return std::unexpected(
        failure(errc::already_exists, "repository already exists", path.string()));
  auto objects = co_await store::open(path / ".liquid-store");
  if (!objects)
    co_return std::unexpected(objects.error());
  repository_document document{};
  document.repository = random_id();
  document.root.node = random_id();
  repository output;
  output.impl_ = std::make_unique<impl>(std::move(path), std::move(*objects), std::move(document),
                                        options.checkpoint_interval);
  auto saved = co_await output.impl_->persist();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return output;
}
runtime::task<repository> repository::open(std::filesystem::path path) {
  auto marker = co_await store_detail::read_file(path / "LIQUID_REPO");
  if (!marker)
    co_return std::unexpected(marker.error());
  auto text = std::string{reinterpret_cast<const char *>(marker->data()), marker->size()};
  if (const auto newline = text.find('\n'); newline != std::string::npos)
    text.resize(newline);
  auto document_id = parse_digest(text);
  if (!document_id)
    co_return std::unexpected(document_id.error());
  auto objects = co_await store::open(path / ".liquid-store");
  if (!objects)
    co_return std::unexpected(objects.error());
  auto saved = co_await objects->get(*document_id);
  if (!saved)
    co_return std::unexpected(saved.error());
  auto document = decode_repository_document(*saved);
  if (!document)
    co_return std::unexpected(document.error());
  if (std::filesystem::exists(path / "LIQUID_JOURNAL")) {
    auto journal = co_await store_detail::read_file(path / "LIQUID_JOURNAL");
    if (!journal)
      co_return std::unexpected(journal.error());
    auto replayed = decode_repository_document(*journal);
    if (!replayed)
      co_return std::unexpected(replayed.error());
    document = std::move(replayed);
  }
  repository output;
  output.impl_ = std::make_unique<impl>(std::move(path), std::move(*objects), std::move(*document));
  co_return output;
}
runtime::task<tree> repository::root() {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  co_return co_await impl_->root_tree();
}
runtime::task<change_id> repository::head() {
  if (!impl_ || !impl_->current.head)
    co_return std::unexpected(failure(errc::not_found, "repository has no change"));
  co_return *impl_->current.head;
}
runtime::task<std::vector<std::byte>> repository::read(std::string path) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  auto *entry = lookup(impl_->current.root, *parts);
  if (!entry || entry->kind != entry_kind::file)
    co_return std::unexpected(failure(errc::not_found, "file does not exist", path));
  auto encoded = co_await impl_->objects.get(entry->object);
  if (!encoded)
    co_return std::unexpected(encoded.error());
  auto blob = objects::decode_blob(*encoded);
  if (!blob)
    co_return std::unexpected(blob.error());
  std::vector<std::byte> output;
  output.reserve(static_cast<std::size_t>(blob->size));
  for (const auto &chunk : blob->chunks) {
    auto content = co_await impl_->objects.get(chunk);
    if (!content)
      co_return std::unexpected(content.error());
    output.insert(output.end(), content->begin(), content->end());
  }
  if (output.size() != blob->size)
    co_return std::unexpected(failure(errc::corrupt, "blob size does not match content", path));
  co_return output;
}
runtime::task<void> repository::write(std::string path, std::vector<std::byte> contents) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  if (impl_->locked(path))
    co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  const auto size = contents.size();
  auto content = co_await impl_->objects.put(std::move(contents));
  if (!content)
    co_return std::unexpected(content.error());
  blob record{{*content}, size};
  auto encoded = objects::encode(record);
  if (!encoded)
    co_return std::unexpected(encoded.error());
  auto reference = co_await impl_->objects.put(std::move(*encoded));
  if (!reference)
    co_return std::unexpected(reference.error());
  auto parent = parent_for(impl_->current.root, *parts, true);
  if (!parent)
    co_return std::unexpected(parent.error());
  auto found = (*parent)->entries.find(parts->back());
  std::optional<working_entry> prior;
  if (found != (*parent)->entries.end() && found->second.kind != entry_kind::file)
    co_return std::unexpected(failure(errc::invalid_argument, "cannot overwrite directory", path));
  auto directory = co_await impl_->directory((*parent)->node);
  if (!directory)
    co_return std::unexpected(directory.error());
  const auto existing_id = found == (*parent)->entries.end() ? node_id{} : found->second.node;
  auto recorded =
      (*directory)->put_entry(parts->back(), {"blob", existing_id, 0100644, to_string(*reference)});
  if (!recorded)
    co_return std::unexpected(recorded.error());
  if (found != (*parent)->entries.end()) {
    prior = found->second;
    found->second.object = *reference;
  } else
    found = (*parent)
                ->entries
                .emplace(parts->back(),
                         working_entry{entry_kind::file, random_id(), 0100644, *reference, {}})
                .first;
  auto operation_hash = hash(algorithm::blake3, std::span<const std::byte>{reference->bytes});
  if (!operation_hash)
    co_return std::unexpected(operation_hash.error());
  impl_->current.operations.push_back({op_id{*operation_hash}, "write", path, {}, prior});
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<void> repository::move(std::string from, std::string to) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  if (impl_->locked(from) || impl_->locked(to))
    co_return std::unexpected(failure(errc::locked, "path is advisory locked", from));
  auto source = split_path(from);
  auto destination = split_path(to);
  if (!source)
    co_return std::unexpected(source.error());
  if (!destination)
    co_return std::unexpected(destination.error());
  if (to.starts_with(from + "/"))
    co_return std::unexpected(
        failure(errc::invalid_argument, "move destination is below source", to));
  auto source_parent = parent_for(impl_->current.root, *source, false);
  auto destination_parent = parent_for(impl_->current.root, *destination, false);
  if (!source_parent || !destination_parent)
    co_return std::unexpected(failure(errc::not_found, "move parent does not exist", from));
  auto found = (*source_parent)->entries.find(source->back());
  if (found == (*source_parent)->entries.end())
    co_return std::unexpected(failure(errc::not_found, "move source does not exist", from));
  if ((*destination_parent)->entries.contains(destination->back()))
    co_return std::unexpected(failure(errc::already_exists, "move destination exists", to));
  auto source_document = co_await impl_->directory((*source_parent)->node);
  if (!source_document)
    co_return std::unexpected(source_document.error());
  if ((*source_parent)->node == (*destination_parent)->node) {
    auto recorded = (*source_document)->rename_entry(source->back(), destination->back());
    if (!recorded)
      co_return std::unexpected(recorded.error());
  } else {
    auto destination_document = co_await impl_->directory((*destination_parent)->node);
    if (!destination_document)
      co_return std::unexpected(destination_document.error());
    auto removed = (*source_document)->remove_entry(source->back());
    if (!removed)
      co_return std::unexpected(removed.error());
    auto inserted =
        (*destination_document)
            ->put_entry(destination->back(), {"blob", found->second.node, found->second.mode,
                                              to_string(found->second.object)});
    if (!inserted)
      co_return std::unexpected(inserted.error());
  }
  auto moved = std::move(found->second);
  (*source_parent)->entries.erase(found);
  (*destination_parent)->entries.emplace(destination->back(), std::move(moved));
  auto hash_value = hash(algorithm::blake3, std::span<const std::byte>{impl_->current.root.node});
  if (!hash_value)
    co_return std::unexpected(hash_value.error());
  impl_->current.operations.push_back({op_id{*hash_value}, "move", from, to, std::nullopt});
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<void> repository::remove(std::string path) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  if (impl_->locked(path))
    co_return std::unexpected(failure(errc::locked, "path is advisory locked", path));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  auto parent = parent_for(impl_->current.root, *parts, false);
  if (!parent)
    co_return std::unexpected(parent.error());
  auto found = (*parent)->entries.find(parts->back());
  if (found == (*parent)->entries.end())
    co_return std::unexpected(failure(errc::not_found, "path does not exist", path));
  auto directory = co_await impl_->directory((*parent)->node);
  if (!directory)
    co_return std::unexpected(directory.error());
  auto recorded = (*directory)->remove_entry(parts->back());
  if (!recorded)
    co_return std::unexpected(recorded.error());
  auto prior = found->second;
  (*parent)->entries.erase(found);
  auto hash_value = hash(algorithm::blake3, std::span<const std::byte>{prior.node});
  if (!hash_value)
    co_return std::unexpected(hash_value.error());
  impl_->current.operations.push_back({op_id{*hash_value}, "remove", path, {}, prior});
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<change_id> repository::cut(std::string message) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto root = co_await impl_->snapshot(impl_->current.root);
  if (!root)
    co_return std::unexpected(root.error());
  change value{};
  if (impl_->current.head)
    value.parents = {*impl_->current.head};
  value.author = actor{"local"};
  value.message = std::move(message);
  value.at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  value.root = *root;
  value.conflicts = impl_->current.conflict_values;
  auto encoded = objects::encode(value);
  if (!encoded)
    co_return std::unexpected(encoded.error());
  auto stored = co_await impl_->objects.put(std::move(*encoded));
  if (!stored)
    co_return std::unexpected(stored.error());
  change_id id{*stored};
  impl_->current.head = id;
  impl_->current.refs["main"] = id;
  impl_->current.pending_drips = 0;
  auto saved = co_await impl_->persist();
  if (!saved)
    co_return std::unexpected(saved.error());
  std::error_code cleanup;
  std::filesystem::remove(impl_->journal_marker(), cleanup);
  co_return id;
}
runtime::task<std::vector<ref>> repository::refs() {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  std::vector<ref> output;
  for (const auto &[name, id] : impl_->current.refs)
    output.push_back({name, id});
  co_return output;
}
runtime::task<void> repository::set_ref(std::string name, change_id id) {
  if (!impl_ || name.empty())
    co_return std::unexpected(
        failure(errc::invalid_argument, "invalid repository reference", name));
  impl_->current.refs[std::move(name)] = id;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
result<repository_status> repository::status() {
  if (!impl_)
    return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  repository_status output{};
  output.pending_drips = impl_->current.pending_drips;
  output.conflicts = impl_->current.conflict_values;
  output.head = impl_->current.head;
  auto visit = [&](auto &&self, const working_entry &entry, std::string path) -> void {
    if (entry.kind == entry_kind::directory && entry.entries.size() > 10000)
      output.oversized_directories.push_back(path);
    for (const auto &[name, child] : entry.entries)
      self(self, child, path.empty() ? name : path + "/" + name);
  };
  visit(visit, impl_->current.root, "");
  return output;
}
runtime::task<std::vector<change>> repository::log(log_options) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  std::vector<change> output;
  auto next = impl_->current.head;
  while (next) {
    auto bytes = co_await impl_->objects.get(next->value);
    if (!bytes)
      co_return std::unexpected(bytes.error());
    auto value = objects::decode_change(*bytes);
    if (!value)
      co_return std::unexpected(value.error());
    output.push_back(*value);
    next = value->parents.empty() ? std::optional<change_id>{}
                                  : std::optional<change_id>{value->parents.front()};
  }
  co_return output;
}
runtime::task<snapshot> repository::at(change_id id) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto bytes = co_await impl_->objects.get(id.value);
  if (!bytes)
    co_return std::unexpected(bytes.error());
  auto change = objects::decode_change(*bytes);
  if (!change)
    co_return std::unexpected(change.error());
  auto tree_bytes = co_await impl_->objects.get(change->root);
  if (!tree_bytes)
    co_return std::unexpected(tree_bytes.error());
  auto tree = objects::decode_tree(*tree_bytes);
  if (!tree)
    co_return std::unexpected(tree.error());
  co_return snapshot{id, *tree};
}
runtime::task<void> repository::nest(repository source, std::string path) {
  if (!impl_ || !source.impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  auto parent = parent_for(impl_->current.root, *parts, true);
  if (!parent)
    co_return std::unexpected(parent.error());
  if ((*parent)->entries.contains(parts->back()))
    co_return std::unexpected(failure(errc::already_exists, "nest destination exists", path));
  auto nested = source.impl_->current.root;
  nested.kind = entry_kind::nested_repository;
  (*parent)->entries.emplace(parts->back(), std::move(nested));
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<repository> repository::split(std::string path, std::filesystem::path into) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  auto *entry = lookup(impl_->current.root, *parts);
  if (!entry || entry->kind != entry_kind::directory)
    co_return std::unexpected(failure(errc::not_found, "subtree does not exist", path));
  auto destination = co_await create(std::move(into));
  if (!destination)
    co_return std::unexpected(destination.error());
  destination->impl_->current.root = *entry;
  auto child_change = co_await destination->cut("split subtree");
  if (!child_change)
    co_return std::unexpected(child_change.error());
  entry->kind = entry_kind::nested_repository;
  entry->object = child_change->value;
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return std::move(*destination);
}
runtime::task<void> repository::combine(repository other, std::optional<std::string> at) {
  if (!impl_ || !other.impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  const auto path = at.value_or("combined");
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  auto parent = parent_for(impl_->current.root, *parts, true);
  if (!parent)
    co_return std::unexpected(parent.error());
  if ((*parent)->entries.contains(parts->back())) {
    if (auto value = repository_detail::entry_conflict(
            path, (*parent)->entries.at(parts->back()).object, other.impl_->current.root.object))
      impl_->current.conflict_values.push_back(std::move(*value));
  } else {
    auto copy = other.impl_->current.root;
    copy.kind = entry_kind::nested_repository;
    (*parent)->entries.emplace(parts->back(), std::move(copy));
  }
  ++impl_->current.pending_drips;
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<std::vector<conflict>> repository::conflicts() {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  co_return impl_->current.conflict_values;
}
runtime::task<void> repository::undo(op_id operation) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto found = std::ranges::find_if(impl_->current.operations, [&](const auto &value) {
    return value.id.value.alg == operation.value.alg &&
           value.id.value.bytes == operation.value.bytes;
  });
  if (found == impl_->current.operations.end())
    co_return std::unexpected(failure(errc::not_found, "operation does not exist"));
  if (found->kind == "move") {
    auto result = co_await move(found->to, found->from);
    if (!result)
      co_return std::unexpected(result.error());
  } else if (found->kind == "remove" && found->prior) {
    auto parts = split_path(found->from);
    if (!parts)
      co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, true);
    if (!parent)
      co_return std::unexpected(parent.error());
    (*parent)->entries[parts->back()] = *found->prior;
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
      co_return std::unexpected(saved.error());
  } else if (found->kind == "write") {
    auto parts = split_path(found->from);
    if (!parts)
      co_return std::unexpected(parts.error());
    auto parent = parent_for(impl_->current.root, *parts, false);
    if (!parent)
      co_return std::unexpected(parent.error());
    if (found->prior)
      (*parent)->entries[parts->back()] = *found->prior;
    else
      (*parent)->entries.erase(parts->back());
    ++impl_->current.pending_drips;
    auto saved = co_await impl_->checkpoint_or_journal();
    if (!saved)
      co_return std::unexpected(saved.error());
  } else
    co_return std::unexpected(failure(errc::unsupported, "operation cannot be undone"));
  co_return result<void>{};
}
runtime::task<void> repository::lock(std::string path) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  auto parts = split_path(path);
  if (!parts)
    co_return std::unexpected(parts.error());
  if (std::ranges::find(impl_->current.locks, path) == impl_->current.locks.end())
    impl_->current.locks.push_back(std::move(path));
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}
runtime::task<void> repository::unlock(std::string path) {
  if (!impl_)
    co_return std::unexpected(failure(errc::invalid_argument, "repository is empty"));
  std::erase(impl_->current.locks, path);
  auto saved = co_await impl_->checkpoint_or_journal();
  if (!saved)
    co_return std::unexpected(saved.error());
  co_return result<void>{};
}

auto repository_detail::testing::directory_loads(repository &value) -> std::uint64_t {
  return value.impl_ ? value.impl_->directory_load_count : 0;
}

auto repository_detail::testing::last_operation(repository &value) -> op_id {
  return value.impl_ && !value.impl_->current.operations.empty()
             ? value.impl_->current.operations.back().id
             : op_id{};
}

void repository_detail::testing::reset_directory_loads(repository &value) {
  if (value.impl_)
    value.impl_->directory_load_count = 0;
}
} // namespace liquid
