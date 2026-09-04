#include "build/cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#include "build/repository_access.hpp"
#include "identity/taap.hpp"
#include "store/pack.hpp"

namespace liquid::build_detail {
namespace {

constexpr std::array<std::byte, 5> index_magic{
    std::byte{'L'}, std::byte{'Q'}, std::byte{'A'}, std::byte{'C'}, std::byte{'T'}};
constexpr std::uint8_t current_index_version = 2;
constexpr std::size_t index_header_size = 64;
constexpr std::size_t legacy_index_record_size = 80;
constexpr std::size_t index_record_size = 114;

std::atomic_uint64_t sandbox_spawns{};

auto cache_error(errc code, std::string message, const std::filesystem::path& path) -> error {
    return {code, std::move(message), path.string()};
}

void put_u64(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index) {
        output.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
    }
}

auto get_u64(std::span<const std::byte> input, std::size_t offset) -> std::uint64_t {
    if (offset + 8 > input.size()) {
        return 0;
    }
    auto value = std::uint64_t{};
    for (unsigned index = 0; index != 8; ++index) {
        value |= std::uint64_t{std::to_integer<unsigned char>(input[offset + index])}
                 << (index * 8U);
    }
    return value;
}

auto checksum(std::span<const std::byte> input) -> std::uint64_t {
    auto value = std::uint64_t{1469598103934665603ULL};
    for (const auto byte : input) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

auto valid_algorithm(unsigned char value) -> bool {
    return value >= static_cast<unsigned char>(algorithm::blake3) &&
           value <= static_cast<unsigned char>(algorithm::blake3_keyed);
}

auto all_zero(std::span<const std::byte> input) -> bool {
    return std::ranges::all_of(input, [](std::byte value) { return value == std::byte{0}; });
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto entry_less(const objects::action_index_entry& left, const objects::action_index_entry& right)
    -> bool {
    if (left.candidate.alg != right.candidate.alg) {
        return left.candidate.alg < right.candidate.alg;
    }
    if (left.candidate.bytes != right.candidate.bytes) {
        return left.candidate.bytes < right.candidate.bytes;
    }
    if (left.sequence != right.sequence) {
        return left.sequence > right.sequence;
    }
    if (left.result.alg != right.result.alg) {
        return left.result.alg < right.result.alg;
    }
    return left.result.bytes < right.result.bytes;
}

void append_digest(std::vector<std::byte>& output, const digest& value) {
    output.push_back(std::byte{static_cast<unsigned char>(value.alg)});
    output.insert(output.end(), value.bytes.begin(), value.bytes.end());
}

auto decode_digest(std::span<const std::byte> input, std::size_t offset) -> result<digest> {
    if (offset + 33 > input.size()) {
        return std::unexpected(error{errc::corrupt, "truncated action index digest", "actions"});
    }
    const auto algorithm_value = std::to_integer<unsigned char>(input[offset]);
    if (!valid_algorithm(algorithm_value)) {
        return std::unexpected(error{errc::corrupt, "invalid action index algorithm", "actions"});
    }
    auto output = digest{static_cast<algorithm>(algorithm_value), {}};
    std::ranges::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset + 1),
                        output.bytes.size(),
                        output.bytes.begin());
    return output;
}

auto encode_index(std::span<const objects::action_index_entry> entries,
                  digest root,
                  std::uint64_t next_sequence) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    output.reserve(index_header_size + entries.size() * index_record_size + 8);
    output.insert(output.end(), index_magic.begin(), index_magic.end());
    output.push_back(std::byte{current_index_version});
    output.insert(output.end(), 2, std::byte{0});
    put_u64(output, entries.size());
    put_u64(output, next_sequence);
    output.push_back(std::byte{static_cast<unsigned char>(root.alg)});
    output.insert(output.end(), 7, std::byte{0});
    output.insert(output.end(), root.bytes.begin(), root.bytes.end());
    for (const auto& entry : entries) {
        append_digest(output, entry.candidate);
        append_digest(output, entry.result);
        put_u64(output, entry.sequence);
        output.push_back(entry.divergent ? std::byte{1} : std::byte{0});
        if (entry.verification) {
            append_digest(output, *entry.verification);
        } else {
            output.insert(output.end(), 33, std::byte{0});
        }
        output.insert(output.end(), 6, std::byte{0});
    }
    put_u64(output, checksum(output));
    return output;
}

