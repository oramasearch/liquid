#include "liquid/forge.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <boost/http.hpp>
#include <nlohmann/json.hpp>

#include "net/http_client.hpp"

namespace liquid {
namespace {

using nlohmann::json;

auto forge_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto json_request(const net_detail::parsed_url& url,
                  std::string_view token,
                  boost::http::method method,
                  std::string target,
                  const json& input,
                  std::string context) -> runtime::task<json> {
    auto encoded = std::string{};
    if (method != boost::http::method::get) {
        encoded = input.dump();
    }
    const auto* first = reinterpret_cast<const std::byte*>(encoded.data());
    auto response = co_await net_detail::perform_request(net_detail::http_request{
        .method = method == boost::http::method::get ? net_detail::http_method::get
                                                     : net_detail::http_method::post,
        .url = url,
        .target = std::move(target),
        .bearer = std::string{token},
        .content_type = "application/json",
        .body = {first, first + encoded.size()},
    });
    if (!response) {
        co_return std::unexpected(
            forge_error(response.error().code, response.error().message, std::move(context)));
    }
    try {
        const auto text = std::string_view{reinterpret_cast<const char*>(response->body.data()),
                                           response->body.size()};
        auto envelope = json::parse(text);
        if (!envelope.is_object() || !envelope.contains("ok") || !envelope.at("ok").is_boolean()) {
            co_return std::unexpected(
                forge_error(errc::corrupt, "invalid JSON response envelope", std::move(context)));
        }
        if (envelope.at("ok").get<bool>()) {
            if (!envelope.contains("value")) {
                co_return std::unexpected(
                    forge_error(errc::corrupt, "successful JSON envelope has no value", context));
            }
            co_return envelope.at("value");
        }
        const auto failure = envelope.value("error", json::object());
        const auto code_name = failure.value("code", "internal");
        auto code = errc::internal;
        if (code_name == "invalid_input") {
            code = errc::invalid_argument;
        } else if (code_name == "unauthorized" || code_name == "mfa_required") {
            code = errc::unauthorized;
        } else if (code_name == "not_found") {
            code = errc::not_found;
        } else if (code_name == "conflict") {
            code = errc::conflict;
        } else if (code_name == "store_unavailable") {
            code = errc::unavailable;
        } else if (code_name == "unsupported_client_version") {
            // The same error a sync path gives: `unsupported`, context unsupported_client_version.
            co_return std::unexpected(forge_error(errc::unsupported,
                                                  failure.value("message", "lake request failed"),
                                                  "unsupported_client_version"));
        }
        co_return std::unexpected(forge_error(code,
                                              failure.value("message", "lake request failed"),
                                              failure.value("path", std::string{})));
    } catch (const json::exception& exception) {
        co_return std::unexpected(forge_error(errc::corrupt, exception.what(), std::move(context)));
    }
}

auto state_name(forge_review_state state) -> std::string_view {
    switch (state) {
    case forge_review_state::open:
        return "open";
    case forge_review_state::approved:
        return "approved";
    case forge_review_state::closed:
        return "closed";
    }
    return "open";
}

auto parse_state(std::string_view value) -> result<forge_review_state> {
    if (value == "open") {
        return forge_review_state::open;
    }
    if (value == "approved") {
        return forge_review_state::approved;
    }
    if (value == "closed") {
        return forge_review_state::closed;
    }
    return std::unexpected(
        forge_error(errc::corrupt, "unknown forge review state", std::string{value}));
}

auto parse_number(std::string_view input, std::size_t offset, std::size_t count)
    -> std::optional<unsigned> {
    if (offset > input.size() || count > input.size() - offset) {
        return std::nullopt;
    }
    auto output = unsigned{};
    const auto field = input.substr(offset, count);
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) {
        return std::nullopt;
    }
    return output;
}

auto parse_time(std::string_view input, std::string_view field)
    -> result<std::chrono::sys_seconds> {
    const auto year = parse_number(input, 0, 4);
    const auto month = parse_number(input, 5, 2);
    const auto day = parse_number(input, 8, 2);
    const auto hour = parse_number(input, 11, 2);
    const auto minute = parse_number(input, 14, 2);
    const auto second = parse_number(input, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second || input.size() < 19 ||
        input[4] != '-' || input[7] != '-' || (input[10] != 'T' && input[10] != ' ') ||
        input[13] != ':' || input[16] != ':') {
        return std::unexpected(
            forge_error(errc::corrupt, "invalid forge timestamp", std::string{field}));
    }
    const auto date = std::chrono::year{static_cast<int>(*year)} / std::chrono::month{*month} /
                      std::chrono::day{*day};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) {
        return std::unexpected(
            forge_error(errc::corrupt, "invalid forge timestamp", std::string{field}));
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

auto parse_change(std::string_view value, std::string_view context) -> result<change_id> {
    auto parsed = parse_digest(value);
    if (!parsed) {
        auto failure = parsed.error();
        failure.code = errc::corrupt;
        failure.context = std::string{context};
        return std::unexpected(std::move(failure));
    }
    return change_id{*parsed};
}

auto parse_actor(const json& value) -> result<actor> {
    if (!value.is_object()) {
        return std::unexpected(forge_error(errc::corrupt, "invalid forge comment author"));
    }
    auto output = actor{};
    output.taap_id = value.value("taap_id", std::string{});
    output.algorithm = value.value("algorithm", std::string{});
    output.fingerprint = value.value("fingerprint", std::string{});
    output.key_fingerprint = value.value("key_fingerprint", std::string{});
    return output;
}

auto parse_review(const json& record) -> result<forge_review> {
    try {
        const auto& value = record.at("value");
        auto change = parse_change(value.at("change_id").get<std::string>(), "change_id");
        if (!change) {
            return std::unexpected(change.error());
        }
        auto state = parse_state(value.at("state").get<std::string>());
        if (!state) {
            return std::unexpected(state.error());
        }
        auto created = parse_time(record.at("created_at").get<std::string>(), "created_at");
        if (!created) {
            return std::unexpected(created.error());
        }
        auto updated = parse_time(record.at("updated_at").get<std::string>(), "updated_at");
        if (!updated) {
            return std::unexpected(updated.error());
        }
        return forge_review{.id = record.at("id").get<std::string>(),
                            .change = *change,
                            .state = *state,
                            .reviewers = value.value("reviewers", std::vector<std::string>{}),
                            .created = *created,
                            .updated = *updated};
    } catch (const json::exception& exception) {
        return std::unexpected(forge_error(errc::corrupt, exception.what(), "forge review"));
    }
}

auto parse_comment(const json& record) -> result<forge_comment> {
    try {
        const auto& value = record.at("value");
        const auto& anchor = value.at("anchor");
        auto change = parse_change(anchor.at("change").get<std::string>(), "anchor/change");
        if (!change) {
            return std::unexpected(change.error());
        }
        auto created = parse_time(record.at("created_at").get<std::string>(), "created_at");
        if (!created) {
            return std::unexpected(created.error());
        }
        auto output = forge_comment{.id = record.at("id").get<std::string>(),
                                    .review = value.at("review_id").get<std::string>(),
                                    .body = value.at("body").get<std::string>(),
                                    .anchor = {.change = *change,
                                               .path = anchor.at("path").get<std::string>(),
                                               .line = anchor.at("line").get<std::uint64_t>(),
                                               .column = anchor.at("column").get<std::uint64_t>()},
                                    .created = *created};
        if (value.contains("author") && !value.at("author").is_null()) {
            auto author = parse_actor(value.at("author"));
            if (!author) {
                return std::unexpected(author.error());
            }
            output.author = std::move(*author);
        }
        if (anchor.contains("disposition")) {
            const auto disposition = anchor.at("disposition").get<std::string>();
            output.mapped = mapped_anchor{.change = output.anchor.change,
                                          .path = output.anchor.path,
                                          .line = output.anchor.line,
                                          .column = output.anchor.column,
                                          .moved = disposition == "shifted",
                                          .approximate = disposition == "approximate",
                                          .deleted = disposition == "deleted"};
        }
        return output;
    } catch (const json::exception& exception) {
        return std::unexpected(forge_error(errc::corrupt, exception.what(), "forge comment"));
    }
}

auto parse_reviews(const json& value) -> result<std::vector<forge_review>> {
    try {
        auto output = std::vector<forge_review>{};
        for (const auto& item : value.at("items")) {
            auto parsed = parse_review(item);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            output.push_back(std::move(*parsed));
        }
        return output;
    } catch (const json::exception& exception) {
        return std::unexpected(forge_error(errc::corrupt, exception.what(), "forge reviews"));
    }
}

auto parse_comments(const json& value) -> result<std::vector<forge_comment>> {
    try {
        auto output = std::vector<forge_comment>{};
        for (const auto& item : value.at("items")) {
            auto parsed = parse_comment(item);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            output.push_back(std::move(*parsed));
        }
        return output;
    } catch (const json::exception& exception) {
        return std::unexpected(forge_error(errc::corrupt, exception.what(), "forge comments"));
    }
}

auto query_escape(std::string_view input) -> std::string {
    constexpr auto hex = std::string_view{"0123456789ABCDEF"};
    auto output = std::string{};
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output.push_back(character);
            continue;
        }
        output.push_back('%');
        output.push_back(hex[byte >> 4]);
        output.push_back(hex[byte & 0x0f]);
    }
    return output;
}

