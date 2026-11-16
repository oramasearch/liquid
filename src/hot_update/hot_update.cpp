// SPDX-License-Identifier: MIT

#include "liquid/hot_update.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "build/repository_access.hpp"
#include "encoding/cbor.hpp"
#include "formats/json.hpp"
#include "liquid/abi.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

auto failure(errc code, std::string message, std::string context = "hot update") -> error {
    return {code, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<std::string, cbor::value>> entries) -> cbor::value {
    auto output = cbor::value::map{};
    for (auto& entry : entries) {
        output.emplace_back(entry.first, entry.second);
    }
    return output;
}

auto field(const cbor::value& value, std::string_view name) -> const cbor::value* {
    const auto* object = std::get_if<cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    for (const auto& [key, child] : *object) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &child;
        }
    }
    return nullptr;
}

auto digest_value(const digest& value) -> cbor::value {
    return cbor::value{to_string(value)};
}

auto read_digest(const cbor::value& value, std::string_view name) -> result<digest> {
    const auto* item = field(value, name);
    const auto* text = item ? std::get_if<std::string>(&item->data) : nullptr;
    if (!text) {
        return std::unexpected(failure(errc::corrupt, "missing digest field", std::string{name}));
    }
    return parse_digest(*text);
}

auto decode_canonical(std::span<const std::byte> bytes, std::string_view context)
    -> result<cbor::value> {
    auto decoded = cbor::decode(bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    auto encoded = cbor::encode(*decoded);
    if (!encoded || !std::ranges::equal(*encoded, bytes)) {
        return std::unexpected(
            failure(errc::corrupt, "record is not canonical CBOR", std::string{context}));
    }
    return decoded;
}

auto evidence_key(result_id value) -> std::string {
    return "hot_update/evidence/" + to_string(value.value);
}

auto patch_key(result_id value, digest record) -> std::string {
    return "hot_update/patches/" + to_string(value.value) + "/" + to_string(record);
}

auto outcome_key(result_id value, digest record) -> std::string {
    return "hot_update/outcomes/" + to_string(value.value) + "/" + to_string(record);
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(std::span<const std::byte> value) -> std::string_view {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto unit_key(const hot_update_unit& unit) -> std::string {
    return std::to_string(static_cast<std::uint8_t>(unit.kind)) + "\n" + unit.name;
}

auto attribute(const attestation& statement, std::string_view name) -> const attestation_value* {
    const auto found = std::ranges::find(statement.attributes, name, &attestation_attribute::name);
    return found == statement.attributes.end() ? nullptr : &found->value;
}

auto attribute_text(const attestation& statement, std::string_view name) -> const std::string* {
    const auto* value = attribute(statement, name);
    return value ? std::get_if<std::string>(value) : nullptr;
}

auto attribute_text_list(const attestation& statement, std::string_view name)
    -> const std::vector<std::string>* {
    const auto* value = attribute(statement, name);
    return value ? std::get_if<std::vector<std::string>>(value) : nullptr;
}

auto plan(hot_update_build_evidence old_value,
          hot_update_build_evidence new_value,
          std::vector<digest> evidence,
          std::vector<digest> abi_evidence) -> hot_update_plan {
    auto output = hot_update_plan{
        .supersedes = old_value.result,
        .target = new_value.result,
        .evidence = std::move(evidence),
    };
    if (old_value.units.empty() || new_value.units.empty()) {
        output.reason = "the changed input has no frontend units";
        return output;
    }
    auto old_units = std::map<std::string, const hot_update_unit*>{};
    auto new_units = std::map<std::string, const hot_update_unit*>{};
    for (const auto& unit : old_value.units) {
        if (!old_units.emplace(unit_key(unit), &unit).second) {
            output.reason = "the old build has duplicate frontend units";
            return output;
        }
    }
    for (const auto& unit : new_value.units) {
        if (!new_units.emplace(unit_key(unit), &unit).second) {
            output.reason = "the new build has duplicate frontend units";
            return output;
        }
    }
    auto body_changes = std::vector<std::string>{};
    auto additions = std::vector<std::string>{};
    for (const auto& [key, old_unit] : old_units) {
        const auto found = new_units.find(key);
        if (found == new_units.end()) {
            output.tier = hot_update_tier::tier_3;
            output.reason = "an existing unit was removed or changed kind";
            output.symbols.push_back(old_unit->name);
            return output;
        }
        const auto* new_unit = found->second;
        if (old_unit->signature != new_unit->signature) {
            output.tier = hot_update_tier::tier_3;
            output.reason = "an existing symbol signature changed";
            output.symbols.push_back(old_unit->name);
            return output;
        }
        if (!same_digest(old_unit->output, new_unit->output)) {
            if (old_unit->kind != frontend_unit_kind::definition) {
                output.tier = hot_update_tier::tier_3;
                output.reason = "a non-definition unit changed";
                output.symbols.push_back(old_unit->name);
                return output;
            }
            body_changes.push_back(old_unit->name);
        }
    }
    for (const auto& [key, new_unit] : new_units) {
        if (old_units.contains(key)) {
            continue;
        }
        if (new_unit->kind != frontend_unit_kind::definition) {
            output.tier = hot_update_tier::tier_3;
            output.reason = "a non-definition unit was added";
            output.symbols.push_back(new_unit->name);
            return output;
        }
        additions.push_back(new_unit->name);
    }
    if (body_changes.empty() && additions.empty()) {
        output.tier = hot_update_tier::tier_0;
        output.outcome = hot_update_outcome::patched;
        output.reason = "every unit output fingerprint is unchanged";
        return output;
    }
    if (!additions.empty()) {
        output.symbols = additions;
        if (!body_changes.empty()) {
            output.symbols.insert(output.symbols.end(), body_changes.begin(), body_changes.end());
            std::ranges::sort(output.symbols);
            output.tier = hot_update_tier::tier_3;
            output.reason = "the build both added definitions and changed existing code";
            return output;
        }
        if (abi_evidence.empty()) {
            output.tier = hot_update_tier::tier_3;
            output.reason = "additive definitions lack an unchanged-layout ABI descriptor";
            return output;
        }
        output.tier = hot_update_tier::tier_2;
        output.outcome = hot_update_outcome::patched;
        output.evidence.insert(output.evidence.end(), abi_evidence.begin(), abi_evidence.end());
        output.reason = "only definitions were added and the ABI descriptor preserves layouts";
        return output;
    }
    output.tier = hot_update_tier::tier_1;
    output.outcome = hot_update_outcome::patched;
    output.symbols = std::move(body_changes);
    std::ranges::sort(output.symbols);
    output.reason = "only definition bodies changed";
    return output;
}

struct loaded_evidence {
    hot_update_build_evidence value;
    digest record;
};

auto load_abi(repository& source, digest record) -> runtime::task<abi_descriptor> {
    auto record_bytes = co_await repository_detail::build_access::get(source, record);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    co_return objects::decode_abi(*record_bytes);
}

auto abi_confirmation(repository& source,
                      const action_result& old_build,
                      const action_result& new_build,
                      std::optional<digest> selected) -> runtime::task<std::vector<digest>> {
    if (!selected || !std::ranges::any_of(new_build.abis, [&](const auto& value) {
            return same_digest(value, *selected);
        })) {
        co_return std::vector<digest>{};
    }
    auto new_descriptor = co_await load_abi(source, *selected);
    if (!new_descriptor) {
        co_return std::unexpected(new_descriptor.error());
    }
    const auto new_output = std::ranges::find_if(new_build.outputs, [&](const auto& output) {
        return same_digest(output.blob, new_descriptor->subject);
    });
    if (new_output == new_build.outputs.end()) {
        co_return std::vector<digest>{};
    }
    const auto old_output =
        std::ranges::find(old_build.outputs, new_output->path, &action_output::path);
    if (old_output == old_build.outputs.end()) {
        co_return std::vector<digest>{};
    }
    for (const auto& old_record : old_build.abis) {
        auto old_descriptor = co_await load_abi(source, old_record);
        if (!old_descriptor) {
            co_return std::unexpected(old_descriptor.error());
        }
        if (!same_digest(old_descriptor->subject, old_output->blob)) {
            continue;
        }
        const auto report = diff_abi(*old_descriptor, *new_descriptor);
        if (old_descriptor->binary != new_descriptor->binary ||
            old_descriptor->architecture != new_descriptor->architecture ||
            old_descriptor->install_name != new_descriptor->install_name ||
            old_descriptor->needed_libraries != new_descriptor->needed_libraries ||
            report.verdict == abi_verdict::incompatible) {
            co_return std::vector<digest>{};
        }
        co_return std::vector<digest>{old_record, *selected};
    }
    co_return std::vector<digest>{};
}

auto load_evidence(repository& source, result_id id)
    -> runtime::task<std::optional<loaded_evidence>> {
    auto association = co_await source.metadata(evidence_key(id));
    if (!association) {
        co_return std::unexpected(association.error());
    }
    if (!*association) {
        co_return std::optional<loaded_evidence>{};
    }
    auto record = parse_digest(text(**association));
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto record_bytes = co_await repository_detail::build_access::get(source, *record);
    if (!record_bytes) {
        co_return std::unexpected(record_bytes.error());
    }
    auto value = decode_hot_update_evidence(*record_bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (!same_digest(value->result.value, id.value)) {
        co_return std::unexpected(
            failure(errc::corrupt, "evidence association names a different result"));
    }
    co_return std::optional<loaded_evidence>{{std::move(*value), *record}};
}

auto patch_attestation_matches(const attestation& statement,
                               const hot_update_patch& value,
                               digest record) -> bool {
    const auto* supersedes = attribute_text(statement, "supersedes");
    const auto* target = attribute_text(statement, "target");
    const auto* tier = attribute_text(statement, "tier");
    const auto* symbols = attribute_text_list(statement, "symbols");
    const auto* object = attribute_text(statement, "object");
    const auto* patch = attribute_text(statement, "patch");
    const auto* outcome = attribute_text(statement, "outcome");
    return statement.profile == "hot_update/1" &&
           same_digest(statement.subject, value.target.value) && supersedes && target && tier &&
           symbols && object && patch && outcome &&
           *supersedes == to_string(value.supersedes.value) &&
           *target == to_string(value.target.value) && *tier == to_string(value.tier) &&
           *symbols == value.symbols && *object == to_string(value.object) &&
           *patch == to_string(record) && *outcome == "patched";
}

auto encode_outcome_record(const hot_update_plan& value) -> result<std::vector<std::byte>> {
    auto symbols = cbor::value::array{};
    for (const auto& symbol : value.symbols) {
        symbols.emplace_back(symbol);
    }
    auto evidence = cbor::value::array{};
    for (const auto& record : value.evidence) {
        evidence.push_back(digest_value(record));
    }
    return cbor::encode(map({{"v", cbor::value{std::uint64_t{1}}},
                             {"kind", cbor::value{"hot-update-outcome"}},
                             {"supersedes", digest_value(value.supersedes.value)},
                             {"target", digest_value(value.target.value)},
                             {"tier", cbor::value{static_cast<std::uint64_t>(value.tier)}},
                             {"outcome", cbor::value{std::string{to_string(value.outcome)}}},
                             {"symbols", cbor::value{std::move(symbols)}},
                             {"evidence", cbor::value{std::move(evidence)}}}));
}

auto runtime_payload(const hot_update_patch& value, const std::filesystem::path& object)
    -> result<std::vector<std::byte>> {
    const auto path = object.string();
    if (path.empty() || path.contains('\n')) {
        return std::unexpected(failure(errc::invalid_argument, "invalid patch object path"));
    }
    auto payload = std::string{"liquid-hot/1\n"} + path +
                   "\ntier=" + std::string{to_string(value.tier)} + "\n";
    for (const auto& symbol : value.symbols) {
        if (symbol.empty() || symbol.contains('\n') || symbol.contains('=')) {
            return std::unexpected(failure(errc::invalid_argument, "invalid patch symbol"));
        }
        payload += symbol + "=" + symbol + "\n";
    }
    return bytes(payload);
}

auto write_patch_object(std::filesystem::path path, std::vector<std::byte> object) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return std::unexpected(failure(errc::io, "cannot create patch object", path.string()));
    }
    stream.write(reinterpret_cast<const char*>(object.data()),
                 static_cast<std::streamsize>(object.size()));
    stream.close();
    if (!stream) {
        return std::unexpected(failure(errc::io, "cannot write patch object", path.string()));
    }
    return {};
}

auto send_runtime_patch(const std::filesystem::path& endpoint, std::span<const std::byte> payload)
    -> result<void> {
#if defined(_WIN32)
    static_cast<void>(endpoint);
    static_cast<void>(payload);
    return std::unexpected(failure(errc::unsupported, "hot update is unsupported on Windows"));
#else
    if (endpoint.empty() || endpoint.string().size() >= sizeof(sockaddr_un::sun_path) ||
        payload.empty() || payload.size() > 64U * 1024U * 1024U) {
        return std::unexpected(failure(errc::invalid_argument, "invalid hot-update delivery"));
    }
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), endpoint.string()));
    }
    auto address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    std::ranges::copy(endpoint.string(), address.sun_path);
    const auto close_descriptor = [&] { ::close(descriptor); };
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto message = std::string{std::strerror(errno)};
        close_descriptor();
        return std::unexpected(failure(errc::io, message, endpoint.string()));
    }
    const auto size = static_cast<std::uint32_t>(payload.size());
    const auto size_bytes = std::array<std::byte, 4>{
        std::byte{static_cast<std::uint8_t>((size >> 24U) & 0xffU)},
        std::byte{static_cast<std::uint8_t>((size >> 16U) & 0xffU)},
        std::byte{static_cast<std::uint8_t>((size >> 8U) & 0xffU)},
        std::byte{static_cast<std::uint8_t>(size & 0xffU)},
    };
    const auto send_all = [descriptor](std::span<const std::byte> value) -> result<void> {
        auto offset = std::size_t{};
        while (offset < value.size()) {
            const auto written = ::write(descriptor, value.data() + offset, value.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return std::unexpected(
                    failure(errc::io, std::strerror(errno), "hot-update endpoint"));
            }
            offset += static_cast<std::size_t>(written);
        }
        return {};
    };
    auto sent = send_all(size_bytes);
    if (sent) {
        sent = send_all(payload);
    }
    if (sent && ::shutdown(descriptor, SHUT_WR) != 0) {
        sent = std::unexpected(
            failure(errc::io, std::strerror(errno), "hot-update endpoint acknowledgement"));
    }
    if (sent) {
        auto response = std::byte{};
        auto count = ssize_t{};
        do {
            count = ::read(descriptor, &response, sizeof(response));
        } while (count < 0 && errno == EINTR);
        if (count != 1) {
            sent = std::unexpected(
                failure(errc::io, "runtime closed without accepting the patch", endpoint.string()));
        } else if (response != std::byte{}) {
            sent =
                std::unexpected(failure(errc::io, "runtime rejected the patch", endpoint.string()));
        }
    }
    close_descriptor();
    return sent;
