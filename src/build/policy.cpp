#include "build/policy.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "build/wrap.hpp"
#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid::build_detail {
namespace {

namespace cbor = encoding::cbor;

struct input_subject {
    std::string path;
    digest subject;
    bool toolchain{};
};

auto invalid(std::string message, std::string context) -> error {
    return {errc::invalid_argument, std::move(message), std::move(context)};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> values) -> cbor::value {
    return cbor::value{cbor::value::map{values}};
}

auto encode_policy(const policy_config& policy) -> result<std::vector<std::byte>> {
    auto approved = cbor::value::array{};
    for (const auto& builder : policy.approved_builders) {
        approved.emplace_back(builder);
    }
    auto rules = cbor::value::array{};
    for (const auto& rule : policy.rules) {
        auto paths = cbor::value::array{};
        for (const auto& path : rule.paths) {
            paths.emplace_back(path);
        }
        auto requirements = cbor::value::array{};
        for (const auto& requirement : rule.require) {
            auto authorities = cbor::value::array{};
            for (const auto& authority : requirement.from) {
                authorities.emplace_back(authority);
            }
            requirements.push_back(map({{"profile", requirement.profile},
                                        {"from", cbor::value{std::move(authorities)}},
                                        {"min_count", std::uint64_t{requirement.min_count}}}));
        }
        const auto scope = rule.scope == policy_scope::paths       ? "paths"
                           : rule.scope == policy_scope::toolchain ? "toolchain"
                                                                   : "all inputs";
        const auto on_missing = rule.on_missing == policy_on_missing::refuse ? "refuse" : "warn";
        rules.push_back(map({{"scope", scope},
                             {"paths", cbor::value{std::move(paths)}},
                             {"require", cbor::value{std::move(requirements)}},
                             {"on_missing", on_missing}}));
    }
    return cbor::encode(map({{"approved_builders", cbor::value{std::move(approved)}},
                             {"rules", cbor::value{std::move(rules)}}}));
}

auto glob_matches(std::string_view pattern, std::string_view path) -> bool {
    auto previous = std::vector<bool>(path.size() + 1);
    previous[0] = true;
    for (std::size_t pattern_index = 0; pattern_index < pattern.size();) {
        auto current = std::vector<bool>(path.size() + 1);
        if (pattern[pattern_index] == '*') {
            const auto recursive =
                pattern_index + 1 < pattern.size() && pattern[pattern_index + 1] == '*';
            pattern_index += recursive ? 2 : 1;
            current[0] = previous[0];
            for (std::size_t path_index = 1; path_index <= path.size(); ++path_index) {
                const auto consumable = recursive || path[path_index - 1] != '/';
                current[path_index] =
                    previous[path_index] || (consumable && current[path_index - 1]);
            }
        } else {
            for (std::size_t path_index = 1; path_index <= path.size(); ++path_index) {
                const auto character_matches =
                    pattern[pattern_index] == '?' || pattern[pattern_index] == path[path_index - 1];
                const auto allowed = pattern[pattern_index] != '?' || path[path_index - 1] != '/';
                current[path_index] = previous[path_index - 1] && character_matches && allowed;
            }
            ++pattern_index;
        }
        previous = std::move(current);
    }
    return previous[path.size()];
}

auto applies(const policy_rule& rule, const input_subject& input) -> bool {
    if (rule.scope == policy_scope::toolchain) {
        return input.toolchain;
    }
    if (rule.scope == policy_scope::all_inputs) {
        return true;
    }
    return !input.toolchain && std::ranges::any_of(rule.paths, [&](const auto& pattern) {
        return glob_matches(pattern, input.path);
    });
}

runtime::task<void> collect_tree(repository& source,
                                 digest root,
                                 std::string prefix,
                                 bool toolchain,
                                 std::vector<input_subject>& output,
                                 std::set<std::string>& visited) {
    if (!visited.insert(to_string(root)).second) {
        co_return result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, root);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto tree = objects::decode_tree(*bytes);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    for (const auto& entry : tree->entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            auto nested =
                co_await collect_tree(source, entry.ref, path, toolchain, output, visited);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
            continue;
        }
        if (entry.kind != node_kind::blob) {
            co_return std::unexpected(
                error{errc::unsupported, "policy input tree contains a non-blob entry", path});
        }
        output.push_back({toolchain ? "toolchain:" + path : path, entry.ref, toolchain});
    }
    co_return result<void>{};
}

