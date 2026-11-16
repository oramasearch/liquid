// SPDX-License-Identifier: MIT

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "liquid/credential.hpp"
#include "liquid/forge.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

struct options {
    std::string verb;
    std::string lake;
    bool json{};
    std::optional<liquid::forge_review_state> state;
    std::vector<std::string> arguments;
};

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: review <list|show|open|comment|approve|close|protections|watch> "
                 "--lake <repository-url> [--json] ...");
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto state_name(liquid::forge_review_state state) -> std::string_view {
    switch (state) {
    case liquid::forge_review_state::open:
        return "open";
    case liquid::forge_review_state::approved:
        return "approved";
    case liquid::forge_review_state::closed:
        return "closed";
    }
    return "open";
}

auto parse_state(std::string_view value) -> std::optional<liquid::forge_review_state> {
    if (value == "open") {
        return liquid::forge_review_state::open;
    }
    if (value == "approved") {
        return liquid::forge_review_state::approved;
    }
    if (value == "closed") {
        return liquid::forge_review_state::closed;
    }
    return std::nullopt;
}

auto parse_options(int argc, char** argv) -> std::optional<options> {
    if (argc == 0) {
        return std::nullopt;
    }
    auto output = options{.verb = argv[0]};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--lake" && index + 1 < argc && output.lake.empty()) {
            output.lake = argv[++index];
            continue;
        }
        if (argument == "--json" && !output.json) {
            output.json = true;
            continue;
        }
        if (argument == "--state" && index + 1 < argc && !output.state) {
            output.state = parse_state(argv[++index]);
            if (!output.state) {
                return std::nullopt;
            }
            continue;
        }
        if (argument.starts_with("--")) {
            return std::nullopt;
        }
        output.arguments.emplace_back(argument);
    }
    if (output.lake.empty() || (output.state && output.verb != "list")) {
        return std::nullopt;
    }
    return output;
}

auto parse_change(std::string_view value) -> liquid::result<liquid::change_id> {
    auto parsed = liquid::parse_digest(value);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return liquid::change_id{*parsed};
}

auto parse_positive(std::string_view value) -> std::optional<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        output == 0) {
        return std::nullopt;
    }
    return output;
}

auto parse_anchor(std::string_view value) -> liquid::result<liquid::text_anchor> {
    const auto at = value.rfind('@');
    const auto last_colon =
        at == std::string_view::npos ? std::string_view::npos : value.rfind(':', at);
    if (at == std::string_view::npos || last_colon == std::string_view::npos || last_colon == 0 ||
        at + 1 == value.size()) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "anchor must be <path>:<line>[:<column>]@<change>",
                                             std::string{value}});
    }
    auto line_colon = last_colon;
    auto line = parse_positive(value.substr(last_colon + 1, at - last_colon - 1));
    auto column = std::uint64_t{1};
    const auto previous_colon = value.rfind(':', last_colon - 1);
    if (line && previous_colon != std::string_view::npos) {
        auto possible_line =
            parse_positive(value.substr(previous_colon + 1, last_colon - previous_colon - 1));
        if (possible_line) {
            column = *line;
            line = possible_line;
            line_colon = previous_colon;
        }
    }
    if (!line || line_colon == 0) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "anchor line and column must be positive integers",
                                             std::string{value}});
    }
    auto change = parse_change(value.substr(at + 1));
    if (!change) {
        return std::unexpected(change.error());
    }
    return liquid::text_anchor{.change = *change,
                               .path = std::string{value.substr(0, line_colon)},
                               .line = *line,
                               .column = column};
}

auto review_json(const liquid::forge_review& review) -> std::string {
    auto reviewers = std::string{"["};
    for (auto index = std::size_t{}; index < review.reviewers.size(); ++index) {
        reviewers += (index == 0 ? "" : ",") + json_string(review.reviewers[index]);
    }
    reviewers += ']';
    return std::format("{{\"id\":{},\"change\":{},\"state\":{},\"reviewers\":{},\"created\":{},"
                       "\"updated\":{}}}",
                       json_string(review.id),
                       json_string(liquid::to_string(review.change.value)),
                       json_string(state_name(review.state)),
                       reviewers,
                       review.created.time_since_epoch().count(),
                       review.updated.time_since_epoch().count());
}