#endif
}

} // namespace

std::string_view to_string(hot_update_tier value) noexcept {
    switch (value) {
    case hot_update_tier::tier_0:
        return "0";
    case hot_update_tier::tier_1:
        return "1";
    case hot_update_tier::tier_2:
        return "2";
    case hot_update_tier::tier_3:
        return "3";
    case hot_update_tier::tier_4:
        return "4";
    }
    return "unknown";
}

std::string_view to_string(hot_update_outcome value) noexcept {
    switch (value) {
    case hot_update_outcome::patched:
        return "patched";
    case hot_update_outcome::restart:
        return "restart";
    }
    return "unknown";
}

result<std::vector<std::byte>> encode_hot_update_evidence(const hot_update_build_evidence& value) {
    auto units = cbor::value::array{};
    for (const auto& unit : value.units) {
        if (unit.name.empty() || unit.signature.empty()) {
            return std::unexpected(
                failure(errc::invalid_argument, "hot-update unit identity is incomplete"));
        }
        units.push_back(map({
            {"kind", cbor::value{static_cast<std::uint64_t>(unit.kind)}},
            {"name", cbor::value{unit.name}},
            {"signature", cbor::value{unit.signature}},
            {"source", digest_value(unit.source)},
            {"output", digest_value(unit.output)},
        }));
    }
    auto abi =
        value.abi_descriptor ? digest_value(*value.abi_descriptor) : cbor::value{cbor::null{}};
    return cbor::encode(map({
        {"v", cbor::value{std::uint64_t{1}}},
        {"kind", cbor::value{"hot-update-evidence"}},
        {"result", digest_value(value.result.value)},
        {"artifact", digest_value(value.artifact)},
        {"changed_input", cbor::value{value.changed_input}},
        {"units", cbor::value{std::move(units)}},
        {"abi", std::move(abi)},
    }));
}