struct endpoint {
    net_detail::parsed_url url;
    std::string repository_url;
    std::string api_prefix;
    std::string repository_id;
    forge_client_options options;
};

auto make_endpoint(std::string repository_url, forge_client_options options) -> result<endpoint> {
    auto parsed = net_detail::parse_url(repository_url, "forge URL");
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto marker = std::string_view{"/v1/repositories/"};
    const auto marker_position = parsed->path.find(marker);
    auto prefix = std::string{"/v1"};
    auto id = std::string{};
    if (marker_position != std::string::npos) {
        prefix = parsed->path.substr(0, marker_position) + "/v1";
        id = parsed->path.substr(marker_position + marker.size());
    } else {
        const auto slash = parsed->path.find_last_of('/');
        id = parsed->path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    if (id.empty() || id.find('/') != std::string::npos) {
        return std::unexpected(forge_error(
            errc::invalid_argument, "forge URL must identify one repository", "forge URL"));
    }
    return endpoint{std::move(*parsed),
                    std::move(repository_url),
                    std::move(prefix),
                    std::move(id),
                    std::move(options)};
}

auto call(const endpoint& endpoint,
          std::string_view capability,
          boost::http::method method,
          json input = json::object()) -> runtime::task<json> {
    input["repository_id"] = endpoint.repository_id;
    auto target = endpoint.api_prefix + '/' + std::string{capability};
    if (method == boost::http::method::get) {
        target += "?repository_id=" + query_escape(endpoint.repository_id) + "&limit=1000";
    }
    co_return co_await json_request(endpoint.url,
                                    endpoint.options.bearer_token,
                                    method,
                                    std::move(target),
                                    input,
                                    std::string{capability});
}

auto load_reviews(const endpoint& endpoint) -> runtime::task<std::vector<forge_review>> {
    auto value = co_await call(endpoint, "list_reviews", boost::http::method::get);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_reviews(*value);
}

auto load_comments(const endpoint& endpoint, std::string_view review)
    -> runtime::task<std::vector<forge_comment>> {
    auto value = co_await call(endpoint, "list_review_comments", boost::http::method::get);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto parsed = parse_comments(*value);
    if (!parsed) {
        co_return std::unexpected(parsed.error());
    }
    std::erase_if(*parsed,
                  [review](const forge_comment& comment) { return comment.review != review; });
    co_return std::move(*parsed);
}

auto find_review(std::span<const forge_review> reviews, std::string_view id)
    -> result<forge_review> {
    const auto found = std::ranges::find(reviews, id, &forge_review::id);
    if (found == reviews.end()) {
        return std::unexpected(
            forge_error(errc::not_found, "review was not found", std::string{id}));
    }
    return *found;
}

auto same_change(const change_id& left, const change_id& right) -> bool {
    return to_string(left.value) == to_string(right.value);
}

auto watch_path() -> std::filesystem::path {
    auto random = std::random_device{};
    return std::filesystem::temp_directory_path() /
           ("liquid-forge-watch-" + std::to_string(random()) + '-' + std::to_string(random()));
}

} // namespace