auto comment_json(const liquid::forge_comment& comment) -> std::string {
    const auto disposition = !comment.mapped               ? std::string{"null"}
                             : comment.mapped->deleted     ? json_string("deleted")
                             : comment.mapped->approximate ? json_string("approximate")
                             : comment.mapped->moved       ? json_string("shifted")
                                                           : json_string("same");
    return std::format(
        "{{\"id\":{},\"review\":{},\"body\":{},\"anchor\":{{\"change\":{},"
        "\"path\":{},\"line\":{},\"column\":{},\"disposition\":{}}},\"created\":{}}}",
        json_string(comment.id),
        json_string(comment.review),
        json_string(comment.body),
        json_string(liquid::to_string(comment.anchor.change.value)),
        json_string(comment.anchor.path),
        comment.anchor.line,
        comment.anchor.column,
        disposition,
        comment.created.time_since_epoch().count());
}

void print_review(const liquid::forge_review& review, bool json) {
    if (json) {
        std::println("{}", review_json(review));
        return;
    }
    std::println("review={} change={} state={} reviewers={} created={} updated={}",
                 review.id,
                 liquid::to_string(review.change.value),
                 state_name(review.state),
                 review.reviewers.size(),
                 review.created.time_since_epoch().count(),
                 review.updated.time_since_epoch().count());
}

void print_comment(const liquid::forge_comment& comment, bool json) {
    if (json) {
        std::println("{}", comment_json(comment));
        return;
    }
    std::println("comment={} review={} anchor={}:{}:{}@{} body={}",
                 comment.id,
                 comment.review,
                 comment.anchor.path,
                 comment.anchor.line,
                 comment.anchor.column,
                 liquid::to_string(comment.anchor.change.value),
                 comment.body);
}

auto list(liquid::forge_client& client, const options& options) -> int {
    if (!options.arguments.empty()) {
        return usage();
    }
    auto reviews = liquid::runtime::block_on(client.list_reviews(options.state));
    if (!reviews) {
        return fail(reviews.error());
    }
    if (options.json) {
        std::print("[");
        for (auto index = std::size_t{}; index < reviews->size(); ++index) {
            std::print("{}{}", index == 0 ? "" : ",", review_json((*reviews)[index]));
        }
        std::println("]");
        return 0;
    }
    for (const auto& review : *reviews) {
        print_review(review, false);
    }
    return 0;
}

auto show(liquid::forge_client& client, const options& options) -> int {
    if (options.arguments.size() != 1) {
        return usage();
    }
    auto review = liquid::runtime::block_on(client.get_review(options.arguments[0]));
    if (!review) {
        return fail(review.error());
    }
    auto comments = liquid::runtime::block_on(client.list_comments(review->id));
    if (!comments) {
        return fail(comments.error());
    }
    if (options.json) {
        std::print("{{\"review\":{},\"comments\":[", review_json(*review));
        for (auto index = std::size_t{}; index < comments->size(); ++index) {
            std::print("{}{}", index == 0 ? "" : ",", comment_json((*comments)[index]));
        }
        std::println("]}}");
        return 0;
    }
    print_review(*review, false);
    for (const auto& comment : *comments) {
        print_comment(comment, false);
    }
    return 0;
}

auto open(liquid::forge_client& client, const options& options) -> int {
    if (options.arguments.size() != 1) {
        return usage();
    }
    auto change = parse_change(options.arguments[0]);
    if (!change) {
        return fail(change.error());
    }
    auto review = liquid::runtime::block_on(client.create_review(*change));
    if (!review) {
        return fail(review.error());
    }
    print_review(*review, options.json);
    return 0;
}

auto comment(liquid::forge_client& client, const options& options) -> int {
    if (options.arguments.size() != 3) {
        return usage();
    }
    auto anchor = parse_anchor(options.arguments[1]);
    if (!anchor) {
        return fail(anchor.error());
    }
    auto created = liquid::runtime::block_on(
        client.create_comment(options.arguments[0], options.arguments[2], std::move(*anchor)));
    if (!created) {
        return fail(created.error());
    }
    print_comment(*created, options.json);
    return 0;
}