result<hot_update_build_evidence>
decode_hot_update_evidence(std::span<const std::byte> bytes_value) {
    auto decoded = decode_canonical(bytes_value, "hot-update evidence");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = field(*decoded, "v");
    const auto* kind_value = field(*decoded, "kind");
    const auto* input_value = field(*decoded, "changed_input");
    const auto* units_value = field(*decoded, "units");
    const auto* abi_value = field(*decoded, "abi");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* input = input_value ? std::get_if<std::string>(&input_value->data) : nullptr;
    const auto* units = units_value ? std::get_if<cbor::value::array>(&units_value->data) : nullptr;
    if (!version || *version != 1 || !kind || *kind != "hot-update-evidence" || !input || !units ||
        !abi_value) {
        return std::unexpected(failure(errc::corrupt, "invalid hot-update evidence record"));
    }
    auto result_value = read_digest(*decoded, "result");
    auto artifact = read_digest(*decoded, "artifact");
    if (!result_value || !artifact) {
        return std::unexpected(result_value ? artifact.error() : result_value.error());
    }
    auto output = hot_update_build_evidence{
        .result = {*result_value},
        .artifact = *artifact,
        .changed_input = *input,
    };
    if (const auto* abi_text = std::get_if<std::string>(&abi_value->data)) {
        auto abi = parse_digest(*abi_text);
        if (!abi) {
            return std::unexpected(abi.error());
        }
        output.abi_descriptor = *abi;
    } else if (!std::holds_alternative<cbor::null>(abi_value->data)) {
        return std::unexpected(failure(errc::corrupt, "invalid hot-update ABI descriptor"));
    }
    for (const auto& item : *units) {
        const auto* kind_item = field(item, "kind");
        const auto* name_item = field(item, "name");
        const auto* signature_item = field(item, "signature");
        const auto* unit_kind = kind_item ? std::get_if<std::uint64_t>(&kind_item->data) : nullptr;
        const auto* name = name_item ? std::get_if<std::string>(&name_item->data) : nullptr;
        const auto* signature =
            signature_item ? std::get_if<std::string>(&signature_item->data) : nullptr;
        if (!unit_kind ||
            *unit_kind < static_cast<std::uint64_t>(frontend_unit_kind::declaration) ||
            *unit_kind > static_cast<std::uint64_t>(frontend_unit_kind::type) || !name ||
            name->empty() || !signature || signature->empty()) {
            return std::unexpected(failure(errc::corrupt, "invalid hot-update unit"));
        }
        auto source = read_digest(item, "source");
        auto unit_output = read_digest(item, "output");
        if (!source || !unit_output) {
            return std::unexpected(source ? unit_output.error() : source.error());
        }
        output.units.push_back({static_cast<frontend_unit_kind>(*unit_kind),
                                *name,
                                *signature,
                                *source,
                                *unit_output});
    }
    return output;
}