struct forge_client::impl {
    endpoint remote;
};

struct forge_watch::impl {
    endpoint remote;
    std::string review_id;
    forge_review last_review;
    std::set<std::string> comment_ids;
    std::deque<forge_event> pending;
    std::filesystem::path mirror_path;
    repository mirror;
    change_id last_head;
    bool has_head{};

    ~impl() {
        if (mirror_path.empty()) {
            return;
        }
        auto ignored = std::error_code{};
        std::filesystem::remove_all(mirror_path, ignored);
    }
};

forge_client::forge_client(std::string lake_repository_url, forge_client_options options) {
    auto remote = make_endpoint(std::move(lake_repository_url), std::move(options));
    if (!remote) {
        throw std::invalid_argument(remote.error().message);
    }
    impl_ = std::make_unique<impl>(impl{std::move(*remote)});
}

forge_client::~forge_client() = default;
forge_client::forge_client(forge_client&&) noexcept = default;
forge_client& forge_client::operator=(forge_client&&) noexcept = default;

runtime::task<std::vector<forge_review>>
forge_client::list_reviews(std::optional<forge_review_state> state) {
    auto reviews = co_await load_reviews(impl_->remote);
    if (!reviews) {
        co_return std::unexpected(reviews.error());
    }
    if (state) {
        std::erase_if(*reviews,
                      [state](const forge_review& review) { return review.state != *state; });
    }
    co_return std::move(*reviews);
}