auto allowed_authorities(const policy_config& policy, const policy_requirement& requirement)
    -> std::set<std::string> {
    auto output = std::set<std::string>{};
    for (const auto& authority : requirement.from) {
        if (authority == "approved-builders") {
            output.insert(policy.approved_builders.begin(), policy.approved_builders.end());
        } else {
            output.insert(authority);
        }
    }
    return output;
}

auto reproducibility_agrees(const attestation& value) -> bool {
    if (value.profile != "reproducibility/1") {
        return true;
    }
    const auto agreement =
        std::ranges::find(value.attributes, "agreement", &attestation_attribute::name);
    return agreement != value.attributes.end() && std::holds_alternative<bool>(agreement->value) &&
           std::get<bool>(agreement->value);
}

runtime::task<policy_requirement_result>
evaluate_requirement(repository& source,
                     digest subject,
                     const policy_config& policy,
                     const policy_requirement& requirement) {
    auto output = policy_requirement_result{.requirement = requirement};
    const auto authorities = allowed_authorities(policy, requirement);
    auto identities = std::set<std::string>{};
    auto last_rejection = std::string{};
    const auto records = repository_detail::build_access::attestations(source, subject);
    for (const auto& id : records) {
        auto bytes = co_await repository_detail::build_access::get(source, id);
        if (!bytes) {
            last_rejection = bytes.error().message;
            continue;
        }
        auto value = objects::decode_attestation(*bytes);
        if (!value) {
            last_rejection = value.error().message;
            continue;
        }
        if (value->profile != requirement.profile || !authorities.contains(value->issuer.taap_id)) {
            continue;
        }
        auto verified = verify_attestation(*value);
        if (!verified) {
            last_rejection = verified.error().message;
            continue;
        }
        if (!verified->verified) {
            last_rejection = verified->reason;
            continue;
        }
        if (!reproducibility_agrees(*value)) {
            last_rejection = "reproducibility attestation records disagreement";
            continue;
        }
        identities.insert(value->issuer.taap_id);
    }
    output.identities.assign(identities.begin(), identities.end());
    output.count = static_cast<std::uint32_t>(
        std::min<std::size_t>(identities.size(), std::numeric_limits<std::uint32_t>::max()));
    output.satisfied = output.count >= requirement.min_count;
    if (!output.satisfied) {
        output.reason = "requires " + std::to_string(requirement.min_count) + " verified " +
                        requirement.profile + " attestation(s); found " +
                        std::to_string(output.count);
        if (!last_rejection.empty()) {
            output.reason += "; last rejection: " + last_rejection;
        }
    }
    co_return output;
}

auto result_message(const policy_input_result& input) -> std::string {
    const auto missing = std::ranges::find_if(
        input.requirements, [](const auto& requirement) { return !requirement.satisfied; });
    const auto reason = missing == input.requirements.end()
                            ? std::string{"policy requirement is not satisfied"}
                            : missing->reason;
    return "input '" + input.input + "' fails config.policy.rules[" + std::to_string(input.rule) +
           "]: " + reason;
}