result<std::vector<std::byte>> encode_hot_update_patch(const hot_update_patch& value) {
    if (value.v != 1 ||
        (value.tier != hot_update_tier::tier_1 && value.tier != hot_update_tier::tier_2) ||
        value.symbols.empty()) {
        return std::unexpected(failure(errc::invalid_argument, "invalid hot-update patch"));
    }
    auto symbols = cbor::value::array{};
    for (const auto& symbol : value.symbols) {
        if (symbol.empty()) {
            return std::unexpected(failure(errc::invalid_argument, "patch symbol is empty"));
        }
        symbols.emplace_back(symbol);
    }
    return cbor::encode(map({
        {"v", cbor::value{value.v}},
        {"kind", cbor::value{"patch"}},
        {"supersedes", digest_value(value.supersedes.value)},
        {"target", digest_value(value.target.value)},
        {"tier", cbor::value{static_cast<std::uint64_t>(value.tier)}},
        {"symbols", cbor::value{std::move(symbols)}},
        {"object", digest_value(value.object)},
    }));
}

result<hot_update_patch> decode_hot_update_patch(std::span<const std::byte> bytes_value) {
    auto decoded = decode_canonical(bytes_value, "hot-update patch");
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = field(*decoded, "v");
    const auto* kind_value = field(*decoded, "kind");
    const auto* tier_value = field(*decoded, "tier");
    const auto* symbols_value = field(*decoded, "symbols");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* kind = kind_value ? std::get_if<std::string>(&kind_value->data) : nullptr;
    const auto* tier = tier_value ? std::get_if<std::uint64_t>(&tier_value->data) : nullptr;
    const auto* symbols =
        symbols_value ? std::get_if<cbor::value::array>(&symbols_value->data) : nullptr;
    if (!version || *version != 1 || !kind || *kind != "patch" || !tier ||
        (*tier != 1 && *tier != 2) || !symbols || symbols->empty()) {
        return std::unexpected(failure(errc::corrupt, "invalid hot-update patch record"));
    }
    auto supersedes = read_digest(*decoded, "supersedes");
    auto target = read_digest(*decoded, "target");
    auto object = read_digest(*decoded, "object");
    if (!supersedes || !target || !object) {
        return std::unexpected(!supersedes ? supersedes.error()
                               : !target   ? target.error()
                                           : object.error());
    }
    auto output = hot_update_patch{
        .v = 1,
        .supersedes = {*supersedes},
        .target = {*target},
        .tier = static_cast<hot_update_tier>(*tier),
        .object = *object,
    };
    for (const auto& item : *symbols) {
        const auto* symbol = std::get_if<std::string>(&item.data);
        if (!symbol || symbol->empty()) {
            return std::unexpected(failure(errc::corrupt, "invalid patch symbol"));
        }
        output.symbols.push_back(*symbol);
    }
    return output;
}

