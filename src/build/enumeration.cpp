#include "liquid/repository.hpp"

#include <algorithm>
#include <string_view>

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "objects/records.hpp"

namespace liquid {
namespace {

constexpr auto maximum_page_size = std::uint64_t{1'000};

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto page_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto validate_page(const record_page_options& options) -> result<void> {
    if (options.limit == 0 || options.limit > maximum_page_size) {
        return std::unexpected(page_error(errc::invalid_argument,
                                          "record page limit must be between 1 and 1000",
                                          std::to_string(options.limit)));
    }
    return {};
}

template <class Record, class Key>
auto make_page(std::vector<Record> records, const record_page_options& options, Key key)
    -> result<record_page<Record>> {
    auto valid = validate_page(options);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    auto begin = std::size_t{};
    if (options.cursor) {
        const auto found = std::ranges::find_if(
            records, [&](const Record& record) { return key(record) == options.cursor->token; });
        if (found == records.end()) {
            return std::unexpected(page_error(errc::invalid_argument,
                                              "record cursor is not in the filtered result set",
                                              options.cursor->token));
        }
        begin = static_cast<std::size_t>(std::distance(records.begin(), found)) + 1;
    }
    const auto remaining = records.size() - begin;
    const auto count = std::min<std::size_t>(remaining, options.limit);
    const auto end = begin + count;
    auto output = record_page<Record>{};
    output.records.insert(output.records.end(),
                          std::make_move_iterator(records.begin() + begin),
                          std::make_move_iterator(records.begin() + end));
    output.done = end == records.size();
    if (!output.done && !output.records.empty()) {
        output.next = record_cursor{key(output.records.back())};
    }
    return output;
}

auto result_matches(const repository_result& record, const result_filter& filter) -> bool {
    if (filter.id && !same_digest(record.id.value, filter.id->value)) {
        return false;
    }
    if (filter.action && !same_digest(record.value.action, *filter.action)) {
        return false;
    }
    if (!filter.output && !filter.content) {
        return true;
    }
    return std::ranges::any_of(record.value.outputs, [&](const action_output& output) {
        const auto path_matches = !filter.output || output.path == *filter.output;
        const auto content_matches = !filter.content || same_digest(output.blob, *filter.content);
        return path_matches && content_matches;
    });
}

auto attestation_matches(const repository_attestation& record, const attestation_filter& filter)
    -> bool {
    if (filter.id && !same_digest(record.id, *filter.id)) {
        return false;
    }
    if (filter.subject && !same_digest(record.value.subject, *filter.subject)) {
        return false;
    }
    if (filter.profile && record.value.profile != *filter.profile) {
        return false;
    }
    return !filter.issuer || record.value.issuer.taap_id == *filter.issuer;
}

auto sbom_cursor(const sbom_report& report) -> std::string {
    return to_string(report.attestation);
}

auto text_attribute(const attestation& value, std::string_view name) -> const std::string* {
    const auto found = std::ranges::find(value.attributes, name, &attestation_attribute::name);
    if (found == value.attributes.end()) {
        return nullptr;
    }
    return std::get_if<std::string>(&found->value);
}

auto sbom_matches(const repository_attestation& record,
                  const action_output& output,
                  sbom_format format) -> bool {
    if (record.value.profile != "sbom/1" || !same_digest(record.value.subject, output.blob)) {
        return false;
    }
    const auto* recorded_output = text_attribute(record.value, "output");
    const auto* recorded_format = text_attribute(record.value, "format");
    const auto expected_format =
        format == sbom_format::spdx ? std::string_view{"SPDX-2.3"} : "CycloneDX-1.5";
    return recorded_output && *recorded_output == output.path && recorded_format &&
           *recorded_format == expected_format;
}

} // namespace

runtime::task<record_page<repository_result>> repository::results(result_filter filter) {
    auto valid = validate_page(filter.page);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto cache = build_detail::disk_cache{*this};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto ids = std::vector<digest>{};
    if (filter.id) {
        ids.push_back(filter.id->value);
    } else {
        auto all = co_await record_ids("result");
        if (!all) {
            co_return std::unexpected(all.error());
        }
        ids = std::move(*all);
    }
    auto records = std::vector<repository_result>{};
    for (const auto& id : ids) {
        auto value = co_await build::result(*this, result_id{id});
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto record = repository_result{.id = {id}, .value = std::move(*value)};
        const auto indexed = std::ranges::find_if(
            cache.entries(), [&](const auto& entry) { return same_digest(entry.result, id); });
        if (indexed != cache.entries().end()) {
            record.sequence = indexed->sequence;
            record.divergent = indexed->divergent;
            record.verification = indexed->verification;
        }
        if (result_matches(record, filter)) {
            records.push_back(std::move(record));
        }
    }
    std::ranges::sort(records, [](const repository_result& left, const repository_result& right) {
        if (left.sequence != 0 && right.sequence != 0 && left.sequence != right.sequence) {
            return left.sequence > right.sequence;
        }
        if ((left.sequence == 0) != (right.sequence == 0)) {
            return left.sequence != 0;
        }
        if (left.value.observed_at != right.value.observed_at) {
            return left.value.observed_at > right.value.observed_at;
        }
        return to_string(left.id.value) < to_string(right.id.value);
    });
    co_return make_page(std::move(records), filter.page, [](const repository_result& record) {
        return to_string(record.id.value);
    });
}

runtime::task<action_input> repository::action(digest id) {
    auto bytes = co_await repository_detail::build_access::get(*this, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto record = objects::decode_action(*bytes);
    if (record) {
        co_return std::move(record->input);
    }
    auto action_result = objects::decode_result(*bytes);
    if (!action_result) {
        co_return std::unexpected(record.error());
    }
    bytes = co_await repository_detail::build_access::get(*this, action_result->action);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    record = objects::decode_action(*bytes);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    co_return std::move(record->input);
}

runtime::task<record_page<repository_attestation>>
repository::attestations(attestation_filter filter) {
    auto valid = validate_page(filter.page);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto ids = std::vector<digest>{};
    if (filter.id) {
        ids.push_back(*filter.id);
    } else if (filter.subject) {
        ids = repository_detail::build_access::attestations(*this, *filter.subject);
    } else {
        auto all = co_await record_ids("attestation");
        if (!all) {
            co_return std::unexpected(all.error());
        }
        ids = std::move(*all);
    }
    auto records = std::vector<repository_attestation>{};
    records.reserve(ids.size());
    for (const auto& id : ids) {
        auto bytes = co_await repository_detail::build_access::get(*this, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = decode_attestation(*bytes);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto record = repository_attestation{id, std::move(*value)};
        if (attestation_matches(record, filter)) {
            records.push_back(std::move(record));
        }
    }
    std::ranges::sort(records, [](const auto& left, const auto& right) {
        if (left.value.at != right.value.at) {
            return left.value.at > right.value.at;
        }
        return to_string(left.id) < to_string(right.id);
    });
    co_return make_page(std::move(records), filter.page, [](const repository_attestation& record) {
        return to_string(record.id);
    });
}

runtime::task<record_page<repository_attestation>>
repository::attestations(digest subject, record_page_options page) {
    co_return co_await attestations({.subject = subject, .page = std::move(page)});
}

runtime::task<record_page<build_trace>> repository::traces(record_page_options page) {
    auto valid = validate_page(page);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto cache = build_detail::disk_cache{*this};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto records = std::vector<build_trace>{};
    records.reserve(cache.traces().size());
    for (auto iterator = cache.traces().rbegin(); iterator != cache.traces().rend(); ++iterator) {
        auto bytes = co_await repository_detail::build_access::get(*this, *iterator);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_build_trace(*bytes);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        value->id = *iterator;
        records.push_back(std::move(*value));
    }
    co_return make_page(
        std::move(records), page, [](const build_trace& record) { return to_string(record.id); });
}

runtime::task<record_page<sbom_report>> repository::sboms(result_id result,
                                                          record_page_options page) {
    auto valid = validate_page(page);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto stored = co_await build::result(*this, result);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto statements = std::vector<repository_attestation>{};
    statements.reserve(stored->sboms.size());
    for (const auto& id : stored->sboms) {
        auto statement_page = co_await attestations({.id = id});
        if (!statement_page) {
            co_return std::unexpected(statement_page.error());
        }
        if (statement_page->records.empty()) {
            co_return std::unexpected(page_error(
                errc::not_found, "result SBOM attestation is not stored", to_string(id)));
        }
        statements.push_back(std::move(statement_page->records.front()));
    }
    auto records = std::vector<sbom_report>{};
    for (const auto& output : stored->outputs) {
        for (const auto format : {sbom_format::spdx, sbom_format::cyclonedx}) {
            const auto statement = std::ranges::find_if(statements, [&](const auto& record) {
                return sbom_matches(record, output, format);
            });
            if (statement == statements.end()) {
                continue;
            }
            auto report = co_await build::generate_sbom(*this, result, output.path, format);
            if (!report) {
                co_return std::unexpected(report.error());
            }
            records.push_back(std::move(*report));
        }
    }
    co_return make_page(std::move(records), page, sbom_cursor);
}

} // namespace liquid