auto update(liquid::forge_client& client, const options& options, liquid::forge_review_state state)
    -> int {
    if (options.arguments.size() != 1) {
        return usage();
    }
    auto review = liquid::runtime::block_on(client.set_review_state(options.arguments[0], state));
    if (!review) {
        return fail(review.error());
    }
    print_review(*review, options.json);
    return 0;
}

auto protections(liquid::forge_client& client, const options& options) -> int {
    if (!options.arguments.empty()) {
        return usage();
    }
    auto rules = liquid::runtime::block_on(client.branch_protections());
    if (!rules) {
        return fail(rules.error());
    }
    if (options.json) {
        std::print("[");
        for (auto index = std::size_t{}; index < rules->size(); ++index) {
            const auto& rule = (*rules)[index];
            std::print("{}{{\"pattern\":{},\"required_review\":{},"
                       "\"required_verification\":{},\"fast_forward_only\":{},"
                       "\"allowed_pushers\":[",
                       index == 0 ? "" : ",",
                       json_string(rule.pattern),
                       rule.required_review,
                       rule.required_verification,
                       rule.fast_forward_only);
            for (auto pusher = std::size_t{}; pusher < rule.allowed_pushers.size(); ++pusher) {
                std::print(
                    "{}{}", pusher == 0 ? "" : ",", json_string(rule.allowed_pushers[pusher]));
            }
            std::print("]}}");
        }
        std::println("]");
        return 0;
    }
    for (const auto& rule : *rules) {
        std::println("pattern={} required_review={} required_verification={} fast_forward_only={} "
                     "allowed_pushers={}",
                     rule.pattern,
                     rule.required_review,
                     rule.required_verification,
                     rule.fast_forward_only,
                     rule.allowed_pushers.size());
    }
    return 0;
}

auto watch(liquid::forge_client& client, const options& options) -> int {
    if (options.arguments.size() != 1) {
        return usage();
    }
    auto watched = liquid::runtime::block_on(client.watch(options.arguments[0]));
    if (!watched) {
        return fail(watched.error());
    }
    for (;;) {
        auto event = liquid::runtime::block_on(watched->next());
        if (!event) {
            return fail(event.error());
        }
        if (event->kind == liquid::forge_event_kind::review_updated) {
            if (options.json) {
                std::println("{{\"type\":\"review_updated\",\"review\":{}}}",
                             review_json(*event->review));
            } else {
                std::print("event=review_updated ");
                print_review(*event->review, false);
            }
        } else if (event->kind == liquid::forge_event_kind::comment_added) {
            if (options.json) {
                std::println("{{\"type\":\"comment_added\",\"comment\":{}}}",
                             comment_json(*event->comment));
            } else {
                std::print("event=comment_added ");
                print_comment(*event->comment, false);
            }
        } else if (options.json) {
            std::println("{{\"type\":\"head_moved\",\"change\":{}}}",
                         json_string(liquid::to_string(event->head->value)));
        } else {
            std::println("event=head_moved change={}", liquid::to_string(event->head->value));
        }
    }
}

} // namespace

auto review(int argc, char** argv) -> int {
    auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return usage();
    }
    auto client_options = liquid::forge_client_options{};
    if (const auto token = liquid::lake_token(); !token.empty()) {
        client_options.bearer_token = token;
    }
    try {
        auto client = liquid::forge_client{parsed->lake, std::move(client_options)};
        if (parsed->verb == "list") {
            return list(client, *parsed);
        }
        if (parsed->verb == "show") {
            return show(client, *parsed);
        }
        if (parsed->verb == "open") {
            return open(client, *parsed);
        }
        if (parsed->verb == "comment") {
            return comment(client, *parsed);
        }
        if (parsed->verb == "approve") {
            return update(client, *parsed, liquid::forge_review_state::approved);
        }
        if (parsed->verb == "close") {
            return update(client, *parsed, liquid::forge_review_state::closed);
        }
        if (parsed->verb == "protections") {
            return protections(client, *parsed);
        }
        if (parsed->verb == "watch") {
            return watch(client, *parsed);
        }
        return usage();
    } catch (const std::exception& exception) {
        return fail(liquid::error{liquid::errc::invalid_argument, exception.what(), "forge URL"});
    }
}

} // namespace liquid_cli