auto manifest_digest(repository& source, digest tree, std::string_view path)
    -> runtime::task<std::optional<digest>> {
    auto current = tree;
    auto offset = std::size_t{};
    while (offset < path.size()) {
        const auto separator = path.find('/', offset);
        const auto name = path.substr(offset,
                                      separator == std::string_view::npos ? path.size() - offset
                                                                          : separator - offset);
        auto bytes = co_await repository_detail::build_access::get(source, current);
        if (!bytes) {
            if (bytes.error().code == errc::not_found) {
                co_return std::nullopt;
            }
            co_return std::unexpected(bytes.error());
        }
        auto decoded = objects::decode_tree(*bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        const auto found = std::ranges::find(decoded->entries, name, &tree_entry::name);
        if (found == decoded->entries.end()) {
            co_return std::nullopt;
        }
        if (separator == std::string_view::npos) {
            if (found->kind != node_kind::blob) {
                co_return std::nullopt;
            }
            co_return found->ref;
        }
        if (found->kind != node_kind::tree) {
            co_return std::nullopt;
        }
        current = found->ref;
        offset = separator + 1;
    }
    co_return std::nullopt;
}

auto referenced_bytes(repository& source, const action_result& value)
    -> runtime::task<std::uint64_t> {
    auto ids = std::vector<digest>{value.stdout_output, value.stderr_output};
    for (const auto& output : value.outputs) {
        ids.push_back(output.blob);
    }
    auto seen = std::vector<digest>{};
    auto total = std::uint64_t{};
    for (const auto& id : ids) {
        if (std::ranges::any_of(seen, [&](const digest& item) { return same_digest(item, id); })) {
            continue;
        }
        seen.push_back(id);
        auto bytes = co_await repository_detail::build_access::get(source, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto blob = objects::decode_blob(*bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        if (std::numeric_limits<std::uint64_t>::max() - total < blob->size) {
            co_return std::unexpected(
                error{errc::internal, "cache byte count overflow", "build cache"});
        }
        total += blob->size;
    }
    co_return total;
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

} // namespace

action_index::action_index(std::filesystem::path directory) : directory_(std::move(directory)) {}

runtime::task<void> action_index::open() {
    entries_.clear();
    root_ = {};
    next_sequence_ = 1;
    const auto path = directory_ / "INDEX";
    if (!std::filesystem::exists(path)) {
        co_return result<void>{};
    }
    auto raw = co_await store_detail::read_file(path);
    if (!raw) {
        co_return std::unexpected(raw.error());
    }
    const auto bytes = std::span<const std::byte>{*raw};
    if (bytes.size() < index_header_size + 8 ||
        !std::equal(index_magic.begin(), index_magic.end(), bytes.begin()) ||
        bytes[6] != std::byte{0} || bytes[7] != std::byte{0}) {
        co_return std::unexpected(cache_error(errc::corrupt, "invalid action index", path));
    }
    const auto version = std::to_integer<std::uint8_t>(bytes[5]);
    if (version != 1 && version != current_index_version) {
        co_return std::unexpected(cache_error(errc::corrupt, "invalid action index", path));
    }
    const auto record_size = version == 1 ? legacy_index_record_size : index_record_size;
    const auto count = get_u64(bytes, 8);
    next_sequence_ = get_u64(bytes, 16);
    const auto maximum_records =
        (std::numeric_limits<std::size_t>::max() - index_header_size - 8) / record_size;
    if (count > maximum_records || next_sequence_ == 0 ||
        bytes.size() != index_header_size + static_cast<std::size_t>(count) * record_size + 8 ||
        checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
        co_return std::unexpected(cache_error(errc::corrupt, "corrupt action index", path));
    }
    const auto root_algorithm = std::to_integer<unsigned char>(bytes[24]);
    if ((root_algorithm != 0 && !valid_algorithm(root_algorithm)) ||
        !all_zero(bytes.subspan(25, 7))) {
        co_return std::unexpected(cache_error(errc::corrupt, "invalid action index root", path));
    }
    root_.alg = static_cast<algorithm>(root_algorithm);
    std::ranges::copy_n(bytes.begin() + 32, root_.bytes.size(), root_.bytes.begin());
    entries_.reserve(static_cast<std::size_t>(count));
    for (auto index = std::uint64_t{}; index != count; ++index) {
        const auto offset = index_header_size + static_cast<std::size_t>(index) * record_size;
        auto candidate = decode_digest(bytes, offset);
        auto result_id = decode_digest(bytes, offset + 33);
        const auto sequence = get_u64(bytes, offset + 66);
        const auto divergent = version == 2 && bytes[offset + 74] == std::byte{1};
        if (version == 2 && bytes[offset + 74] != std::byte{0} &&
            bytes[offset + 74] != std::byte{1}) {
            co_return std::unexpected(
                cache_error(errc::corrupt, "invalid action index entry", path));
        }
        auto verification = std::optional<digest>{};
        if (version == 2 && bytes[offset + 75] != std::byte{0}) {
            auto decoded = decode_digest(bytes, offset + 75);
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            verification = *decoded;
        } else if (version == 2 && !all_zero(bytes.subspan(offset + 75, 33))) {
            co_return std::unexpected(
                cache_error(errc::corrupt, "invalid action verification digest", path));
        }
        const auto reserved_offset = version == 1 ? offset + 74 : offset + 108;
        if (!candidate || !result_id || sequence == 0 ||
            !all_zero(bytes.subspan(reserved_offset, 6))) {
            co_return std::unexpected(
                cache_error(errc::corrupt, "invalid action index entry", path));
        }
        entries_.push_back({*candidate, *result_id, sequence, divergent, verification});
    }
    if (!std::ranges::is_sorted(entries_, entry_less)) {
        co_return std::unexpected(cache_error(errc::corrupt, "unsorted action index", path));
    }
    co_return result<void>{};
}

std::vector<digest> action_index::candidates(digest candidate) const {
    auto output = std::vector<digest>{};
    for (const auto& entry : entries_) {
        if (same_digest(entry.candidate, candidate)) {
            output.push_back(entry.result);
        }
    }
    return output;
}

result<void> action_index::insert(digest candidate, digest result_id) {
    const auto duplicate = std::ranges::any_of(entries_, [&](const auto& entry) {
        return same_digest(entry.candidate, candidate) && same_digest(entry.result, result_id);
    });
    if (duplicate) {
        return {};
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(
            error{errc::io, "action index sequence exhausted", directory_.string()});
    }
    entries_.push_back({candidate, result_id, next_sequence_++});
    std::ranges::sort(entries_, entry_less);
    return {};
}

runtime::task<void> action_index::rebuild(std::vector<objects::action_index_entry> entries,
                                          digest root) {
    entries_ = std::move(entries);
    std::ranges::sort(entries_, entry_less);
    next_sequence_ = 1;
    for (const auto& entry : entries_) {
        if (entry.sequence == 0 || entry.sequence == std::numeric_limits<std::uint64_t>::max()) {
            co_return std::unexpected(
                error{errc::corrupt, "invalid action index sequence", directory_.string()});
        }
        next_sequence_ = std::max(next_sequence_, entry.sequence + 1);
    }
    co_return co_await persist(root);
}

runtime::task<void> action_index::persist(digest root) {
    root_ = root;
    const auto path = directory_ / "INDEX";
    const auto temporary = directory_ / "INDEX.tmp";
    try {
        std::filesystem::create_directories(directory_);
    } catch (const std::system_error& caught) {
        co_return std::unexpected(cache_error(errc::io, caught.what(), directory_));
    }
    auto written =
        co_await store_detail::write_file(temporary, encode_index(entries_, root_, next_sequence_));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        co_return std::unexpected(cache_error(errc::io, code.message(), path));
    }
    co_return result<void>{};
}

const std::vector<objects::action_index_entry>& action_index::entries() const noexcept {
    return entries_;
}

digest action_index::root() const noexcept {
    return root_;
}

disk_cache::disk_cache(repository& source)
    : source_(source),
      index_(*repository_detail::build_access::root_path(source) / ".liquid-store" / "actions"),
      directory_(*repository_detail::build_access::root_path(source) / ".liquid-store" /
                 "actions") {}

runtime::task<std::optional<digest>> disk_cache::load_root_pointer() {
    const auto path = directory_ / "ROOT";
    if (!std::filesystem::exists(path)) {
        co_return std::nullopt;
    }
    auto bytes = co_await store_detail::read_file(path);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto text = std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    if (const auto newline = text.find('\n'); newline != std::string::npos) {
        text.resize(newline);
    }
    auto parsed = parse_digest(text);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    co_return *parsed;
}

runtime::task<void> disk_cache::save_root_pointer(digest root) {
    try {
        std::filesystem::create_directories(directory_);
    } catch (const std::system_error& caught) {
        co_return std::unexpected(cache_error(errc::io, caught.what(), directory_));
    }
    const auto text = to_string(root) + "\n";
    auto bytes =
        std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                               reinterpret_cast<const std::byte*>(text.data() + text.size())};
    const auto temporary = directory_ / "ROOT.tmp";
    const auto path = directory_ / "ROOT";
    auto written = co_await store_detail::write_file(temporary, std::move(bytes));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        co_return std::unexpected(cache_error(errc::io, code.message(), path));
    }
    co_return result<void>{};
}