result<std::string> hot_update_plan_json(const hot_update_plan& value) {
    auto symbols = formats::json::value::array{};
    for (const auto& symbol : value.symbols) {
        symbols.emplace_back(symbol);
    }
    auto evidence = formats::json::value::array{};
    for (const auto& record : value.evidence) {
        evidence.emplace_back(to_string(record));
    }
    return formats::json::write(formats::json::value::object{
        {"tier", formats::json::number{std::string{to_string(value.tier)}}},
        {"outcome", std::string{to_string(value.outcome)}},
        {"supersedes", to_string(value.supersedes.value)},
        {"target", to_string(value.target.value)},
        {"symbols", std::move(symbols)},
        {"evidence", std::move(evidence)},
        {"reason", value.reason},
    });
}

result<std::string> hot_update_status_json(const hot_update_composition& value) {
    if (value.records.size() != value.patches.size()) {
        return std::unexpected(
            failure(errc::invalid_argument, "hot-update composition is incomplete"));
    }
    auto patches = formats::json::value::array{};
    for (std::size_t index = 0; index < value.patches.size(); ++index) {
        auto symbols = formats::json::value::array{};
        for (const auto& symbol : value.patches[index].symbols) {
            symbols.emplace_back(symbol);
        }
        patches.emplace_back(formats::json::value::object{
            {"record", to_string(value.records[index])},
            {"target", to_string(value.patches[index].target.value)},
            {"tier", formats::json::number{std::string{to_string(value.patches[index].tier)}}},
            {"symbols", std::move(symbols)},
            {"object", to_string(value.patches[index].object)},
        });
    }
    return formats::json::write(formats::json::value::object{
        {"artifact", to_string(value.artifact.value)},
        {"patches", std::move(patches)},
    });
}