void add_unavailable(policy_report& report, bool toolchain, digest subject, std::string reason) {
    for (std::size_t index = 0; index < report.config.rules.size(); ++index) {
        const auto& rule = report.config.rules[index];
        if ((toolchain && rule.scope == policy_scope::paths) ||
            (!toolchain && rule.scope == policy_scope::toolchain)) {
            continue;
        }
        auto input = policy_input_result{.input = toolchain ? "toolchain" : "inputs",
                                         .subject = subject,
                                         .toolchain = toolchain,
                                         .rule = index,
                                         .on_missing = rule.on_missing};
        for (const auto& requirement : rule.require) {
            input.requirements.push_back(
                {.requirement = requirement, .reason = "cannot evaluate input: " + reason});
        }
        if (rule.on_missing == policy_on_missing::refuse) {
            report.allowed = false;
        } else {
            report.warnings.push_back(result_message(input));
        }
        report.inputs.push_back(std::move(input));
    }
}

runtime::task<action_input> action_for(repository& source, result_id target) {
    auto bytes = co_await repository_detail::build_access::get(source, target.value);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto value = objects::decode_result(*bytes);
    if (value) {
        bytes = co_await repository_detail::build_access::get(source, value->action);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
    }
    auto action = objects::decode_action(*bytes);
    if (!action) {
        co_return std::unexpected(error{errc::invalid_argument,
                                        "policy target is not an action or result",
                                        to_string(target.value)});
    }
    co_return action->input;
}

runtime::task<repository> open_policy_repository(std::filesystem::path working_tree) {
    if (std::filesystem::exists(working_tree / "LIQUID_REPO")) {
        co_return co_await repository::open(std::move(working_tree));
    }
    auto cache = build::wrap_cache_directory(working_tree);
    if (!cache) {
        co_return std::unexpected(cache.error());
    }
    co_return co_await repository::open(*cache / "repository");
}

} // namespace

result<void> validate_policy(const policy_config& policy) {
    auto approved = std::set<std::string>{};
    for (const auto& builder : policy.approved_builders) {
        if (builder.empty() || builder == "approved-builders" || !approved.insert(builder).second) {
            return std::unexpected(
                invalid("approved builders must be non-empty, unique identities", "config.policy"));
        }
    }
    for (std::size_t index = 0; index < policy.rules.size(); ++index) {
        const auto& rule = policy.rules[index];
        const auto context = "config.policy.rules[" + std::to_string(index) + "]";
        if ((rule.scope == policy_scope::paths) != !rule.paths.empty()) {
            return std::unexpected(
                invalid("only a paths scope has a non-empty paths list", context));
        }
        if (rule.require.empty()) {
            return std::unexpected(invalid("policy rule has no requirements", context));
        }
        for (const auto& requirement : rule.require) {
            if (!attestation_profile_registry::global().find(requirement.profile)) {
                return std::unexpected(
                    invalid("unknown attestation profile: " + requirement.profile, context));
            }
            if (requirement.from.empty() || requirement.min_count == 0) {
                return std::unexpected(invalid(
                    "policy requirement needs authorities and a positive min_count", context));
            }
            const auto references_approved =
                std::ranges::contains(requirement.from, "approved-builders");
            if (references_approved && policy.approved_builders.empty()) {
                return std::unexpected(
                    invalid("approved-builders is referenced but its list is empty", context));
            }
            const auto authorities = allowed_authorities(policy, requirement);
            if (requirement.min_count > authorities.size()) {
                return std::unexpected(
                    invalid("min_count exceeds the number of distinct authorities", context));
            }
        }
    }
    return {};
}

result<digest> policy_digest(const policy_config& policy) {
    auto encoded = encode_policy(policy);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return hash(algorithm::blake3, *encoded);
}

