#include "crdt/journal.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "encoding/cbor.hpp"

namespace liquid::crdt {
namespace {

namespace cbor = liquid::encoding::cbor;

auto io_error(std::string message, const std::filesystem::path &path) -> error {
    return error{errc::io, std::move(message), path.string()};
}

auto corrupt(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "drip journal"};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
    return cbor::value{cbor::value::map{values}};
}

auto field(const cbor::value &source, std::string_view name) -> result<const cbor::value *> {
    const auto *object = std::get_if<cbor::value::map>(&source.data);
    if (!object)
        return std::unexpected(corrupt("record is not a map"));
    for (const auto &[key, value] : *object) {
        const auto *text = std::get_if<std::string>(&key.data);
        if (text && *text == name)
            return &value;
    }
    return std::unexpected(corrupt("record field is missing: " + std::string{name}));
}

template <std::size_t Size>
auto decode_fixed(const cbor::value &value, std::string_view name)
    -> result<std::array<std::byte, Size>> {
    const auto *bytes = std::get_if<cbor::value::bytes>(&value.data);
    if (!bytes || bytes->size() != Size)
        return std::unexpected(corrupt("invalid byte string: " + std::string{name}));
    auto output = std::array<std::byte, Size>{};
    std::ranges::copy(*bytes, output.begin());
    return output;
}

template <std::size_t Size>
auto encode_fixed(const std::array<std::byte, Size> &value) -> cbor::value {
    return cbor::value{cbor::value::bytes{value.begin(), value.end()}};
}

auto encode_heads(const head_set &heads) -> cbor::value {
    auto output = cbor::value::array{};
    std::ranges::transform(heads, std::back_inserter(output),
                           [](const auto &value) { return encode_fixed(value); });
    return cbor::value{std::move(output)};
}

auto decode_heads(const cbor::value &value) -> result<head_set> {
    const auto *array = std::get_if<cbor::value::array>(&value.data);
    if (!array)
        return std::unexpected(corrupt("base is not an array"));
    auto output = head_set{};
    for (const auto &item : *array) {
        auto decoded = decode_fixed<32>(item, "head");
        if (!decoded)
            return std::unexpected(decoded.error());
        output.push_back(*decoded);
    }
    std::ranges::sort(output);
    return output;
}

auto encode_operation(const operation &op) -> cbor::value {
    return map({{"id", encode_fixed(op.id)},
                {"kind", cbor::value{static_cast<std::uint64_t>(op.kind)}},
                {"path", cbor::value{op.path}},
                {"to", cbor::value{op.to}},
                {"ref", cbor::value{op.ref}},
                {"mode", cbor::value{static_cast<std::uint64_t>(op.mode)}},
                {"entry", encode_fixed(op.entry_id)}});
}

auto decode_operation(const cbor::value &value) -> result<operation> {
    auto id_value = field(value, "id");
    auto kind_value = field(value, "kind");
    auto path_value = field(value, "path");
    auto to_value = field(value, "to");
    auto ref_value = field(value, "ref");
    auto mode_value = field(value, "mode");
    auto entry_value = field(value, "entry");
    if (!id_value || !kind_value || !path_value || !to_value || !ref_value || !mode_value ||
        !entry_value)
        return std::unexpected(corrupt("invalid operation record"));
    auto id = decode_fixed<16>(**id_value, "operation id");
    auto entry = decode_fixed<16>(**entry_value, "entry id");
    const auto *kind = std::get_if<std::uint64_t>(&(**kind_value).data);
    const auto *path = std::get_if<std::string>(&(**path_value).data);
    const auto *to = std::get_if<std::string>(&(**to_value).data);
    const auto *ref = std::get_if<std::string>(&(**ref_value).data);
    const auto *mode = std::get_if<std::uint64_t>(&(**mode_value).data);
    if (!id || !entry || !kind || *kind < 1 || *kind > 3 || !path || !to || !ref || !mode ||
        *mode > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(corrupt("invalid operation fields"));
    return operation{.id = *id,
                     .kind = static_cast<operation_kind>(*kind),
                     .path = *path,
                     .to = *to,
                     .ref = *ref,
                     .mode = static_cast<std::uint32_t>(*mode),
                     .entry_id = *entry};
}

auto encode_record(const drip_record &record) -> result<std::vector<std::byte>> {
    auto operations = cbor::value::array{};
    std::ranges::transform(record.ops, std::back_inserter(operations), encode_operation);
    auto fields = cbor::value::map{
        {cbor::value{"id"}, encode_fixed(record.id)},
        {cbor::value{"base"}, encode_heads(record.base)},
        {cbor::value{"ops"}, cbor::value{std::move(operations)}},
        {cbor::value{"by"}, encode_fixed(record.by)},
        {cbor::value{"at"}, cbor::value{record.at}},
    };
    if (record.signature)
        fields.emplace_back(cbor::value{"sig"}, cbor::value{*record.signature});
    return cbor::encode(cbor::value{std::move(fields)});
}

auto decode_record(std::span<const std::byte> bytes) -> result<drip_record> {
    auto decoded = cbor::decode(bytes);
    if (!decoded)
        return std::unexpected(decoded.error());
    auto id_value = field(*decoded, "id");
    auto base_value = field(*decoded, "base");
    auto ops_value = field(*decoded, "ops");
    auto by_value = field(*decoded, "by");
    auto at_value = field(*decoded, "at");
    if (!id_value || !base_value || !ops_value || !by_value || !at_value)
        return std::unexpected(corrupt("invalid drip record"));
    auto id = decode_fixed<16>(**id_value, "drip id");
    auto base = decode_heads(**base_value);
    auto by = decode_fixed<16>(**by_value, "actor id");
    const auto *operations = std::get_if<cbor::value::array>(&(**ops_value).data);
    auto at = std::int64_t{};
    if (const auto *signed_at = std::get_if<std::int64_t>(&(**at_value).data))
        at = *signed_at;
    else if (const auto *unsigned_at = std::get_if<std::uint64_t>(&(**at_value).data);
             unsigned_at &&
             *unsigned_at <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        at = static_cast<std::int64_t>(*unsigned_at);
    else
        return std::unexpected(corrupt("invalid drip timestamp"));
    if (!id || !base || !by || !operations)
        return std::unexpected(corrupt("invalid drip fields"));
    auto ops = std::vector<operation>{};
    for (const auto &item : *operations) {
        auto op = decode_operation(item);
        if (!op)
            return std::unexpected(op.error());
        ops.push_back(std::move(*op));
    }
    auto signature = std::optional<std::vector<std::byte>>{};
    auto sig = field(*decoded, "sig");
    if (sig) {
        const auto *value = std::get_if<cbor::value::bytes>(&(**sig).data);
        if (!value)
            return std::unexpected(corrupt("invalid drip signature"));
        signature = *value;
    }
    return drip_record{.id = *id,
                       .base = std::move(*base),
                       .ops = std::move(ops),
                       .by = *by,
                       .at = at,
                       .signature = std::move(signature)};
}

void write_length(std::ostream &output, std::uint32_t value) {
    for (auto shift : {24U, 16U, 8U, 0U})
        output.put(static_cast<char>(value >> shift));
}

auto read_length(std::istream &input) -> std::optional<std::uint32_t> {
    auto value = std::uint32_t{0};
    for (int i = 0; i < 4; ++i) {
        const auto byte = input.get();
        if (byte == std::char_traits<char>::eof())
            return std::nullopt;
        value = (value << 8U) | static_cast<unsigned char>(byte);
    }
    return value;
}

} // namespace

journal::journal(std::filesystem::path path) : path_(std::move(path)) {}

auto journal::cut_path() const -> std::filesystem::path {
    auto output = path_;
    output += ".cut";
    return output;
}

auto journal::append(const drip_record &record) -> result<void> {
    auto encoded = encode_record(record);
    if (!encoded)
        return std::unexpected(encoded.error());
    if (encoded->size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(io_error("drip record is too large", path_));
    auto output = std::ofstream{path_, std::ios::binary | std::ios::app};
    if (!output)
        return std::unexpected(io_error("cannot open journal", path_));
    write_length(output, static_cast<std::uint32_t>(encoded->size()));
    output.write(reinterpret_cast<const char *>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    output.flush();
    if (!output)
        return std::unexpected(io_error("cannot append journal", path_));
    return {};
}

auto journal::cut(const document &value, std::uint64_t epoch) -> result<void> {
    auto saved = value.save();
    if (!saved)
        return std::unexpected(saved.error());
    auto encoded = cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                                     {"epoch", cbor::value{epoch}},
                                     {"document", cbor::value{std::move(*saved)}}}));
    if (!encoded)
        return std::unexpected(encoded.error());
    auto temporary = cut_path();
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        if (!output)
            return std::unexpected(io_error("cannot open cut file", temporary));
        output.write(reinterpret_cast<const char *>(encoded->data()),
                     static_cast<std::streamsize>(encoded->size()));
        output.flush();
        if (!output)
            return std::unexpected(io_error("cannot write cut file", temporary));
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, cut_path(), code);
    if (code) {
        std::filesystem::remove(cut_path(), code);
        code.clear();
        std::filesystem::rename(temporary, cut_path(), code);
    }
    if (code)
        return std::unexpected(io_error(code.message(), cut_path()));
    auto journal_file = std::ofstream{path_, std::ios::binary | std::ios::trunc};
    if (!journal_file)
        return std::unexpected(io_error("cannot reset journal after cut", path_));
    return {};
}