namespace hot_update {

runtime::task<digest> record_evidence(repository& source, hot_update_build_evidence value) {
    auto result_value = co_await build::result(source, value.result);
    if (!result_value) {
        co_return std::unexpected(result_value.error());
    }
    auto encoded = encode_hot_update_evidence(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto stored = co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto associated =
        co_await source.set_metadata(evidence_key(value.result), bytes(to_string(*stored)));
    if (!associated) {
        co_return std::unexpected(associated.error());
    }
    co_return *stored;
}

runtime::task<hot_update_plan>
classify(repository& source, result_id old_result, result_id new_result) {
    auto old_build = co_await build::result(source, old_result);
    if (!old_build) {
        co_return std::unexpected(old_build.error());
    }
    auto new_build = co_await build::result(source, new_result);
    if (!new_build) {
        co_return std::unexpected(new_build.error());
    }
    auto old_value = co_await load_evidence(source, old_result);
    if (!old_value) {
        co_return std::unexpected(old_value.error());
    }
    auto new_value = co_await load_evidence(source, new_result);
    if (!new_value) {
        co_return std::unexpected(new_value.error());
    }
    if (!*old_value || !*new_value) {
        co_return hot_update_plan{
            .supersedes = old_result,
            .target = new_result,
            .reason = "the changed input has no frontend evidence",
        };
    }
    auto abi_evidence = co_await abi_confirmation(
        source, *old_build, *new_build, (*new_value)->value.abi_descriptor);
    if (!abi_evidence) {
        co_return std::unexpected(abi_evidence.error());
    }
    co_return plan((*old_value)->value,
                   (*new_value)->value,
                   {(*old_value)->record, (*new_value)->record},
                   std::move(*abi_evidence));
}

runtime::task<hot_update_patch_creation>
create_patch(repository& source, hot_update_plan plan_value, std::vector<std::byte> object) {
    if (plan_value.outcome != hot_update_outcome::patched ||
        (plan_value.tier != hot_update_tier::tier_1 &&
         plan_value.tier != hot_update_tier::tier_2) ||
        plan_value.symbols.empty() || object.empty()) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "plan cannot produce a hot-update patch"));
    }
    auto object_id =
        co_await repository_detail::build_access::put_record(source, std::move(object));
    if (!object_id) {
        co_return std::unexpected(object_id.error());
    }
    auto value = hot_update_patch{.supersedes = plan_value.supersedes,
                                  .target = plan_value.target,
                                  .tier = plan_value.tier,
                                  .symbols = std::move(plan_value.symbols),
                                  .object = *object_id};
    auto encoded = encode_hot_update_patch(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto statement = attestation{
        .subject = value.target.value,
        .profile = "hot_update/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"supersedes", to_string(value.supersedes.value)},
                       {"target", to_string(value.target.value)},
                       {"tier", std::string{to_string(value.tier)}},
                       {"symbols", value.symbols},
                       {"object", to_string(value.object)},
                       {"patch", to_string(*record)},
                       {"outcome", std::string{"patched"}}},
    };
    auto signed_value =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto attestation_id = co_await source.store_attestation(std::move(statement));
    if (!attestation_id) {
        co_return std::unexpected(attestation_id.error());
    }
    auto associated = co_await source.set_metadata(patch_key(value.target, *record), {});
    if (!associated) {
        co_return std::unexpected(associated.error());
    }
    co_return hot_update_patch_creation{value, *record, *attestation_id};
}