runtime::task<forge_review> forge_client::get_review(std::string id) {
    auto reviews = co_await load_reviews(impl_->remote);
    if (!reviews) {
        co_return std::unexpected(reviews.error());
    }
    co_return find_review(*reviews, id);
}

runtime::task<forge_review> forge_client::create_review(change_id change,
                                                        std::vector<std::string> reviewers) {
    auto value = co_await call(
        impl_->remote,
        "create_review",
        boost::http::method::post,
        {{"change_id", to_string(change.value)}, {"reviewers", std::move(reviewers)}});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_review(*value);
}

runtime::task<forge_review> forge_client::set_review_state(std::string id,
                                                           forge_review_state state) {
    auto value = co_await call(impl_->remote,
                               "update_review",
                               boost::http::method::post,
                               {{"review_id", std::move(id)}, {"state", state_name(state)}});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_review(*value);
}

runtime::task<std::vector<forge_comment>> forge_client::list_comments(std::string review) {
    co_return co_await load_comments(impl_->remote, review);
}

runtime::task<forge_comment>
forge_client::create_comment(std::string review, std::string body, text_anchor anchor) {
    auto value = co_await call(impl_->remote,
                               "create_review_comment",
                               boost::http::method::post,
                               {{"review_id", std::move(review)},
                                {"body", std::move(body)},
                                {"anchor",
                                 {{"change", to_string(anchor.change.value)},
                                  {"path", std::move(anchor.path)},
                                  {"line", anchor.line},
                                  {"column", anchor.column}}}});
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return parse_comment(*value);
}

runtime::task<std::vector<forge_comment>>
forge_client::advance_anchors(repository& source, std::string review, change_id target) {
    auto current = co_await get_review(review);
    if (!current) {
        co_return std::unexpected(current.error());
    }
    auto comments = co_await load_comments(impl_->remote, review);
    if (!comments) {
        co_return std::unexpected(comments.error());
    }
    auto differences = co_await source.diff(current->change, target, {.text = true});
    if (!differences) {
        co_return std::unexpected(differences.error());
    }
    auto paths = std::set<std::string>{};
    for (const auto& comment : *comments) {
        paths.insert(comment.anchor.path);
    }
    for (const auto& path : paths) {
        const auto difference = std::ranges::find_if(*differences, [&path](const auto& entry) {
            return entry.path == path || entry.old_path == path;
        });
        auto hunks = json::array();
        if (difference != differences->end() && difference->line_hunks) {
            for (const auto& hunk : *difference->line_hunks) {
                hunks.push_back({{"old_line", hunk.old_line},
                                 {"old_count", hunk.old_count},
                                 {"new_line", hunk.new_line},
                                 {"new_count", hunk.new_count}});
            }
        }
        auto mapped = co_await call(impl_->remote,
                                    "map_review_anchors",
                                    boost::http::method::post,
                                    {{"source_change", to_string(current->change.value)},
                                     {"target_change", to_string(target.value)},
                                     {"path", path},
                                     {"hunks", std::move(hunks)}});
        if (!mapped) {
            co_return std::unexpected(mapped.error());
        }
    }
    co_return co_await load_comments(impl_->remote, review);
}

runtime::task<std::vector<forge_branch_protection>> forge_client::branch_protections() {
    auto value = co_await call(impl_->remote, "list_branch_protections", boost::http::method::get);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    try {
        auto output = std::vector<forge_branch_protection>{};
        for (const auto& item : value->at("items")) {
            const auto& rule = item.at("value");
            output.push_back(
                {.pattern = rule.value("ref", std::string{}),
                 .required_review = rule.value("required_review", false),
                 .required_verification = rule.value("required_verification", false),
                 .fast_forward_only = rule.value("fast_forward_only", false),
                 .allowed_pushers = rule.value("allowed_pushers", std::vector<std::string>{})});
        }
        co_return output;
    } catch (const json::exception& exception) {
        co_return std::unexpected(
            forge_error(errc::corrupt, exception.what(), "branch protections"));
    }
}