auto journal::replay(std::string_view taap_id, std::string_view device_id) const
    -> result<document> {
    auto cut_input = std::ifstream{cut_path(), std::ios::binary};
    if (!cut_input)
        return std::unexpected(io_error("cannot open cut file", cut_path()));
    auto cut_bytes = std::vector<std::byte>{};
    for (char byte; cut_input.get(byte);)
        cut_bytes.push_back(std::byte{static_cast<unsigned char>(byte)});
    auto cut_record = cbor::decode(cut_bytes);
    if (!cut_record)
        return std::unexpected(cut_record.error());
    auto version = field(*cut_record, "v");
    auto document_value = field(*cut_record, "document");
    const auto *v = version ? std::get_if<std::uint64_t>(&(**version).data) : nullptr;
    const auto *saved =
        document_value ? std::get_if<cbor::value::bytes>(&(**document_value).data) : nullptr;
    if (!v || *v != 1 || !saved)
        return std::unexpected(corrupt("invalid cut file"));
    auto replayed = document::load(*saved, taap_id, device_id);
    if (!replayed)
        return std::unexpected(replayed.error());

    auto input = std::ifstream{path_, std::ios::binary};
    if (!input)
        return std::unexpected(io_error("cannot open journal", path_));
    while (input.peek() != std::char_traits<char>::eof()) {
        auto length = read_length(input);
        if (!length)
            return std::unexpected(corrupt("truncated journal length"));
        auto bytes = std::vector<std::byte>(*length);
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            return std::unexpected(corrupt("truncated journal record"));
        auto record = decode_record(bytes);
        if (!record)
            return std::unexpected(record.error());
        auto current = replayed->heads();
        if (!current)
            return std::unexpected(current.error());
        if (*current != record->base)
            return std::unexpected(corrupt("drip base does not match replay heads"));
        if (replayed->actor() != record->by)
            return std::unexpected(corrupt("drip actor does not match replay actor"));
        auto applied = replayed->transact(std::move(record->ops));
        if (!applied)
            return std::unexpected(applied.error());
    }
    return replayed;
}

} // namespace liquid::crdt