runtime::task<hot_update_patch_creation> create_patch(repository& source,
                                                      hot_update_plan plan_value) {
    auto evidence = co_await load_evidence(source, plan_value.target);
    if (!evidence) {
        co_return std::unexpected(evidence.error());
    }
    if (!*evidence) {
        co_return std::unexpected(
            failure(errc::not_found, "target build has no hot-update evidence"));
    }
    auto object =
        co_await repository_detail::build_access::get(source, (*evidence)->value.artifact);
    if (!object) {
        co_return std::unexpected(object.error());
    }
    co_return co_await create_patch(source, std::move(plan_value), std::move(*object));
}

runtime::task<hot_update_patch>
verify_patch(repository& source, digest record, hot_update_trust trust) {
    // liquid#388: the patch is a shared library loaded into a running process, so only a
    // configured signer may authorize it. With no signer configured, no patch is delivered.
    if (trust.signers.empty()) {
        co_return std::unexpected(failure(
            errc::unauthorized, "no hot-update signers are configured; every patch is refused"));
    }
    auto bytes_value = co_await repository_detail::build_access::get(source, record);
    if (!bytes_value) {
        co_return std::unexpected(bytes_value.error());
    }
    auto value = decode_hot_update_patch(*bytes_value);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto page = record_page_options{};
    auto invalid = std::optional<std::string>{};
    auto untrusted = false;
    while (true) {
        auto statements = co_await source.attestations(value->target.value, page);
        if (!statements) {
            co_return std::unexpected(statements.error());
        }
        for (const auto& item : statements->records) {
            if (!patch_attestation_matches(item.value, *value, record)) {
                continue;
            }
            // An attestation from an identity outside the signer set never counts, however
            // valid its signature and chain; it does not hide a trusted one stored later.
            if (!std::ranges::contains(trust.signers, item.value.issuer.taap_id)) {
                untrusted = true;
                continue;
            }
            // The issuer's name is bound to a key only through its identity chain in this
            // machine's identity store; the key carried in the record is not trusted.
            auto verified = verify_attestation(item.value);
            if (!verified) {
                co_return std::unexpected(verified.error());
            }
            if (!verified->verified) {
                invalid = verified->reason;
                continue;
            }
            co_return *value;
        }
        if (statements->done || !statements->next) {
            break;
        }
        page.cursor = statements->next;
    }
    if (invalid) {
        co_return std::unexpected(
            failure(errc::unauthorized, "hot-update attestation is invalid: " + *invalid));
    }
    if (untrusted) {
        co_return std::unexpected(
            failure(errc::unauthorized,
                    "hot-update patch is not signed by a configured hot-update signer"));
    }
    co_return std::unexpected(failure(errc::unauthorized, "hot-update patch is unsigned"));
}

runtime::task<hot_update_apply_result>
apply(repository& source, digest record, std::filesystem::path endpoint, hot_update_trust trust) {
    auto value = co_await verify_patch(source, record, std::move(trust));
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto object = co_await repository_detail::build_access::get(source, value->object);
    if (!object) {
        co_return std::unexpected(object.error());
    }
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
#if defined(__APPLE__)
    constexpr auto suffix = ".dylib";
#else
    constexpr auto suffix = ".so";
#endif
    auto object_name = to_string(value->object);
    std::ranges::replace(object_name, ':', '-');
    const auto path = *root / ".liquid" / "hot-update" / (object_name + suffix);
    auto written = co_await runtime::offload([path, object = std::move(*object)]() mutable {
        return write_patch_object(path, std::move(object));
    });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (!*written) {
        co_return std::unexpected(written->error());
    }
    auto payload = runtime_payload(*value, path);
    if (!payload) {
        co_return std::unexpected(payload.error());
    }
    auto sent =
        co_await runtime::offload([endpoint = std::move(endpoint), payload = std::move(*payload)] {
            return send_runtime_patch(endpoint, payload);
        });
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    if (!*sent) {
        co_return std::unexpected(sent->error());
    }
    co_return hot_update_apply_result{
        hot_update_outcome::patched, record, value->target, value->symbols};
}

