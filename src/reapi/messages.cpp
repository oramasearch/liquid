// SPDX-License-Identifier: MIT

#include "reapi/messages.hpp"

#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include "protobuf/wire.hpp"

namespace liquid::reapi {
namespace {

namespace pb = protobuf;

template <typename Owner, typename Member> struct field_descriptor {
    std::uint32_t number;
    Member Owner::* member;
};

template <typename Owner, typename Member>
auto field(std::uint32_t number, Member Owner::* member) -> field_descriptor<Owner, Member> {
    return {number, member};
}

template <typename Type> struct is_optional : std::false_type {};

template <typename Type> struct is_optional<std::optional<Type>> : std::true_type {
    using value_type = Type;
};

template <typename Type> struct is_vector : std::false_type {};

template <typename Type> struct is_vector<std::vector<Type>> : std::true_type {
    using value_type = Type;
};

template <typename Type> struct is_map : std::false_type {};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct is_map<std::map<Key, Value, Compare, Allocator>> : std::true_type {
    using key_type = Key;
    using mapped_type = Value;
};

template <typename Type>
constexpr bool is_bytes = std::is_same_v<std::remove_cvref_t<Type>, std::vector<std::byte>>;

template <typename Type>
concept message_type = requires(Type value) { value.unknown_fields; };

auto failure(std::string message) -> error {
    return {errc::corrupt, std::move(message), "REAPI protobuf"};
}

auto invalid(std::string message) -> error {
    return {errc::invalid_argument, std::move(message), "REAPI protobuf"};
}

auto valid_utf8(std::string_view text) -> bool {
    for (auto position = std::size_t{}; position < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[position]);
        auto continuation = std::size_t{};
        auto codepoint = std::uint32_t{};
        if (first <= 0x7fU) {
            ++position;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            continuation = 1;
            codepoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation = 2;
            codepoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (position + continuation >= text.size()) {
            return false;
        }
        for (auto index = std::size_t{1}; index <= continuation; ++index) {
            const auto next = static_cast<std::uint8_t>(text[position + index]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        constexpr auto minimum = std::uint32_t{0x80};
        const auto lower_bound = continuation == 1   ? minimum
                                 : continuation == 2 ? std::uint32_t{0x800}
                                                     : std::uint32_t{0x10000};
        if (codepoint < lower_bound || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        position += continuation + 1;
    }
    return true;
}

template <typename Type>
auto encode_value(pb::writer& output, std::uint32_t number, const Type& value) -> result<void>;

template <typename Type> auto decode_value(const pb::field& input, Type& output) -> result<void>;

template <typename Message>
auto encode_nested(pb::writer& output, std::uint32_t number, const Message& value) -> result<void> {
    auto encoded = encode(value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    output.message(number, *encoded);
    return {};
}

template <typename Message>
auto decode_nested(const pb::field& input, Message& output) -> result<void> {
    auto bytes = pb::require_bytes(input, "nested message");
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return decode(*bytes, output);
}

template <typename Value>
auto encode_map_entry(const std::string& key, const Value& value)
    -> result<std::vector<std::byte>> {
    if (!valid_utf8(key)) {
        return std::unexpected(invalid("map key is not valid UTF-8"));
    }
    auto output = pb::writer{};
    if (!key.empty()) {
        output.string(1, key);
    }
    auto encoded = encode_nested(output, 2, value);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return std::move(output).take();
}

template <typename Value>
auto decode_map_entry(std::span<const std::byte> input) -> result<std::pair<std::string, Value>> {
    auto key = std::string{};
    auto value = Value{};
    auto source = pb::reader{input};
    while (true) {
        auto next = source.next();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            break;
        }
        if ((*next)->number == 1) {
            auto text = pb::require_string(**next, "map key");
            if (!text || !valid_utf8(*text)) {
                return std::unexpected(failure("invalid map key"));
            }
            key = std::move(*text);
        } else if ((*next)->number == 2) {
            auto decoded = decode_nested(**next, value);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
        }
    }
    return std::pair{std::move(key), std::move(value)};
}

template <typename Type>
auto encode_value(pb::writer& output, std::uint32_t number, const Type& value) -> result<void> {
    using value_type = std::remove_cvref_t<Type>;
    if constexpr (is_optional<value_type>::value) {
        if (value) {
            return encode_nested(output, number, *value);
        }
    } else if constexpr (is_map<value_type>::value) {
        for (const auto& [key, item] : value) {
            auto entry = encode_map_entry(key, item);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.message(number, *entry);
        }
    } else if constexpr (is_bytes<value_type>) {
        if (!value.empty()) {
            output.bytes(number, value);
        }
    } else if constexpr (is_vector<value_type>::value) {
        using item_type = typename is_vector<value_type>::value_type;
        if constexpr (std::is_enum_v<item_type>) {
            auto values = std::vector<std::uint64_t>{};
            values.reserve(value.size());
            for (const auto item : value) {
                values.push_back(static_cast<std::uint64_t>(item));
            }
            if (!values.empty()) {
                output.packed_varints(number, values);
            }
        } else {
            for (const auto& item : value) {
                if constexpr (std::is_same_v<item_type, std::string>) {
                    if (!valid_utf8(item)) {
                        return std::unexpected(invalid("string is not valid UTF-8"));
                    }
                    output.string(number, item);
                } else {
                    auto encoded = encode_nested(output, number, item);
                    if (!encoded) {
                        return std::unexpected(encoded.error());
                    }
                }
            }
        }
    } else if constexpr (std::is_same_v<value_type, std::string>) {
        if (!valid_utf8(value)) {
            return std::unexpected(invalid("string is not valid UTF-8"));
        }
        if (!value.empty()) {
            output.string(number, value);
        }
    } else if constexpr (std::is_same_v<value_type, bool>) {
        if (value) {
            output.boolean(number, value);
        }
    } else if constexpr (std::is_enum_v<value_type>) {
        if (value != static_cast<value_type>(0)) {
            output.int32(number, static_cast<std::int32_t>(value));
        }
    } else if constexpr (std::is_same_v<value_type, std::int32_t>) {
        if (value != 0) {
            output.int32(number, value);
        }
    } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
        if (value != 0) {
            output.int64(number, value);
        }
    } else if constexpr (std::is_same_v<value_type, std::uint32_t>) {
        if (value != 0) {
            output.varint(number, value);
        }
    } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
        if (value != 0) {
            output.varint(number, value);
        }
    } else if constexpr (message_type<value_type>) {
        return encode_nested(output, number, value);
    } else {
        static_assert(sizeof(Type) == 0, "unsupported protobuf field type");
    }
    return {};
}

template <typename Type> auto decode_value(const pb::field& input, Type& output) -> result<void> {
    using value_type = std::remove_cvref_t<Type>;
    if constexpr (is_optional<value_type>::value) {
        using item_type = typename is_optional<value_type>::value_type;
        if (!output) {
            output = item_type{};
        }
        return decode_nested(input, *output);
    } else if constexpr (is_map<value_type>::value) {
        using item_type = typename is_map<value_type>::mapped_type;
        auto bytes = pb::require_bytes(input, "map entry");
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto entry = decode_map_entry<item_type>(*bytes);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        output.insert_or_assign(std::move(entry->first), std::move(entry->second));
        return {};
    } else if constexpr (is_bytes<value_type>) {
        auto bytes = pb::require_bytes(input, "bytes field");
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        output.assign(bytes->begin(), bytes->end());
        return {};
    } else if constexpr (is_vector<value_type>::value) {
        using item_type = typename is_vector<value_type>::value_type;
        if constexpr (std::is_enum_v<item_type>) {
            auto values = pb::unpack_varints(input, "packed enum field");
            if (!values) {
                return std::unexpected(values.error());
            }
            for (const auto value : *values) {
                output.push_back(static_cast<item_type>(static_cast<std::int32_t>(value)));
            }
        } else if constexpr (std::is_same_v<item_type, std::string>) {
            auto text = pb::require_string(input, "repeated string field");
            if (!text || !valid_utf8(*text)) {
                return std::unexpected(failure("invalid repeated string"));
            }
            output.push_back(std::move(*text));
        } else {
            auto item = item_type{};
            auto decoded = decode_nested(input, item);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            output.push_back(std::move(item));
        }
        return {};
    } else if constexpr (std::is_same_v<value_type, std::string>) {
        auto text = pb::require_string(input, "string field");
        if (!text || !valid_utf8(*text)) {
            return std::unexpected(failure("invalid string field"));
        }
        output = std::move(*text);
        return {};
    } else if constexpr (std::is_same_v<value_type, bool>) {
        auto value = pb::require_varint(input, "bool field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = *value != 0;
        return {};
    } else if constexpr (std::is_enum_v<value_type>) {
        auto value = pb::require_varint(input, "enum field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = static_cast<value_type>(static_cast<std::int32_t>(*value));
        return {};
    } else if constexpr (std::is_same_v<value_type, std::int32_t>) {
        auto value = pb::require_varint(input, "int32 field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = static_cast<std::int32_t>(*value);
        return {};
    } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
        auto value = pb::require_varint(input, "int64 field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = static_cast<std::int64_t>(*value);
        return {};
    } else if constexpr (std::is_same_v<value_type, std::uint32_t>) {
        auto value = pb::require_varint(input, "uint32 field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = static_cast<std::uint32_t>(*value);
        return {};
    } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
        auto value = pb::require_varint(input, "uint64 field");
        if (!value) {
            return std::unexpected(value.error());
        }
        output = *value;
        return {};
    } else if constexpr (message_type<value_type>) {
        return decode_nested(input, output);
    } else {
        static_assert(sizeof(Type) == 0, "unsupported protobuf field type");
    }
}

template <std::size_t Index = 0, typename Message, typename Fields>
auto encode_fields(const Message& value, const Fields& fields, pb::writer& output) -> result<void> {
    if constexpr (Index == std::tuple_size_v<Fields>) {
        return {};
    } else {
        const auto& descriptor = std::get<Index>(fields);
        auto encoded = encode_value(output, descriptor.number, value.*(descriptor.member));
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        return encode_fields<Index + 1>(value, fields, output);
    }
}

template <std::size_t Index = 0, typename Message, typename Fields>
auto decode_field(Message& value, const Fields& fields, const pb::field& input) -> result<bool> {
    if constexpr (Index == std::tuple_size_v<Fields>) {
        return false;
    } else {
        const auto& descriptor = std::get<Index>(fields);
        if (descriptor.number == input.number) {
            auto decoded = decode_value(input, value.*(descriptor.member));
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            return true;
        }
        return decode_field<Index + 1>(value, fields, input);
    }
}

template <typename Message, typename Fields>
auto encode_generic(const Message& value, const Fields& fields) -> result<std::vector<std::byte>> {
    auto output = pb::writer{};
    auto encoded = encode_fields(value, fields, output);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    output.unknown(value.unknown_fields);
    return std::move(output).take();
}

template <typename Message, typename Fields>
auto decode_generic(std::span<const std::byte> input, Message& output, const Fields& fields)
    -> result<void> {
    auto source = pb::reader{input};
    while (true) {
        auto next = source.next();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            return {};
        }
        auto decoded = decode_field(output, fields, **next);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!*decoded) {
            pb::preserve_unknown(output.unknown_fields, **next);
        }
    }
}

auto fields(Any*) {
    return std::tuple{field(1, &Any::type_url), field(2, &Any::value)};
}

auto fields(Timestamp*) {
    return std::tuple{field(1, &Timestamp::seconds), field(2, &Timestamp::nanos)};
}

auto fields(Duration*) {
    return std::tuple{field(1, &Duration::seconds), field(2, &Duration::nanos)};
}

auto fields(UInt32Value*) {
    return std::tuple{field(1, &UInt32Value::value)};
}

auto fields(Status*) {
    return std::tuple{
        field(1, &Status::code), field(2, &Status::message), field(3, &Status::details)};
}

auto fields(SemVer*) {
    return std::tuple{field(1, &SemVer::major),
                      field(2, &SemVer::minor),
                      field(3, &SemVer::patch),
                      field(4, &SemVer::prerelease)};
}

auto fields(DigestFunction*) {
    return std::tuple{};
}

auto fields(Digest*) {
    return std::tuple{field(1, &Digest::hash), field(2, &Digest::size_bytes)};
}

auto fields(NodeProperty*) {
    return std::tuple{field(1, &NodeProperty::name), field(2, &NodeProperty::value)};
}

auto fields(NodeProperties*) {
    return std::tuple{field(1, &NodeProperties::properties),
                      field(2, &NodeProperties::mtime),
                      field(3, &NodeProperties::unix_mode)};
}

auto fields(FileNode*) {
    return std::tuple{field(1, &FileNode::name),
                      field(2, &FileNode::digest),
                      field(4, &FileNode::is_executable),
                      field(6, &FileNode::node_properties)};
}

auto fields(DirectoryNode*) {
    return std::tuple{field(1, &DirectoryNode::name), field(2, &DirectoryNode::digest)};
}

auto fields(SymlinkNode*) {
    return std::tuple{field(1, &SymlinkNode::name),
                      field(2, &SymlinkNode::target),
                      field(4, &SymlinkNode::node_properties)};
}

auto fields(Directory*) {
    return std::tuple{field(1, &Directory::files),
                      field(2, &Directory::directories),
                      field(3, &Directory::symlinks),
                      field(5, &Directory::node_properties)};
}

auto fields(EnvironmentVariable*) {
    return std::tuple{field(1, &EnvironmentVariable::name), field(2, &EnvironmentVariable::value)};
}

auto fields(PlatformProperty*) {
    return std::tuple{field(1, &PlatformProperty::name), field(2, &PlatformProperty::value)};
}

auto fields(Platform*) {
    return std::tuple{field(1, &Platform::properties)};
}

auto fields(Command*) {
    return std::tuple{field(1, &Command::arguments),
                      field(2, &Command::environment_variables),
                      field(3, &Command::output_files),
                      field(4, &Command::output_directories),
                      field(5, &Command::platform),
                      field(6, &Command::working_directory),
                      field(7, &Command::output_paths),
                      field(8, &Command::output_node_properties),
                      field(9, &Command::output_directory_format)};
}

auto fields(Action*) {
    return std::tuple{field(1, &Action::command_digest),
                      field(2, &Action::input_root_digest),
                      field(6, &Action::timeout),
                      field(7, &Action::do_not_cache),
                      field(9, &Action::salt),
                      field(10, &Action::platform)};
}

auto fields(ExecutedActionMetadata*) {
    return std::tuple{field(1, &ExecutedActionMetadata::worker),
                      field(2, &ExecutedActionMetadata::queued_timestamp),
                      field(3, &ExecutedActionMetadata::worker_start_timestamp),
                      field(4, &ExecutedActionMetadata::worker_completed_timestamp),
                      field(5, &ExecutedActionMetadata::input_fetch_start_timestamp),
                      field(6, &ExecutedActionMetadata::input_fetch_completed_timestamp),
                      field(7, &ExecutedActionMetadata::execution_start_timestamp),
                      field(8, &ExecutedActionMetadata::execution_completed_timestamp),
                      field(9, &ExecutedActionMetadata::output_upload_start_timestamp),
                      field(10, &ExecutedActionMetadata::output_upload_completed_timestamp),
                      field(11, &ExecutedActionMetadata::auxiliary_metadata),
                      field(12, &ExecutedActionMetadata::virtual_execution_duration)};
}

auto fields(OutputFile*) {
    return std::tuple{field(1, &OutputFile::path),
                      field(2, &OutputFile::digest),
                      field(4, &OutputFile::is_executable),
                      field(5, &OutputFile::contents),
                      field(7, &OutputFile::node_properties)};
}

auto fields(OutputDirectory*) {
    return std::tuple{field(1, &OutputDirectory::path),
                      field(3, &OutputDirectory::tree_digest),
                      field(4, &OutputDirectory::is_topologically_sorted),
                      field(5, &OutputDirectory::root_directory_digest)};
}

auto fields(OutputSymlink*) {
    return std::tuple{field(1, &OutputSymlink::path),
                      field(2, &OutputSymlink::target),
                      field(4, &OutputSymlink::node_properties)};
}

auto fields(Tree*) {
    return std::tuple{field(1, &Tree::root), field(2, &Tree::children)};
}

auto fields(LogFile*) {
    return std::tuple{field(1, &LogFile::digest), field(2, &LogFile::human_readable)};
}

auto fields(ActionResult*) {
    return std::tuple{field(2, &ActionResult::output_files),
                      field(3, &ActionResult::output_directories),
                      field(4, &ActionResult::exit_code),
                      field(5, &ActionResult::stdout_raw),
                      field(6, &ActionResult::stdout_digest),
                      field(7, &ActionResult::stderr_raw),
                      field(8, &ActionResult::stderr_digest),
                      field(9, &ActionResult::execution_metadata),
                      field(10, &ActionResult::output_file_symlinks),
                      field(11, &ActionResult::output_directory_symlinks),
                      field(12, &ActionResult::output_symlinks)};
}

auto fields(ExecutionPolicy*) {
    return std::tuple{field(1, &ExecutionPolicy::priority)};
}

auto fields(ResultsCachePolicy*) {
    return std::tuple{field(1, &ResultsCachePolicy::priority)};
}

auto fields(ExecuteRequest*) {
    return std::tuple{field(1, &ExecuteRequest::instance_name),
                      field(3, &ExecuteRequest::skip_cache_lookup),
                      field(6, &ExecuteRequest::action_digest),
                      field(7, &ExecuteRequest::execution_policy),
                      field(8, &ExecuteRequest::results_cache_policy),
                      field(9, &ExecuteRequest::digest_function),
                      field(10, &ExecuteRequest::inline_stdout),
                      field(11, &ExecuteRequest::inline_stderr),
                      field(12, &ExecuteRequest::inline_output_files)};
}

auto fields(ExecuteResponse*) {
    return std::tuple{field(1, &ExecuteResponse::result),
                      field(2, &ExecuteResponse::cached_result),
                      field(3, &ExecuteResponse::status),
                      field(4, &ExecuteResponse::server_logs),
                      field(5, &ExecuteResponse::message)};
}

auto fields(ExecuteOperationMetadata*) {
    return std::tuple{field(1, &ExecuteOperationMetadata::stage),
                      field(2, &ExecuteOperationMetadata::action_digest),
                      field(3, &ExecuteOperationMetadata::stdout_stream_name),
                      field(4, &ExecuteOperationMetadata::stderr_stream_name),
                      field(5, &ExecuteOperationMetadata::partial_execution_metadata),
                      field(6, &ExecuteOperationMetadata::digest_function)};
}

auto fields(WaitExecutionRequest*) {
    return std::tuple{field(1, &WaitExecutionRequest::name)};
}

auto fields(Empty*) {
    return std::tuple{};
}

auto fields(CancelOperationRequest*) {
    return std::tuple{field(1, &CancelOperationRequest::name)};
}

auto fields(GetActionResultRequest*) {
    return std::tuple{field(1, &GetActionResultRequest::instance_name),
                      field(2, &GetActionResultRequest::action_digest),
                      field(3, &GetActionResultRequest::inline_stdout),
                      field(4, &GetActionResultRequest::inline_stderr),
                      field(5, &GetActionResultRequest::inline_output_files),
                      field(6, &GetActionResultRequest::digest_function)};
}

auto fields(UpdateActionResultRequest*) {
    return std::tuple{field(1, &UpdateActionResultRequest::instance_name),
                      field(2, &UpdateActionResultRequest::action_digest),
                      field(3, &UpdateActionResultRequest::action_result),
                      field(4, &UpdateActionResultRequest::results_cache_policy),
                      field(5, &UpdateActionResultRequest::digest_function)};
}

auto fields(FindMissingBlobsRequest*) {
    return std::tuple{field(1, &FindMissingBlobsRequest::instance_name),
                      field(2, &FindMissingBlobsRequest::blob_digests),
                      field(3, &FindMissingBlobsRequest::digest_function)};
}

auto fields(FindMissingBlobsResponse*) {
    return std::tuple{field(2, &FindMissingBlobsResponse::missing_blob_digests)};
}

auto fields(BatchUpdateBlobsRequestEntry*) {
    return std::tuple{field(1, &BatchUpdateBlobsRequestEntry::digest),
                      field(2, &BatchUpdateBlobsRequestEntry::data),
                      field(3, &BatchUpdateBlobsRequestEntry::compressor)};
}

auto fields(BatchUpdateBlobsRequest*) {
    return std::tuple{field(1, &BatchUpdateBlobsRequest::instance_name),
                      field(2, &BatchUpdateBlobsRequest::requests),
                      field(5, &BatchUpdateBlobsRequest::digest_function)};
}

auto fields(BatchUpdateBlobsResponseEntry*) {
    return std::tuple{field(1, &BatchUpdateBlobsResponseEntry::digest),
                      field(2, &BatchUpdateBlobsResponseEntry::status)};
}

auto fields(BatchUpdateBlobsResponse*) {
    return std::tuple{field(1, &BatchUpdateBlobsResponse::responses)};
}

auto fields(BatchReadBlobsRequest*) {
    return std::tuple{field(1, &BatchReadBlobsRequest::instance_name),
                      field(2, &BatchReadBlobsRequest::digests),
                      field(3, &BatchReadBlobsRequest::acceptable_compressors),
                      field(4, &BatchReadBlobsRequest::digest_function)};
}

auto fields(BatchReadBlobsResponseEntry*) {
    return std::tuple{field(1, &BatchReadBlobsResponseEntry::digest),
                      field(2, &BatchReadBlobsResponseEntry::data),
                      field(3, &BatchReadBlobsResponseEntry::status),
                      field(4, &BatchReadBlobsResponseEntry::compressor)};
}

auto fields(BatchReadBlobsResponse*) {
    return std::tuple{field(1, &BatchReadBlobsResponse::responses)};
}

auto fields(GetTreeRequest*) {
    return std::tuple{field(1, &GetTreeRequest::instance_name),
                      field(2, &GetTreeRequest::root_digest),
                      field(3, &GetTreeRequest::page_size),
                      field(4, &GetTreeRequest::page_token),
                      field(5, &GetTreeRequest::digest_function)};
}

auto fields(GetTreeResponse*) {
    return std::tuple{field(1, &GetTreeResponse::directories),
                      field(2, &GetTreeResponse::next_page_token)};
}

auto fields(GetCapabilitiesRequest*) {
    return std::tuple{field(1, &GetCapabilitiesRequest::instance_name)};
}

auto fields(ActionCacheUpdateCapabilities*) {
    return std::tuple{field(1, &ActionCacheUpdateCapabilities::update_enabled)};
}

auto fields(PriorityRange*) {
    return std::tuple{field(1, &PriorityRange::min_priority),
                      field(2, &PriorityRange::max_priority)};
}

auto fields(PriorityCapabilities*) {
    return std::tuple{field(1, &PriorityCapabilities::priorities)};
}

auto fields(FastCdc2020Params*) {
    return std::tuple{field(1, &FastCdc2020Params::avg_chunk_size_bytes),
                      field(2, &FastCdc2020Params::seed)};
}

auto fields(RepMaxCdcParams*) {
    return std::tuple{field(1, &RepMaxCdcParams::min_chunk_size_bytes),
                      field(2, &RepMaxCdcParams::horizon_size_bytes)};
}

auto fields(CacheCapabilities*) {
    return std::tuple{field(1, &CacheCapabilities::digest_functions),
                      field(2, &CacheCapabilities::action_cache_update_capabilities),
                      field(3, &CacheCapabilities::cache_priority_capabilities),
                      field(4, &CacheCapabilities::max_batch_total_size_bytes),
                      field(5, &CacheCapabilities::symlink_absolute_path_strategy),
                      field(6, &CacheCapabilities::supported_compressors),
                      field(7, &CacheCapabilities::supported_batch_update_compressors),
                      field(8, &CacheCapabilities::max_cas_blob_size_bytes),
                      field(9, &CacheCapabilities::split_blob_support),
                      field(10, &CacheCapabilities::splice_blob_support),
                      field(11, &CacheCapabilities::fast_cdc_2020_params),
                      field(12, &CacheCapabilities::rep_max_cdc_params)};
}

auto fields(ExecutionCapabilities*) {
    return std::tuple{field(1, &ExecutionCapabilities::digest_function),
                      field(2, &ExecutionCapabilities::exec_enabled),
                      field(3, &ExecutionCapabilities::execution_priority_capabilities),
                      field(4, &ExecutionCapabilities::supported_node_properties),
                      field(5, &ExecutionCapabilities::digest_functions)};
}

auto fields(ServerCapabilities*) {
    return std::tuple{field(1, &ServerCapabilities::cache_capabilities),
                      field(2, &ServerCapabilities::execution_capabilities),
                      field(3, &ServerCapabilities::deprecated_api_version),
                      field(4, &ServerCapabilities::low_api_version),
                      field(5, &ServerCapabilities::high_api_version)};
}

auto fields(ReadRequest*) {
    return std::tuple{field(1, &ReadRequest::resource_name),
                      field(2, &ReadRequest::read_offset),
                      field(3, &ReadRequest::read_limit)};
}

auto fields(ReadResponse*) {
    return std::tuple{field(10, &ReadResponse::data)};
}

auto fields(WriteRequest*) {
    return std::tuple{field(1, &WriteRequest::resource_name),
                      field(2, &WriteRequest::write_offset),
                      field(3, &WriteRequest::finish_write),
                      field(10, &WriteRequest::data)};
}

auto fields(WriteResponse*) {
    return std::tuple{field(1, &WriteResponse::committed_size)};
}

auto fields(QueryWriteStatusRequest*) {
    return std::tuple{field(1, &QueryWriteStatusRequest::resource_name)};
}

auto fields(QueryWriteStatusResponse*) {
    return std::tuple{field(1, &QueryWriteStatusResponse::committed_size),
                      field(2, &QueryWriteStatusResponse::complete)};
}

template <typename Message>
auto encode_known(const Message& value) -> result<std::vector<std::byte>> {
    return encode_generic(value, fields(static_cast<Message*>(nullptr)));
}

template <typename Message>
auto decode_known(std::span<const std::byte> input, Message& output) -> result<void> {
    return decode_generic(input, output, fields(static_cast<Message*>(nullptr)));
}

} // namespace

auto encode(const Operation& value) -> result<std::vector<std::byte>> {
    auto output = pb::writer{};
    auto name = encode_value(output, 1, value.name);
    auto metadata = encode_value(output, 2, value.metadata);
    auto done = encode_value(output, 3, value.done);
    if (!name || !metadata || !done) {
        return std::unexpected(invalid("invalid Operation field"));
    }
    if (const auto* status = std::get_if<Status>(&value.result)) {
        auto encoded = encode_nested(output, 4, *status);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
    } else if (const auto* response = std::get_if<Any>(&value.result)) {
        auto encoded = encode_nested(output, 5, *response);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
    }
    output.unknown(value.unknown_fields);
    return std::move(output).take();
}

auto decode(std::span<const std::byte> input, Operation& output) -> result<void> {
    auto source = pb::reader{input};
    while (true) {
        auto next = source.next();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            return {};
        }
        switch ((*next)->number) {
        case 1: {
            auto decoded = decode_value(**next, output.name);
            if (!decoded) {
                return decoded;
            }
            break;
        }
        case 2: {
            auto decoded = decode_value(**next, output.metadata);
            if (!decoded) {
                return decoded;
            }
            break;
        }
        case 3: {
            auto decoded = decode_value(**next, output.done);
            if (!decoded) {
                return decoded;
            }
            break;
        }
        case 4: {
            auto status = Status{};
            auto decoded = decode_nested(**next, status);
            if (!decoded) {
                return decoded;
            }
            output.result = std::move(status);
            break;
        }
        case 5: {
            auto response = Any{};
            auto decoded = decode_nested(**next, response);
            if (!decoded) {
                return decoded;
            }
            output.result = std::move(response);
            break;
        }
        default:
            pb::preserve_unknown(output.unknown_fields, **next);
            break;
        }
    }
}

#define LIQUID_DEFINE_REAPI_CODEC(Type)                                                            \
    auto encode(const Type& value) -> result<std::vector<std::byte>> {                             \
        return encode_known(value);                                                                \
    }                                                                                              \
    auto decode(std::span<const std::byte> input, Type& output) -> result<void> {                  \
        return decode_known(input, output);                                                        \
    }

LIQUID_DEFINE_REAPI_CODEC(Any)
LIQUID_DEFINE_REAPI_CODEC(Timestamp)
LIQUID_DEFINE_REAPI_CODEC(Duration)
LIQUID_DEFINE_REAPI_CODEC(UInt32Value)
LIQUID_DEFINE_REAPI_CODEC(Status)
LIQUID_DEFINE_REAPI_CODEC(SemVer)
LIQUID_DEFINE_REAPI_CODEC(DigestFunction)
LIQUID_DEFINE_REAPI_CODEC(Digest)
LIQUID_DEFINE_REAPI_CODEC(NodeProperty)
LIQUID_DEFINE_REAPI_CODEC(NodeProperties)
LIQUID_DEFINE_REAPI_CODEC(FileNode)
LIQUID_DEFINE_REAPI_CODEC(DirectoryNode)
LIQUID_DEFINE_REAPI_CODEC(SymlinkNode)
LIQUID_DEFINE_REAPI_CODEC(Directory)
LIQUID_DEFINE_REAPI_CODEC(EnvironmentVariable)
LIQUID_DEFINE_REAPI_CODEC(PlatformProperty)
LIQUID_DEFINE_REAPI_CODEC(Platform)
LIQUID_DEFINE_REAPI_CODEC(Command)
LIQUID_DEFINE_REAPI_CODEC(Action)
LIQUID_DEFINE_REAPI_CODEC(ExecutedActionMetadata)
LIQUID_DEFINE_REAPI_CODEC(OutputFile)
LIQUID_DEFINE_REAPI_CODEC(OutputDirectory)
LIQUID_DEFINE_REAPI_CODEC(OutputSymlink)
LIQUID_DEFINE_REAPI_CODEC(Tree)
LIQUID_DEFINE_REAPI_CODEC(LogFile)
LIQUID_DEFINE_REAPI_CODEC(ActionResult)
LIQUID_DEFINE_REAPI_CODEC(ExecutionPolicy)
LIQUID_DEFINE_REAPI_CODEC(ResultsCachePolicy)
LIQUID_DEFINE_REAPI_CODEC(ExecuteRequest)
LIQUID_DEFINE_REAPI_CODEC(ExecuteResponse)
LIQUID_DEFINE_REAPI_CODEC(ExecuteOperationMetadata)
LIQUID_DEFINE_REAPI_CODEC(WaitExecutionRequest)
LIQUID_DEFINE_REAPI_CODEC(Empty)
LIQUID_DEFINE_REAPI_CODEC(CancelOperationRequest)
LIQUID_DEFINE_REAPI_CODEC(GetActionResultRequest)
LIQUID_DEFINE_REAPI_CODEC(UpdateActionResultRequest)
LIQUID_DEFINE_REAPI_CODEC(FindMissingBlobsRequest)
LIQUID_DEFINE_REAPI_CODEC(FindMissingBlobsResponse)
LIQUID_DEFINE_REAPI_CODEC(BatchUpdateBlobsRequestEntry)
LIQUID_DEFINE_REAPI_CODEC(BatchUpdateBlobsRequest)
LIQUID_DEFINE_REAPI_CODEC(BatchUpdateBlobsResponseEntry)
LIQUID_DEFINE_REAPI_CODEC(BatchUpdateBlobsResponse)
LIQUID_DEFINE_REAPI_CODEC(BatchReadBlobsRequest)
LIQUID_DEFINE_REAPI_CODEC(BatchReadBlobsResponseEntry)
LIQUID_DEFINE_REAPI_CODEC(BatchReadBlobsResponse)
LIQUID_DEFINE_REAPI_CODEC(GetTreeRequest)
LIQUID_DEFINE_REAPI_CODEC(GetTreeResponse)
LIQUID_DEFINE_REAPI_CODEC(GetCapabilitiesRequest)
LIQUID_DEFINE_REAPI_CODEC(ActionCacheUpdateCapabilities)
LIQUID_DEFINE_REAPI_CODEC(PriorityRange)
LIQUID_DEFINE_REAPI_CODEC(PriorityCapabilities)
LIQUID_DEFINE_REAPI_CODEC(FastCdc2020Params)
LIQUID_DEFINE_REAPI_CODEC(RepMaxCdcParams)
LIQUID_DEFINE_REAPI_CODEC(CacheCapabilities)
LIQUID_DEFINE_REAPI_CODEC(ExecutionCapabilities)
LIQUID_DEFINE_REAPI_CODEC(ServerCapabilities)
LIQUID_DEFINE_REAPI_CODEC(ReadRequest)
LIQUID_DEFINE_REAPI_CODEC(ReadResponse)
LIQUID_DEFINE_REAPI_CODEC(WriteRequest)
LIQUID_DEFINE_REAPI_CODEC(WriteResponse)
LIQUID_DEFINE_REAPI_CODEC(QueryWriteStatusRequest)
LIQUID_DEFINE_REAPI_CODEC(QueryWriteStatusResponse)

#undef LIQUID_DEFINE_REAPI_CODEC

} // namespace liquid::reapi
