#include "resolve/concretizer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <compare>
#include <format>
#include <limits>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace liquid::resolve_detail {
namespace {

struct clause {
    std::vector<int> literals;
    std::optional<std::size_t> origin;
};

struct candidate {
    std::size_t package{};
    std::string version;
    std::map<std::string, std::string> variants;
    std::string target;
    int variable{};
    std::size_t newer_rank{};
    std::size_t default_deviations{};
};

struct problem final : grounded_concretization {
    package_universe universe;
    std::vector<spec> roots;
    std::vector<candidate> candidates;
    std::vector<std::vector<std::size_t>> by_package;
    std::map<std::string, std::size_t> package_by_name;
    std::vector<clause> clauses;
    std::vector<unsat_constraint> origins;
    std::string requested_target;
    int variable_count{};
};

auto fail(errc code, std::string message, std::string context = "dependency resolution") -> error {
    return {code, std::move(message), std::move(context)};
}

auto digest_equal(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto contains(std::span<const digest> values, const digest& value) -> bool {
    return std::ranges::any_of(values, [&](const auto& item) { return digest_equal(item, value); });
}

auto natural_parts(std::string_view value)
    -> std::vector<std::variant<std::uint64_t, std::string>> {
    auto output = std::vector<std::variant<std::uint64_t, std::string>>{};
    for (std::size_t begin = 0; begin < value.size();) {
        const auto numeric = value[begin] >= '0' && value[begin] <= '9';
        auto end = begin + 1;
        while (end < value.size()) {
            const auto next_numeric = value[end] >= '0' && value[end] <= '9';
            if (next_numeric != numeric) {
                break;
            }
            ++end;
        }
        const auto part = value.substr(begin, end - begin);
        if (numeric) {
            auto number = std::uint64_t{};
            std::from_chars(part.data(), part.data() + part.size(), number);
            output.emplace_back(number);
        } else {
            output.emplace_back(std::string{part});
        }
        begin = end;
    }
    return output;
}

auto compare_versions(std::string_view left, std::string_view right) -> std::strong_ordering {
    const auto left_parts = natural_parts(left);
    const auto right_parts = natural_parts(right);
    const auto count = std::min(left_parts.size(), right_parts.size());
    for (std::size_t index = 0; index != count; ++index) {
        if (left_parts[index].index() != right_parts[index].index()) {
            return left_parts[index].index() < right_parts[index].index()
                       ? std::strong_ordering::greater
                       : std::strong_ordering::less;
        }
        if (left_parts[index] == right_parts[index]) {
            continue;
        }
        if (const auto* left_number = std::get_if<std::uint64_t>(&left_parts[index])) {
            const auto right_number = std::get<std::uint64_t>(right_parts[index]);
            return *left_number <=> right_number;
        }
        return std::get<std::string>(left_parts[index]) <=>
               std::get<std::string>(right_parts[index]);
    }
    return left_parts.size() <=> right_parts.size();
}

auto trim(std::string_view value) -> std::string_view {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

auto satisfies_one(std::string_view version, std::string_view constraint) -> bool {
    constraint = trim(constraint);
    if (constraint.empty() || constraint == "*") {
        return true;
    }
    if (constraint.starts_with('@')) {
        constraint.remove_prefix(1);
    }
    if (constraint.starts_with(">=")) {
        return compare_versions(version, constraint.substr(2)) != std::strong_ordering::less;
    }
    if (constraint.starts_with("<=")) {
        return compare_versions(version, constraint.substr(2)) != std::strong_ordering::greater;
    }
    if (constraint.starts_with('>')) {
        return compare_versions(version, constraint.substr(1)) == std::strong_ordering::greater;
    }
    if (constraint.starts_with('<')) {
        return compare_versions(version, constraint.substr(1)) == std::strong_ordering::less;
    }
    if (constraint.starts_with('=')) {
        constraint.remove_prefix(1);
    }
    if (const auto separator = constraint.find(':'); separator != std::string_view::npos) {
        const auto lower = constraint.substr(0, separator);
        const auto upper = constraint.substr(separator + 1);
        return (lower.empty() || compare_versions(version, lower) != std::strong_ordering::less) &&
               (upper.empty() || compare_versions(version, upper) != std::strong_ordering::greater);
    }
    return version == constraint ||
           (version.starts_with(constraint) && version.size() > constraint.size() &&
            version[constraint.size()] == '.');
}

auto satisfies(std::string_view version, const std::optional<std::string>& constraint) -> bool {
    if (!constraint || constraint->empty()) {
        return true;
    }
    for (std::size_t begin = 0; begin <= constraint->size();) {
        const auto end = constraint->find(',', begin);
        const auto part = std::string_view{*constraint}.substr(
            begin, end == std::string::npos ? constraint->size() - begin : end - begin);
        if (!satisfies_one(version, part)) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

auto ranges_overlap(const std::optional<std::string>& left, const std::optional<std::string>& right)
    -> bool {
    if (!left || left->empty() || !right || right->empty()) {
        return true;
    }
    const auto endpoint = [](std::string_view range) {
        if (range.starts_with('@')) {
            range.remove_prefix(1);
        }
        if (range.starts_with(">=") || range.starts_with("<=")) {
            range.remove_prefix(2);
        } else if (range.starts_with('>') || range.starts_with('<') || range.starts_with('=')) {
            range.remove_prefix(1);
        }
        const auto separator = range.find(':');
        if (separator == std::string_view::npos) {
            return std::string{range};
        }
        if (separator != 0) {
            return std::string{range.substr(0, separator)};
        }
        return std::string{range.substr(separator + 1)};
    };
    const auto left_endpoint = endpoint(*left);
    const auto right_endpoint = endpoint(*right);
    return satisfies(left_endpoint, right) || satisfies(right_endpoint, left);
}

auto target_family(std::string_view value) -> std::vector<std::string_view> {
    if (value == "zen4") {
        return {value, "zen3", "zen2", "x86_64_v4", "x86_64_v3", "x86_64_v2", "x86_64"};
    }
    if (value == "zen3") {
        return {value, "zen2", "x86_64_v3", "x86_64_v2", "x86_64"};
    }
    if (value == "zen2") {
        return {value, "x86_64_v3", "x86_64_v2", "x86_64"};
    }
    if (value == "x86_64_v4") {
        return {value, "x86_64_v3", "x86_64_v2", "x86_64"};
    }
    if (value == "x86_64_v3") {
        return {value, "x86_64_v2", "x86_64"};
    }
    if (value == "x86_64_v2") {
        return {value, "x86_64"};
    }
    if (value.starts_with("neoverse") || value.starts_with("apple-m")) {
        return {value, "aarch64"};
    }
    return {value};
}

auto target_matches(std::string_view candidate_target, std::string_view requested) -> bool {
    if (requested.empty() || candidate_target == "generic") {
        return true;
    }
    const auto family = target_family(requested);
    return std::ranges::find(family, candidate_target) != family.end();
}

auto matches_when(const candidate& value, const std::optional<std::string>& condition) -> bool {
    if (!condition || condition->empty()) {
        return true;
    }
    auto remaining = std::string_view{*condition};
    while (!remaining.empty()) {
        const auto separator = remaining.find_first_of(" ,");
        auto part = remaining.substr(0, separator);
        if (part.starts_with('@')) {
            if (!satisfies(value.version, std::string{part.substr(1)})) {
                return false;
            }
        } else if (part.starts_with('+') || part.starts_with('~')) {
            const auto found = value.variants.find(std::string{part.substr(1)});
            const auto expected = part.front() == '+' ? "true" : "false";
            if (found == value.variants.end() || found->second != expected) {
                return false;
            }
        } else if (const auto equals = part.find('='); equals != std::string_view::npos) {
            const auto name = std::string{part.substr(0, equals)};
            const auto selected = part.substr(equals + 1);
            if (name == "target") {
                if (value.target != selected) {
                    return false;
                }
            } else {
                const auto found = value.variants.find(name);
                if (found == value.variants.end() || found->second != selected) {
                    return false;
                }
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
        while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
            remaining.remove_prefix(1);
        }
    }
    return true;
}

auto matches_spec(const candidate& value, const package_record& package, const spec& request)
    -> bool {
    if (package.name != request.name || !satisfies(value.version, request.version)) {
        return false;
    }
    for (const auto& [name, selected] : request.variants) {
        const auto found = value.variants.find(name);
        if (found == value.variants.end() || found->second != selected) {
            return false;
        }
    }
    return !request.target || target_matches(value.target, *request.target);
}

auto origin_name(const constraint_origin& origin) -> std::string_view {
    switch (origin.kind) {
    case constraint_origin_kind::root:
        return "root";
    case constraint_origin_kind::dependency:
        return "dependency";
    case constraint_origin_kind::variant_default:
        return "variant_default";
    case constraint_origin_kind::conflict:
        return "conflict";
    case constraint_origin_kind::compiler:
        return "compiler";
    case constraint_origin_kind::target:
        return "target";
    }
    return "root";
}

auto escape_json(std::string_view value) -> std::string {
    auto output = std::string{};
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
            output += character;
            break;
        }
    }
    return output;
}

class cdcl_solver {
public:
    cdcl_solver(int variables, std::vector<std::vector<int>> clauses)
        : clauses_(std::move(clauses)), assignment_(static_cast<std::size_t>(variables + 1), -1),
          levels_(static_cast<std::size_t>(variables + 1)),
          reasons_(static_cast<std::size_t>(variables + 1), -1), variables_(variables) {}

    auto solve() -> bool {
        for (;;) {
            const auto conflict = propagate();
            if (conflict >= 0) {
                if (decision_literals_.empty()) {
                    return false;
                }
                auto learned = std::vector<int>{};
                learned.reserve(decision_literals_.size());
                for (const auto literal : decision_literals_) {
                    learned.push_back(-literal);
                }
                const auto backtrack_level = decision_literals_.size() - 1;
                backtrack(backtrack_level);
                clauses_.push_back(std::move(learned));
                continue;
            }
            const auto variable = choose_variable();
            if (variable == 0) {
                return true;
            }
            trail_limits_.push_back(trail_.size());
            decision_literals_.push_back(-variable);
            assign(-variable, -1);
        }
    }

    auto selected() const -> std::vector<int> {
        auto output = std::vector<int>{};
        for (int variable = 1; variable <= variables_; ++variable) {
            if (assignment_[static_cast<std::size_t>(variable)] == 1) {
                output.push_back(variable);
            }
        }
        return output;
    }

private:
    auto literal_value(int literal) const -> int {
        const auto value = assignment_[static_cast<std::size_t>(std::abs(literal))];
        if (value < 0) {
            return -1;
        }
        return literal > 0 ? value : 1 - value;
    }

    auto assign(int literal, int reason) -> bool {
        const auto variable = static_cast<std::size_t>(std::abs(literal));
        const auto value = literal > 0 ? std::int8_t{1} : std::int8_t{0};
        if (assignment_[variable] >= 0) {
            return assignment_[variable] == value;
        }
        assignment_[variable] = value;
        levels_[variable] = decision_literals_.size();
        reasons_[variable] = reason;
        trail_.push_back(literal);
        return true;
    }

    auto propagate() -> int {
        for (;;) {
            auto changed = false;
            for (std::size_t index = 0; index != clauses_.size(); ++index) {
                auto unknown = 0;
                auto last_unknown = 0;
                auto satisfied_clause = false;
                for (const auto literal : clauses_[index]) {
                    const auto value = literal_value(literal);
                    if (value == 1) {
                        satisfied_clause = true;
                        break;
                    }
                    if (value < 0) {
                        ++unknown;
                        last_unknown = literal;
                    }
                }
                if (satisfied_clause) {
                    continue;
                }
                if (unknown == 0) {
                    return static_cast<int>(index);
                }
                if (unknown == 1) {
                    if (!assign(last_unknown, static_cast<int>(index))) {
                        return static_cast<int>(index);
                    }
                    changed = true;
                }
            }
            if (!changed) {
                return -1;
            }
        }
    }

    auto choose_variable() const -> int {
        for (int variable = 1; variable <= variables_; ++variable) {
            if (assignment_[static_cast<std::size_t>(variable)] < 0) {
                return variable;
            }
        }
        return 0;
    }

    auto backtrack(std::size_t level) -> void {
        while (decision_literals_.size() > level) {
            const auto begin = trail_limits_.back();
            for (auto index = trail_.size(); index > begin; --index) {
                const auto variable = static_cast<std::size_t>(std::abs(trail_[index - 1]));
                assignment_[variable] = -1;
                levels_[variable] = 0;
                reasons_[variable] = -1;
            }
            trail_.resize(begin);
            trail_limits_.pop_back();
            decision_literals_.pop_back();
        }
    }

    std::vector<std::vector<int>> clauses_;
    std::vector<std::int8_t> assignment_;
    std::vector<std::size_t> levels_;
    std::vector<int> reasons_;
    std::vector<int> trail_;
    std::vector<std::size_t> trail_limits_;
    std::vector<int> decision_literals_;
    int variables_{};
};

auto add_origin(problem& value,
                constraint_origin_kind kind,
                std::string package,
                std::string detail) -> std::size_t {
    value.origins.push_back({detail, {kind, std::move(package), std::move(detail)}});
    return value.origins.size() - 1;
}

auto add_clause(problem& value,
                std::vector<int> literals,
                std::optional<std::size_t> origin = std::nullopt) -> void {
    auto unique = std::vector<int>{};
    for (const auto literal : literals) {
        if (std::ranges::find(unique, literal) == unique.end()) {
            unique.push_back(literal);
        }
    }
    value.clauses.push_back({std::move(unique), origin});
}

auto variant_combinations(const package_record& package,
                          std::size_t index,
                          std::map<std::string, std::string> current,
                          std::vector<std::map<std::string, std::string>>& output) -> result<void> {
    if (index == package.variants.size()) {
        output.push_back(std::move(current));
        return {};
    }
    const auto& variant = package.variants[index];
    auto values = variant.values;
    if (values.empty()) {
        values = {"false", "true"};
    }
    if (std::ranges::find(values, variant.default_value) == values.end()) {
        return std::unexpected(fail(errc::invalid_argument,
                                    "variant default is not one of its values",
                                    package.name + ":" + variant.name));
    }
    for (const auto& selected : values) {
        auto next = current;
        next[variant.name] = selected;
        auto expanded = variant_combinations(package, index + 1, std::move(next), output);
        if (!expanded) {
            return expanded;
        }
        if (output.size() > 4096) {
            return std::unexpected(fail(errc::invalid_argument,
                                        "package variant space exceeds 4096 configurations",
                                        package.name));
        }
    }
    return {};
}

auto candidates_for_dependency(const problem& value, const dependency_definition& dependency)
    -> std::vector<int> {
    auto output = std::vector<int>{};
    for (std::size_t index = 0; index != value.candidates.size(); ++index) {
        const auto& selected = value.candidates[index];
        const auto& package = value.universe.packages[selected.package];
        auto matches = !dependency.virtual_dependency && package.name == dependency.name &&
                       satisfies(selected.version, dependency.version);
        if (dependency.virtual_dependency) {
            matches = std::ranges::any_of(package.provides, [&](const auto& provided) {
                return provided.name == dependency.name &&
                       ranges_overlap(provided.version, dependency.version);
            });
        }
        if (matches) {
            output.push_back(selected.variable);
        }
    }
    return output;
}

auto candidates_for_spec(const problem& value, const spec& request) -> std::vector<int> {
    auto output = std::vector<int>{};
    for (const auto& selected : value.candidates) {
        const auto& package = value.universe.packages[selected.package];
        if (matches_spec(selected, package, request)) {
            output.push_back(selected.variable);
        }
    }
    return output;
}

auto compiler_candidates(const problem& value, std::span<const std::string> constraints)
    -> std::vector<int> {
    auto output = std::vector<int>{};
    for (const auto& raw : constraints) {
        auto parsed = parse_spec(raw);
        if (!parsed) {
            continue;
        }
        auto matching = candidates_for_spec(value, *parsed);
        output.insert(output.end(), matching.begin(), matching.end());
    }
    std::ranges::sort(output);
    output.erase(std::unique(output.begin(), output.end()), output.end());
    return output;
}

auto active_clauses(const problem& value, const std::vector<bool>& enabled)
    -> std::vector<std::vector<int>> {
    auto output = std::vector<std::vector<int>>{};
    for (const auto& item : value.clauses) {
        if (!item.origin || enabled[*item.origin]) {
            output.push_back(item.literals);
        }
    }
    return output;
}

auto solve_sat(const problem& value,
               const std::vector<bool>& enabled,
               std::span<const clause> extra = {}) -> std::optional<std::vector<int>> {
    auto clauses = active_clauses(value, enabled);
    for (const auto& item : extra) {
        clauses.push_back(item.literals);
    }
    auto solver = cdcl_solver{value.variable_count, std::move(clauses)};
    if (!solver.solve()) {
        return std::nullopt;
    }
    return solver.selected();
}

auto selected_candidates(const problem& value, std::span<const int> variables)
    -> std::vector<std::size_t> {
    auto output = std::vector<std::size_t>{};
    for (const auto variable : variables) {
        const auto found = std::ranges::find(value.candidates, variable, &candidate::variable);
        if (found != value.candidates.end()) {
            output.push_back(static_cast<std::size_t>(found - value.candidates.begin()));
        }
    }
    return output;
}

auto choose_dependency(const problem& value,
                       std::span<const std::size_t> selected,
                       const dependency_definition& dependency) -> std::optional<std::size_t> {
    for (const auto index : selected) {
        const auto& choice = value.candidates[index];
        const auto& package = value.universe.packages[choice.package];
        if (!dependency.virtual_dependency && package.name == dependency.name &&
            satisfies(choice.version, dependency.version)) {
            return index;
        }
        if (dependency.virtual_dependency &&
            std::ranges::any_of(package.provides, [&](const auto& provided) {
                return provided.name == dependency.name &&
                       ranges_overlap(provided.version, dependency.version);
            })) {
            return index;
        }
    }
    return std::nullopt;
}

class graph_builder {
public:
    graph_builder(const problem& source, std::vector<std::size_t> selected)
        : source_(source), selected_(std::move(selected)) {
        for (const auto index : selected_) {
            selected_by_package_[source_.candidates[index].package] = index;
        }
    }

    auto build() -> result<resolution_graph> {
        auto output = resolution_graph{};
        for (const auto& root : source_.roots) {
            const auto package = source_.package_by_name.find(root.name);
            if (package == source_.package_by_name.end()) {
                return std::unexpected(fail(errc::internal, "selected root package disappeared"));
            }
            auto built = build_package(package->second);
            if (!built) {
                return std::unexpected(built.error());
            }
            output.roots.push_back(*built);
        }
        for (auto& [package, node] : nodes_) {
            static_cast<void>(package);
            output.nodes.push_back(std::move(node));
        }
        std::ranges::sort(output.nodes, {}, &concretised_spec::name);
        std::ranges::sort(output.roots, [](const auto& left, const auto& right) {
            return to_string(left) < to_string(right);
        });
        auto identity_text = std::string{"resolution/1\n"};
        for (const auto& root : output.roots) {
            identity_text += to_string(root);
            identity_text += '\n';
        }
        const auto bytes = std::as_bytes(std::span{identity_text});
        auto identity = hash(algorithm::blake3, bytes);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        output.id = *identity;
        return output;
    }

private:
    auto root_for(std::string_view package) const -> const spec* {
        const auto found = std::ranges::find(source_.roots, package, &spec::name);
        return found == source_.roots.end() ? nullptr : &*found;
    }

    auto selected_compiler(std::size_t package_index, const candidate& choice)
        -> std::optional<std::size_t> {
        const auto& package = source_.universe.packages[package_index];
        auto constraints = package.compiler_constraints;
        if (const auto* root = root_for(package.name); root && root->compiler) {
            auto request = root->compiler->name;
            if (root->compiler->version) {
                request += '@';
                request += *root->compiler->version;
            }
            constraints = {std::move(request)};
        }
        const auto allowed = compiler_candidates(source_, constraints);
        for (const auto compiler : selected_) {
            if (compiler == selected_by_package_.at(package_index)) {
                continue;
            }
            if (std::ranges::find(allowed, source_.candidates[compiler].variable) !=
                allowed.end()) {
                return compiler;
            }
        }
        static_cast<void>(choice);
        return std::nullopt;
    }

    struct planned_edge {
        std::string name;
        dependency_type type{dependency_type::link};
        bool virtual_dependency{};
        bool compiler{};
        std::size_t package{};
    };

    struct build_frame {
        std::size_t package{};
        std::vector<planned_edge> edges;
        std::size_t next_edge{};
    };

    // Opens the frame of one selected package: marks it in progress and plans its dependency and
    // compiler edges without touching any dependency node. A package already in progress is a
    // cycle.
    auto open_frame(std::size_t package_index) -> result<build_frame> {
        if (!visiting_.insert(package_index).second) {
            return std::unexpected(fail(errc::conflict,
                                        "cyclic package dependency cannot be content-addressed",
                                        source_.universe.packages[package_index].name));
        }
        const auto selected = selected_by_package_.find(package_index);
        if (selected == selected_by_package_.end()) {
            return std::unexpected(fail(errc::internal, "required package is not selected"));
        }
        const auto& choice = source_.candidates[selected->second];
        const auto& package = source_.universe.packages[package_index];
        auto frame = build_frame{.package = package_index};
        for (const auto& dependency : package.dependencies) {
            if (!matches_when(choice, dependency.when)) {
                continue;
            }
            const auto target = choose_dependency(source_, selected_, dependency);
            if (!target) {
                continue;
            }
            frame.edges.push_back({dependency.name,
                                   dependency.type,
                                   dependency.virtual_dependency,
                                   false,
                                   source_.candidates[*target].package});
        }
        if (const auto compiler = selected_compiler(package_index, choice)) {
            const auto compiler_package = source_.candidates[*compiler].package;
            frame.edges.push_back({source_.universe.packages[compiler_package].name,
                                   dependency_type::build,
                                   false,
                                   true,
                                   compiler_package});
        }
        return frame;
    }

    // Closes a frame whose every edge target already has a node: binds the edge digests, hashes
    // the canonical record, and publishes the node.
    auto close_frame(const build_frame& frame) -> result<void> {
        const auto& choice = source_.candidates[selected_by_package_.at(frame.package)];
        const auto& package = source_.universe.packages[frame.package];
        auto node = concretised_spec{.name = package.name,
                                     .version = choice.version,
                                     .variants = choice.variants,
                                     .target = choice.target};
        for (const auto& edge : frame.edges) {
            const auto built = nodes_.find(edge.package);
            if (built == nodes_.end()) {
                return std::unexpected(
                    fail(errc::internal, "dependency node was not built before its parent"));
            }
            if (edge.compiler) {
                node.compiler = built->second.id;
            }
            node.dependencies.push_back(
                {edge.name, edge.type, built->second.id, edge.virtual_dependency});
        }
        for (const auto& patch : package.patches) {
            if (matches_when(choice, patch.when)) {
                node.patches.push_back(patch.value);
            }
        }
        if (const auto* root = root_for(package.name)) {
            node.reasons.push_back("selected by root " + liquid::to_string(*root));
        } else {
            node.reasons.push_back("selected as a dependency");
        }
        node.reasons.push_back("selected newest satisfying version " + choice.version);
        for (const auto& [name, selected_value] : choice.variants) {
            node.reasons.push_back("bound variant " + name + "=" + selected_value);
        }
        std::ranges::sort(node.dependencies, [](const auto& left, const auto& right) {
            return std::tuple{
                       left.name, left.type, left.virtual_dependency, to_string(left.target)} <
                   std::tuple{
                       right.name, right.type, right.virtual_dependency, to_string(right.target)};
        });
        std::ranges::sort(node.patches, [](const auto& left, const auto& right) {
            return to_string(left) < to_string(right);
        });
        auto encoded = encode_concretised_spec(node);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        auto identity = hash(algorithm::blake3, *encoded);
        if (!identity) {
            return std::unexpected(identity.error());
        }
        node.id = *identity;
        visiting_.erase(frame.package);
        nodes_.emplace(frame.package, std::move(node));
        return {};
    }

    // Builds the node of one package and, in postorder, every node it reaches. The traversal
    // keeps its frames in a heap vector instead of recursing: at most one frame is open per
    // selected package, so a dependency chain as long as the universe costs no native stack.
    // The solver runs on a runtime pool thread, whose stack on macOS is 512 KiB.
    auto build_package(std::size_t package_index) -> result<digest> {
        if (const auto found = nodes_.find(package_index); found != nodes_.end()) {
            return found->second.id;
        }
        auto first = open_frame(package_index);
        if (!first) {
            return std::unexpected(first.error());
        }
        auto frames = std::vector<build_frame>{};
        frames.push_back(std::move(*first));
        while (!frames.empty()) {
            auto& frame = frames.back();
            if (frame.next_edge != frame.edges.size()) {
                const auto child = frame.edges[frame.next_edge++].package;
                if (nodes_.contains(child)) {
                    continue;
                }
                auto next = open_frame(child);
                if (!next) {
                    return std::unexpected(next.error());
                }
                frames.push_back(std::move(*next));
                continue;
            }
            auto closed = close_frame(frame);
            if (!closed) {
                return std::unexpected(closed.error());
            }
            frames.pop_back();
        }
        return nodes_.at(package_index).id;
    }

    const problem& source_;
    std::vector<std::size_t> selected_;
    std::map<std::size_t, std::size_t> selected_by_package_;
    std::map<std::size_t, concretised_spec> nodes_;
    std::set<std::size_t> visiting_;
};

struct objective {
    std::size_t reused{};
    std::size_t known{};
    std::size_t newer{};
    std::size_t deviations{};
    std::string tie;
};

auto better(const objective& left, const objective& right) -> bool {
    return std::tuple{left.reused,
                      left.known,
                      std::numeric_limits<std::size_t>::max() - left.newer,
                      std::numeric_limits<std::size_t>::max() - left.deviations,
                      left.tie} >
           std::tuple{right.reused,
                      right.known,
                      std::numeric_limits<std::size_t>::max() - right.newer,
                      std::numeric_limits<std::size_t>::max() - right.deviations,
                      right.tie};
}

auto score(const problem& value,
           const resolution_graph& graph,
           std::span<const std::size_t> selected,
           const resolve_options& options) -> objective {
    auto output = objective{.tie = to_string(graph.id)};
    for (const auto& node : graph.nodes) {
        if (contains(options.reuse, node.id)) {
            ++output.reused;
        } else {
            for (const auto& fact : options.compatibility) {
                if (fact.compatible && digest_equal(fact.candidate, node.id) &&
                    contains(options.reuse, fact.built)) {
                    ++output.reused;
                    break;
                }
            }
        }
        if (contains(options.known_outputs, node.id)) {
            ++output.known;
        }
    }
    for (const auto index : selected) {
        output.newer += value.candidates[index].newer_rank;
        output.deviations += value.candidates[index].default_deviations;
    }
    return output;
}

auto explain_unsat(const problem& value) -> unsatisfiable {
    auto enabled = std::vector<bool>(value.origins.size(), true);
    for (std::size_t index = 0; index != value.origins.size(); ++index) {
        enabled[index] = false;
        if (solve_sat(value, enabled)) {
            enabled[index] = true;
        }
    }
    auto output = unsatisfiable{};
    for (std::size_t index = 0; index != enabled.size(); ++index) {
        if (enabled[index]) {
            output.core.push_back(value.origins[index]);
        }
    }
    std::ranges::sort(output.core, [](const auto& left, const auto& right) {
        return std::tie(left.origin.kind, left.origin.package, left.origin.detail) <
               std::tie(right.origin.kind, right.origin.package, right.origin.detail);
    });
    output.narrative = "Resolution is unsatisfiable.";
    for (const auto& item : output.core) {
        output.narrative += "\n- ";
        output.narrative += origin_name(item.origin);
        output.narrative += " ";
        output.narrative += item.origin.package;
        output.narrative += ": ";
        output.narrative += item.origin.detail;
    }
    output.narrative +=
        std::format("\nNo configuration satisfies all {} constraints.", output.core.size());
    output.json = "{\"version\":1,\"unsatisfiable\":true,\"core\":[";
    for (std::size_t index = 0; index != output.core.size(); ++index) {
        if (index != 0) {
            output.json += ',';
        }
        const auto& item = output.core[index];
        output.json += std::format(
            "{{\"kind\":\"{}\",\"package\":\"{}\",\"constraint\":\"{}\",\"detail\":\"{}\"}}",
            origin_name(item.origin),
            escape_json(item.origin.package),
            escape_json(item.constraint),
            escape_json(item.origin.detail));
    }
    output.json += "],\"narrative\":\"" + escape_json(output.narrative) + "\"}";
    return output;
}

auto ground_target(problem& output) -> void {
    for (const auto& root : output.roots) {
        if (!root.target) {
            continue;
        }
        if (!output.requested_target.empty() && output.requested_target != *root.target) {
            const auto origin = add_origin(output,
                                           constraint_origin_kind::target,
                                           root.name,
                                           "root targets " + output.requested_target + " and " +
                                               *root.target + " conflict");
            add_clause(output, {}, origin);
        }
        output.requested_target = *root.target;
    }
}

auto ground_candidates(problem& output) -> result<void> {
    for (std::size_t package_index = 0; package_index != output.universe.packages.size();
         ++package_index) {
        auto& package = output.universe.packages[package_index];
        if (!output.package_by_name.emplace(package.name, package_index).second) {
            return std::unexpected(
                fail(errc::invalid_argument, "duplicate package '" + package.name + "'"));
        }
        std::ranges::sort(package.versions, [](const auto& left, const auto& right) {
            return compare_versions(left.version, right.version) == std::strong_ordering::greater;
        });
        std::ranges::sort(package.variants, {}, &variant_definition::name);
        auto combinations = std::vector<std::map<std::string, std::string>>{};
        auto expanded = variant_combinations(package, 0, {}, combinations);
        if (!expanded) {
            return std::unexpected(expanded.error());
        }
        auto targets = package.target_constraints;
        if (targets.empty()) {
            targets.push_back(output.requested_target.empty() ? "generic"
                                                              : output.requested_target);
        }
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (!output.requested_target.empty()) {
            std::erase_if(targets, [&](const auto& target) {
                return !target_matches(target, output.requested_target);
            });
        }
        std::ranges::sort(combinations, [&](const auto& left, const auto& right) {
            const auto deviations = [&](const auto& combination) {
                return std::ranges::count_if(package.variants, [&](const auto& definition) {
                    return combination.at(definition.name) != definition.default_value;
                });
            };
            return std::tuple{deviations(left), left} > std::tuple{deviations(right), right};
        });
        for (std::size_t offset = 0; offset != package.versions.size(); ++offset) {
            const auto version_index = package.versions.size() - offset - 1;
            for (const auto& variants : combinations) {
                const auto deviations =
                    std::ranges::count_if(package.variants, [&](const auto& definition) {
                        return variants.at(definition.name) != definition.default_value;
                    });
                for (const auto& target : targets) {
                    const auto probe = candidate{
                        .package = package_index,
                        .version = package.versions[version_index].version,
                        .variants = variants,
                        .target = target,
                    };
                    const auto applicable =
                        std::ranges::all_of(package.variants, [&](const auto& definition) {
                            return matches_when(probe, definition.when) ||
                                   variants.at(definition.name) == definition.default_value;
                        });
                    if (!applicable) {
                        continue;
                    }
                    const auto index = output.candidates.size();
                    const auto variable = ++output.variable_count;
                    output.candidates.push_back({package_index,
                                                 package.versions[version_index].version,
                                                 variants,
                                                 target,
                                                 variable,
                                                 version_index,
                                                 static_cast<std::size_t>(deviations)});
                    output.by_package[package_index].push_back(index);
                }
            }
        }
    }
    return {};
}

auto ground_uniqueness(problem& output) -> void {
    for (const auto& indices : output.by_package) {
        for (std::size_t left = 0; left != indices.size(); ++left) {
            for (std::size_t right = left + 1; right != indices.size(); ++right) {
                add_clause(output,
                           {-output.candidates[indices[left]].variable,
                            -output.candidates[indices[right]].variable});
            }
        }
    }
}

auto ground_roots(problem& output) -> void {
    for (const auto& root : output.roots) {
        const auto origin = add_origin(
            output, constraint_origin_kind::root, root.name, "root requires " + to_string(root));
        add_clause(output, candidates_for_spec(output, root), origin);
        for (const auto& dependency : root.dependencies) {
            const auto dependency_origin =
                add_origin(output,
                           constraint_origin_kind::root,
                           root.name,
                           "root constrains dependency " + to_string(dependency));
            add_clause(output, candidates_for_spec(output, dependency), dependency_origin);
        }
        if (!root.compiler) {
            continue;
        }
        auto compiler_text = root.compiler->name;
        if (root.compiler->version) {
            compiler_text += '@';
            compiler_text += *root.compiler->version;
        }
        const auto compiler_origin = add_origin(output,
                                                constraint_origin_kind::compiler,
                                                root.name,
                                                "root requires compiler " + compiler_text);
        add_clause(output,
                   compiler_candidates(output, std::span<const std::string>{&compiler_text, 1}),
                   compiler_origin);
    }
}

auto ground_dependencies(problem& output, std::size_t package_index, const package_record& package)
    -> void {
    for (const auto& dependency : package.dependencies) {
        const auto origin = add_origin(output,
                                       constraint_origin_kind::dependency,
                                       package.name,
                                       package.name + " requires " + dependency.name +
                                           (dependency.version ? "@" + *dependency.version : ""));
        const auto targets = candidates_for_dependency(output, dependency);
        for (const auto index : output.by_package[package_index]) {
            const auto& parent = output.candidates[index];
            if (!matches_when(parent, dependency.when)) {
                continue;
            }
            auto literals = targets;
            literals.push_back(-parent.variable);
            add_clause(output, std::move(literals), origin);
        }
    }
}

auto ground_compilers(problem& output, std::size_t package_index, const package_record& package)
    -> result<void> {
    if (package.compiler_constraints.empty()) {
        return {};
    }
    for (const auto& constraint : package.compiler_constraints) {
        auto parsed = parse_spec(constraint);
        if (!parsed || parsed->name.empty()) {
            return std::unexpected(fail(errc::invalid_argument,
                                        "invalid compiler constraint",
                                        package.name + ":" + constraint));
        }
    }
    const auto origin =
        add_origin(output,
                   constraint_origin_kind::compiler,
                   package.name,
                   package.name + " requires compiler " + package.compiler_constraints.front());
    const auto compilers = compiler_candidates(output, package.compiler_constraints);
    for (const auto index : output.by_package[package_index]) {
        auto literals = compilers;
        literals.push_back(-output.candidates[index].variable);
        add_clause(output, std::move(literals), origin);
    }
    return {};
}

auto ground_conflicts(problem& output, std::size_t package_index, const package_record& package)
    -> void {
    for (const auto& conflict : package.conflicts) {
        const auto origin =
            add_origin(output,
                       constraint_origin_kind::conflict,
                       package.name,
                       conflict.message.empty()
                           ? package.name + " conflicts with " + to_string(conflict.constraint)
                           : conflict.message);
        const auto targets = candidates_for_spec(output, conflict.constraint);
        for (const auto index : output.by_package[package_index]) {
            const auto& parent = output.candidates[index];
            if (!matches_when(parent, conflict.when)) {
                continue;
            }
            if (conflict.constraint.name == package.name &&
                matches_spec(parent, package, conflict.constraint)) {
                add_clause(output, {-parent.variable}, origin);
                continue;
            }
            for (const auto target : targets) {
                add_clause(output, {-parent.variable, -target}, origin);
            }
        }
    }
}

auto ground_package_rules(problem& output) -> result<void> {
    for (std::size_t package_index = 0; package_index != output.universe.packages.size();
         ++package_index) {
        const auto& package = output.universe.packages[package_index];
        ground_dependencies(output, package_index, package);
        auto compilers = ground_compilers(output, package_index, package);
        if (!compilers) {
            return compilers;
        }
        ground_conflicts(output, package_index, package);
    }
    return {};
}

class cdcl_concretizer final : public concretizer {
public:
    auto ground(package_universe universe, std::vector<spec> roots) const
        -> result<std::unique_ptr<grounded_concretization>> override {
        if (roots.empty()) {
            return std::unexpected(
                fail(errc::invalid_argument, "at least one root spec is required"));
        }
        std::ranges::sort(universe.packages, {}, &package_record::name);
        auto output = std::make_unique<problem>();
        output->universe = std::move(universe);
        output->roots = std::move(roots);
        ground_target(*output);
        output->by_package.resize(output->universe.packages.size());
        auto candidates = ground_candidates(*output);
        if (!candidates) {
            return std::unexpected(candidates.error());
        }
        ground_uniqueness(*output);
        ground_roots(*output);
        auto rules = ground_package_rules(*output);
        if (!rules) {
            return std::unexpected(rules.error());
        }
        return std::unique_ptr<grounded_concretization>{std::move(output)};
    }

    auto solve(std::unique_ptr<grounded_concretization> abstract, resolve_options options) const
        -> result<resolution> override {
        auto* value = dynamic_cast<problem*>(abstract.get());
        if (!value) {
            return std::unexpected(fail(errc::internal, "invalid concretizer problem"));
        }
        if (options.stop.stop_requested()) {
            return std::unexpected(fail(errc::busy, "resolution cancelled"));
        }
        auto enabled = std::vector<bool>(value->origins.size(), true);
        auto extra = std::vector<clause>{};
        auto best = std::optional<resolution_graph>{};
        auto best_objective = objective{};
        for (;;) {
            if (options.stop.stop_requested()) {
                return std::unexpected(fail(errc::busy, "resolution cancelled"));
            }
            auto model = solve_sat(*value, enabled, extra);
            if (!model) {
                break;
            }
            const auto selected = selected_candidates(*value, *model);
            auto graph = graph_builder{*value, selected}.build();
            if (graph) {
                const auto candidate_score = score(*value, *graph, selected, options);
                if (!best || better(candidate_score, best_objective)) {
                    best = std::move(*graph);
                    best_objective = candidate_score;
                }
                const auto objectives_absent =
                    options.reuse.empty() && options.known_outputs.empty();
                const auto maximum_reuse = !options.reuse.empty() &&
                                           options.compatibility.empty() &&
                                           candidate_score.reused >= options.reuse.size();
                const auto maximum_known = options.reuse.empty() &&
                                           !options.known_outputs.empty() &&
                                           candidate_score.known >= options.known_outputs.size();
                if (objectives_absent || maximum_reuse || maximum_known) {
                    break;
                }
            }
            auto blocking = std::vector<int>{};
            for (const auto index : selected) {
                blocking.push_back(-value->candidates[index].variable);
            }
            if (blocking.empty()) {
                break;
            }
            extra.push_back({std::move(blocking), std::nullopt});
        }
        if (!best) {
            return resolution{explain_unsat(*value)};
        }
        for (auto& node : best->nodes) {
            if (contains(options.reuse, node.id)) {
                node.reasons.insert(node.reasons.begin(), "reused already-built concretisation");
            } else if (std::ranges::any_of(options.compatibility, [&](const auto& fact) {
                           return fact.compatible && digest_equal(fact.candidate, node.id) &&
                                  contains(options.reuse, fact.built);
                       })) {
                node.reasons.insert(node.reasons.begin(),
                                    "reused ABI-compatible built concretisation");
            } else if (contains(options.known_outputs, node.id)) {
                node.reasons.insert(node.reasons.begin(),
                                    "selected configuration with known identical output");
            }
        }
        return resolution{std::move(*best)};
    }
};

} // namespace

auto make_concretizer() -> std::shared_ptr<const concretizer> {
    return std::make_shared<cdcl_concretizer>();
}

} // namespace liquid::resolve_detail