runtime::task<hot_update_apply_result> record_outcome(repository& source,
                                                      hot_update_plan plan_value) {
    if (!((plan_value.tier == hot_update_tier::tier_0 &&
           plan_value.outcome == hot_update_outcome::patched) ||
          ((plan_value.tier == hot_update_tier::tier_3 ||
            plan_value.tier == hot_update_tier::tier_4) &&
           plan_value.outcome == hot_update_outcome::restart))) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "plan requires a patch or has an invalid outcome"));
    }
    auto encoded = encode_outcome_record(plan_value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto record = co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto statement = attestation{
        .subject = plan_value.target.value,
        .profile = "hot_update/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"supersedes", to_string(plan_value.supersedes.value)},
                       {"target", to_string(plan_value.target.value)},
                       {"tier", std::string{to_string(plan_value.tier)}},
                       {"symbols", plan_value.symbols},
                       {"object", to_string(plan_value.target.value)},
                       {"patch", to_string(*record)},
                       {"outcome", std::string{to_string(plan_value.outcome)}}},
    };
    auto signed_value =
        co_await repository_detail::build_access::sign_attestation(source, statement);
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto attestation_id = co_await source.store_attestation(std::move(statement));
    if (!attestation_id) {
        co_return std::unexpected(attestation_id.error());
    }
    auto associated = co_await source.set_metadata(outcome_key(plan_value.target, *record), {});
    if (!associated) {
        co_return std::unexpected(associated.error());
    }
    co_return hot_update_apply_result{
        plan_value.outcome, *record, plan_value.target, plan_value.symbols};
}

runtime::task<hot_update_apply_result> restart(repository& source,
                                               hot_update_plan plan_value,
                                               std::filesystem::path working_tree,
                                               std::vector<std::string> hook) {
    if (plan_value.outcome != hot_update_outcome::restart ||
        (plan_value.tier != hot_update_tier::tier_3 &&
         plan_value.tier != hot_update_tier::tier_4) ||
        hook.empty() || std::ranges::any_of(hook, &std::string::empty)) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "plan cannot invoke a hot-update restart hook"));
    }
    hook.push_back("restart");
    hook.push_back(to_string(plan_value.supersedes.value));
    hook.push_back(to_string(plan_value.target.value));
    hook.push_back(std::string{to_string(plan_value.tier)});
    auto invoked = co_await build::wrap(
        std::move(working_tree), std::move(hook), {.cache = false, .verify = false});
    if (!invoked) {
        co_return std::unexpected(invoked.error());
    }
    if (invoked->exit != 0) {
        co_return std::unexpected(failure(
            errc::io, "hot-update restart hook failed with exit " + std::to_string(invoked->exit)));
    }
    co_return co_await record_outcome(source, std::move(plan_value));
}

runtime::task<hot_update_composition>
status(repository& source, result_id target, hot_update_trust trust) {
    auto output = hot_update_composition{.artifact = target};
    auto visited = std::set<std::string>{};
    auto current = target;
    while (true) {
        const auto current_text = to_string(current.value);
        if (!visited.insert(current_text).second) {
            co_return std::unexpected(failure(errc::corrupt, "hot-update patch chain has a cycle"));
        }
        auto keys = co_await source.metadata_keys("hot_update/patches/" + current_text + "/");
        if (!keys) {
            co_return std::unexpected(keys.error());
        }
        if (keys->empty()) {
            output.artifact = current;
            break;
        }
        if (keys->size() != 1) {
            co_return std::unexpected(
                failure(errc::conflict, "hot-update result has multiple patch records"));
        }
        const auto& key = keys->front();
        const auto slash = key.rfind('/');
        if (slash == std::string::npos) {
            co_return std::unexpected(failure(errc::corrupt, "invalid hot-update patch index"));
        }
        auto record = parse_digest(std::string_view{key}.substr(slash + 1));
        if (!record) {
            co_return std::unexpected(record.error());
        }
        auto value = co_await verify_patch(source, *record, trust);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        if (!same_digest(value->target.value, current.value)) {
            co_return std::unexpected(
                failure(errc::corrupt, "hot-update patch index names a different target"));
        }
        output.records.push_back(*record);
        output.patches.push_back(std::move(*value));
        current = output.patches.back().supersedes;
    }
    std::ranges::reverse(output.records);
    std::ranges::reverse(output.patches);
    co_return output;
}

} // namespace hot_update
} // namespace liquid