runtime::task<forge_watch> forge_client::watch(std::string review) {
    auto reviews = co_await load_reviews(impl_->remote);
    if (!reviews) {
        co_return std::unexpected(reviews.error());
    }
    auto selected = find_review(*reviews, review);
    if (!selected) {
        co_return std::unexpected(selected.error());
    }
    auto comments = co_await load_comments(impl_->remote, review);
    if (!comments) {
        co_return std::unexpected(comments.error());
    }
    auto implementation = std::make_unique<forge_watch::impl>();
    implementation->remote = impl_->remote;
    implementation->review_id = std::move(review);
    implementation->last_review = std::move(*selected);
    for (const auto& comment : *comments) {
        implementation->comment_ids.insert(comment.id);
    }
    implementation->mirror_path = watch_path();
    auto mirror = co_await repository::create(implementation->mirror_path);
    if (!mirror) {
        co_return std::unexpected(mirror.error());
    }
    implementation->mirror = std::move(*mirror);
    auto options = sync_endpoint_options{};
    options.bearer_token = implementation->remote.options.bearer_token;
    auto sync = sync_endpoint{implementation->remote.repository_url, std::move(options)};
    auto pulled = co_await sync.pull(implementation->mirror);
    if (!pulled && pulled.error().code != errc::not_found) {
        co_return std::unexpected(pulled.error());
    }
    if (pulled) {
        auto head = co_await implementation->mirror.head();
        if (head) {
            implementation->last_head = *head;
            implementation->has_head = true;
        } else if (head.error().code != errc::not_found) {
            co_return std::unexpected(head.error());
        }
    }
    co_return forge_watch{std::move(implementation)};
}

forge_watch::forge_watch() noexcept = default;
forge_watch::forge_watch(std::unique_ptr<impl> implementation) noexcept
    : impl_(std::move(implementation)) {}
forge_watch::~forge_watch() = default;
forge_watch::forge_watch(forge_watch&&) noexcept = default;
forge_watch& forge_watch::operator=(forge_watch&&) noexcept = default;

runtime::task<forge_event> forge_watch::next() {
    if (!impl_) {
        co_return std::unexpected(forge_error(errc::invalid_argument, "forge watch is empty"));
    }
    for (;;) {
        if (!impl_->pending.empty()) {
            auto event = std::move(impl_->pending.front());
            impl_->pending.pop_front();
            co_return event;
        }
        auto [delay_error] = co_await runtime::delay(impl_->remote.options.poll_interval);
        if (delay_error) {
            co_return std::unexpected(
                forge_error(errc::internal, delay_error.message(), "forge watch"));
        }
        auto options = sync_endpoint_options{};
        options.bearer_token = impl_->remote.options.bearer_token;
        auto sync = sync_endpoint{impl_->remote.repository_url, std::move(options)};
        auto pulled = co_await sync.pull(impl_->mirror);
        if (!pulled) {
            co_return std::unexpected(pulled.error());
        }
        auto head = co_await impl_->mirror.head();
        if (!head && head.error().code != errc::not_found) {
            co_return std::unexpected(head.error());
        }
        if (head && (!impl_->has_head || !same_change(*head, impl_->last_head))) {
            impl_->last_head = *head;
            impl_->has_head = true;
            impl_->pending.push_back({.kind = forge_event_kind::head_moved, .head = *head});
        }
        auto reviews = co_await load_reviews(impl_->remote);
        if (!reviews) {
            co_return std::unexpected(reviews.error());
        }
        auto selected = find_review(*reviews, impl_->review_id);
        if (!selected) {
            co_return std::unexpected(selected.error());
        }
        if (selected->updated != impl_->last_review.updated) {
            impl_->last_review = *selected;
            impl_->pending.push_back(
                {.kind = forge_event_kind::review_updated, .review = *selected});
        }
        auto comments = co_await load_comments(impl_->remote, impl_->review_id);
        if (!comments) {
            co_return std::unexpected(comments.error());
        }
        for (const auto& comment : *comments) {
            if (impl_->comment_ids.insert(comment.id).second) {
                impl_->pending.push_back(
                    {.kind = forge_event_kind::comment_added, .comment = comment});
            }
        }
    }
}

} // namespace liquid