runtime::task<policy_report> enforce_policy(repository& source, const action_input& input) {
    auto configured = repository_detail::build_access::policy(source);
    if (!configured) {
        co_return std::unexpected(configured.error());
    }
    auto digest = policy_digest(*configured);
    if (!digest) {
        co_return std::unexpected(digest.error());
    }
    auto report = policy_report{.config = *configured, .policy = *digest};
    const auto needs_sources = std::ranges::any_of(
        configured->rules, [](const auto& rule) { return rule.scope != policy_scope::toolchain; });
    const auto needs_toolchain = std::ranges::any_of(
        configured->rules, [](const auto& rule) { return rule.scope != policy_scope::paths; });
    auto subjects = std::vector<input_subject>{};
    if (needs_sources) {
        auto visited = std::set<std::string>{};
        auto collected = co_await collect_tree(source, input.inputs, {}, false, subjects, visited);
        if (!collected) {
            add_unavailable(report, false, input.inputs, collected.error().message);
        }
    }
    if (needs_toolchain) {
        auto visited = std::set<std::string>{};
        auto collected =
            co_await collect_tree(source, input.toolchain, {}, true, subjects, visited);
        if (!collected) {
            add_unavailable(report, true, input.toolchain, collected.error().message);
        }
    }
    for (const auto& subject : subjects) {
        for (std::size_t index = 0; index < configured->rules.size(); ++index) {
            const auto& rule = configured->rules[index];
            if (!applies(rule, subject)) {
                continue;
            }
            auto result = policy_input_result{.input = subject.path,
                                              .subject = subject.subject,
                                              .toolchain = subject.toolchain,
                                              .rule = index,
                                              .on_missing = rule.on_missing,
                                              .satisfied = true};
            for (const auto& requirement : rule.require) {
                auto evaluated = co_await evaluate_requirement(
                    source, subject.subject, *configured, requirement);
                if (!evaluated) {
                    co_return std::unexpected(evaluated.error());
                }
                result.satisfied = result.satisfied && evaluated->satisfied;
                result.requirements.push_back(std::move(*evaluated));
            }
            if (!result.satisfied) {
                if (rule.on_missing == policy_on_missing::refuse) {
                    report.allowed = false;
                } else {
                    report.warnings.push_back(result_message(result));
                }
            }
            report.inputs.push_back(std::move(result));
        }
    }
    co_return report;
}

error policy_refusal(const policy_report& report) {
    const auto refused = std::ranges::find_if(report.inputs, [](const auto& input) {
        return !input.satisfied && input.on_missing == policy_on_missing::refuse;
    });
    if (refused == report.inputs.end()) {
        return {errc::unauthorized, "input policy refused the action", "config.policy"};
    }
    return {errc::unauthorized,
            result_message(*refused),
            "config.policy.rules[" + std::to_string(refused->rule) + "]"};
}

} // namespace liquid::build_detail

namespace liquid::build {

runtime::task<policy_report> check_policy(repository& source, const action_input& input) {
    co_return co_await build_detail::enforce_policy(source, input);
}

runtime::task<policy_report> check_policy(repository& source, result_id target) {
    auto input = co_await build_detail::action_for(source, target);
    if (!input) {
        co_return std::unexpected(input.error());
    }
    co_return co_await check_policy(source, *input);
}

runtime::task<policy_report> check_policy(std::filesystem::path working_tree, std::string target) {
    auto source = co_await build_detail::open_policy_repository(working_tree);
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto parsed = parse_digest(target);
    if (parsed) {
        co_return co_await check_policy(*source, result_id{*parsed});
    }
    auto cache = build_detail::disk_cache{*source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto found = co_await cache.find_output(target);
    if (!found) {
        co_return std::unexpected(found.error());
    }
    if (!*found) {
        co_return std::unexpected(
            error{errc::not_found, "policy target is not in the action cache", target});
    }
    co_return co_await check_policy(*source, **found);
}

liquid::result<policy_report> explain_policy(repository& source) {
    auto configured = repository_detail::build_access::policy(source);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    auto digest = build_detail::policy_digest(*configured);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return policy_report{.config = *configured, .policy = *digest};
}

runtime::task<policy_report> explain_policy(std::filesystem::path working_tree) {
    auto source = co_await build_detail::open_policy_repository(std::move(working_tree));
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return explain_policy(*source);
}

} // namespace liquid::build