runtime::task<void> disk_cache::open() {
    auto root = co_await load_root_pointer();
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto opened = co_await index_.open();
    const auto root_matches = opened && ((!*root && index_.entries().empty()) ||
                                         (*root && same_digest(index_.root(), **root)));
    if (root_matches) {
        co_return result<void>{};
    }
    if (!*root) {
        if (!opened) {
            co_return std::unexpected(opened.error());
        }
        co_return result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source_, **root);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto decoded = objects::decode_action_index_root(*bytes);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return co_await index_.rebuild(std::move(decoded->entries), **root);
}

result<digest> disk_cache::candidate_key(const action_input& input, digest sandbox) const {
    auto candidate = input;
    candidate.inputs = digest{input.inputs.alg, {}};
    candidate.outputs.clear();
    candidate.timeout.reset();
    auto encoded = objects::encode(objects::action_record{std::move(candidate), sandbox});
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(input.inputs.alg, *encoded);
}

runtime::task<cache_lookup> disk_cache::lookup(const action_input& input, digest sandbox) {
    auto candidate = candidate_key(input, sandbox);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    auto output = cache_lookup{};
    for (const auto& id : index_.candidates(*candidate)) {
        if (divergent(id)) {
            continue;
        }
        auto bytes = co_await repository_detail::build_access::get(source_, id);
        if (!bytes) {
            if (bytes.error().code == errc::not_found) {
                continue;
            }
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_result(*bytes);
        if (!value) {
            continue;
        }
        auto signature_valid = signed_result_is_valid(*value);
        if (!signature_valid) {
            co_return std::unexpected(signature_valid.error());
        }
        if (!*signature_valid) {
            continue;
        }
        if (!value->provenance) {
            continue;
        }
        auto provenance = co_await build::verify_provenance(source_, result_id{id});
        if (!provenance || !provenance->verified) {
            continue;
        }
        auto action_bytes = co_await repository_detail::build_access::get(source_, value->action);
        if (!action_bytes) {
            continue;
        }
        auto action = objects::decode_action(*action_bytes);
        if (!action) {
            continue;
        }
        auto recorded_candidate = candidate_key(action->input, action->sandbox);
        if (!recorded_candidate || !same_digest(*recorded_candidate, *candidate)) {
            continue;
        }
        auto references_present = true;
        auto references = std::vector<digest>{value->stdout_output, value->stderr_output};
        references.push_back(*value->provenance);
        for (const auto& action_output : value->outputs) {
            references.push_back(action_output.blob);
        }
        for (const auto& reference : references) {
            auto present = co_await repository_detail::build_access::contains(source_, reference);
            if (!present) {
                co_return std::unexpected(present.error());
            }
            if (!*present) {
                references_present = false;
                break;
            }
        }
        if (!references_present) {
            continue;
        }
        auto matches = true;
        for (const auto& expected : value->manifest) {
            auto actual = co_await manifest_digest(source_, input.inputs, expected.path);
            if (!actual) {
                co_return std::unexpected(actual.error());
            }
            if (!*actual || !same_digest(expected.content, **actual)) {
                matches = false;
                if (!output.why) {
                    output.why = cache_miss{expected.path, expected.content, *actual};
                }
                break;
            }
        }
        if (!matches) {
            continue;
        }
        auto served = co_await referenced_bytes(source_, *value);
        if (!served) {
            co_return std::unexpected(served.error());
        }
        output.hit = result_id{id};
        output.why.reset();
        output.bytes_served = *served;
        co_return output;
    }
    co_return output;
}

runtime::task<std::optional<result_id>> disk_cache::find_output(std::string_view path,
                                                                std::optional<digest> content) {
    const objects::action_index_entry* newest = nullptr;
    for (const auto& entry : index_.entries()) {
        auto bytes = co_await repository_detail::build_access::get(source_, entry.result);
        if (!bytes) {
            if (bytes.error().code == errc::not_found) {
                continue;
            }
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_result(*bytes);
        if (!value || !value->provenance) {
            continue;
        }
        const auto found = std::ranges::find(value->outputs, path, &action_output::path);
        if (found == value->outputs.end() || (content && !same_digest(found->blob, *content))) {
            continue;
        }
        if (!newest || entry.sequence > newest->sequence) {
            newest = &entry;
        }
    }
    if (!newest) {
        co_return std::nullopt;
    }
    co_return result_id{newest->result};
}

runtime::task<void>
disk_cache::publish(digest candidate, result_id id, const action_result& value) {
    if (!repository_detail::build_access::cache_publish_enabled(source_)) {
        co_return std::unexpected(
            error{errc::unauthorized, "cache publishing is disabled", "config.cache.publish"});
    }
    auto signature_valid = signed_result_is_valid(value);
    if (!signature_valid) {
        co_return std::unexpected(signature_valid.error());
    }
    if (!*signature_valid) {
        co_return std::unexpected(
            error{errc::unauthorized, "cache result signature is invalid", to_string(id.value)});
    }
    auto provenance = co_await build::verify_provenance(source_, id);
    if (!provenance) {
        co_return std::unexpected(provenance.error());
    }
    if (!provenance->verified) {
        co_return std::unexpected(
            error{errc::unauthorized, "cache result provenance is invalid", provenance->reason});
    }
    auto inserted = index_.insert(candidate, id.value);
    if (!inserted) {
        co_return std::unexpected(inserted.error());
    }
    auto persisted = co_await persist_root();
    if (!persisted) {
        co_return std::unexpected(persisted.error());
    }
    auto result_bytes = objects::encode(value);
    if (!result_bytes) {
        co_return std::unexpected(result_bytes.error());
    }
    auto stored = co_await referenced_bytes(source_, value);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    repository_detail::build_access::record_cache_store(source_, result_bytes->size() + *stored);
    co_return result<void>{};
}

runtime::task<void>
disk_cache::record_verification(digest result, digest attestation, bool is_divergent) {
    auto entries = index_.entries();
    auto found = false;
    for (auto& entry : entries) {
        if (!same_digest(entry.result, result)) {
            continue;
        }
        entry.verification = attestation;
        entry.divergent = entry.divergent || is_divergent;
        found = true;
    }
    if (!found) {
        co_return std::unexpected(
            error{errc::not_found, "cached result is not indexed", to_string(result)});
    }
    auto rebuilt = co_await index_.rebuild(std::move(entries), index_.root());
    if (!rebuilt) {
        co_return std::unexpected(rebuilt.error());
    }
    co_return co_await persist_root();
}

const std::vector<objects::action_index_entry>& disk_cache::entries() const noexcept {
    return index_.entries();
}

bool disk_cache::divergent(digest result) const noexcept {
    return std::ranges::any_of(index_.entries(), [&](const auto& entry) {
        return same_digest(entry.result, result) && entry.divergent;
    });
}

runtime::task<void> disk_cache::persist_root() {
    auto encoded_root = objects::encode(objects::action_index_root{index_.entries()});
    if (!encoded_root) {
        co_return std::unexpected(encoded_root.error());
    }
    auto root =
        co_await repository_detail::build_access::put_record(source_, std::move(*encoded_root));
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto retained = co_await repository_detail::build_access::retain_cache_root(source_, *root);
    if (!retained) {
        co_return std::unexpected(retained.error());
    }
    auto pointer = co_await save_root_pointer(*root);
    if (!pointer) {
        co_return std::unexpected(pointer.error());
    }
    co_return co_await index_.persist(*root);
}

void testing::reset_sandbox_spawn_count() noexcept {
    sandbox_spawns = 0;
}

std::uint64_t testing::sandbox_spawn_count() noexcept {
    return sandbox_spawns.load();
}

void testing::record_sandbox_spawn() noexcept {
    ++sandbox_spawns;
}

runtime::task<void> testing::release_cache_root(repository& source) {
    co_return co_await repository_detail::build_access::release_cache_root(source);
}

} // namespace liquid::build_detail
