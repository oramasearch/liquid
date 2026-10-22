#include "formats/xml/xml.hpp"
#include "formats/structural_budget.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace liquid::formats::xml {
namespace {

auto failure(std::string message) -> error {
    return {errc::corrupt, std::move(message), "XML"};
}

auto is_name_char(char value) -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' || value == ':' ||
           value == '.';
}

class parser {
public:
    parser(std::string_view input, bool omit_relationship_ids)
        : input_(input), omit_relationship_ids_(omit_relationship_ids) {}

    auto parse() -> result<std::string> {
        skip_misc();
        auto output = element(nullptr);
        if (!output) {
            return std::unexpected(output.error());
        }
        skip_misc();
        if (position_ != input_.size()) {
            return std::unexpected(failure("trailing XML data"));
        }
        return output;
    }

private:
    // The declarations one element makes. Lookups walk outward through at most the depth
    // limit, so a child never copies the namespaces its ancestors declared.
    struct namespace_scope {
        const namespace_scope* parent{};
        std::map<std::string, std::string, std::less<>> declared;

        auto find(std::string_view prefix) const -> const std::string* {
            for (const auto* scope = this; scope != nullptr; scope = scope->parent) {
                if (const auto found = scope->declared.find(prefix);
                    found != scope->declared.end()) {
                    return &found->second;
                }
            }
            return nullptr;
        }
    };

    auto skip_space() -> void {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    auto skip_misc() -> void {
        for (;;) {
            skip_space();
            if (input_.substr(position_).starts_with("<?")) {
                const auto end = input_.find("?>", position_ + 2);
                position_ = end == std::string_view::npos ? input_.size() : end + 2;
            } else if (input_.substr(position_).starts_with("<!--")) {
                const auto end = input_.find("-->", position_ + 4);
                position_ = end == std::string_view::npos ? input_.size() : end + 3;
            } else if (input_.substr(position_).starts_with("<!DOCTYPE")) {
                const auto end = input_.find('>', position_ + 9);
                position_ = end == std::string_view::npos ? input_.size() : end + 1;
            } else {
                return;
            }
        }
    }

    auto name() -> result<std::string> {
        const auto start = position_;
        while (position_ < input_.size() && is_name_char(input_[position_])) {
            ++position_;
        }
        if (start == position_) {
            return std::unexpected(failure("XML name is absent"));
        }
        return std::string{input_.substr(start, position_ - start)};
    }

    auto quoted() -> result<std::string> {
        if (position_ == input_.size() || (input_[position_] != '\'' && input_[position_] != '"')) {
            return std::unexpected(failure("XML attribute value is not quoted"));
        }
        const auto quote = input_[position_++];
        const auto start = position_;
        while (position_ < input_.size() && input_[position_] != quote) {
            ++position_;
        }
        if (position_ == input_.size()) {
            return std::unexpected(failure("unterminated XML attribute"));
        }
        return std::string{input_.substr(start, position_++ - start)};
    }

    auto expanded_name(const std::string& value, const namespace_scope& namespaces, bool attribute)
        -> std::string {
        const auto separator = value.find(':');
        const auto prefix =
            separator == std::string::npos ? std::string{} : value.substr(0, separator);
        const auto local = separator == std::string::npos ? value : value.substr(separator + 1);
        const auto* found = namespaces.find(prefix);
        if (found == nullptr || (attribute && prefix.empty())) {
            return local;
        }
        return "{" + *found + "}" + local;
    }

    auto element(const namespace_scope* inherited, std::size_t depth = 0) -> result<std::string> {
        if (!budget_.consume(depth)) {
            return std::unexpected(failure("XML structural limit exceeded"));
        }
        auto namespaces = namespace_scope{.parent = inherited};
        if (position_ == input_.size() || input_[position_++] != '<') {
            return std::unexpected(failure("XML element is absent"));
        }
        auto raw_name = name();
        if (!raw_name) {
            return std::unexpected(raw_name.error());
        }
        auto attributes = std::vector<std::pair<std::string, std::string>>{};
        for (;;) {
            skip_space();
            if (input_.substr(position_).starts_with("/>")) {
                position_ += 2;
                auto output = "<" + expanded_name(*raw_name, namespaces, false);
                std::ranges::sort(attributes);
                for (const auto& [name, value] : attributes) {
                    output += " " + name + "=\"" + value + "\"";
                }
                return output + "/>";
            }
            if (position_ < input_.size() && input_[position_] == '>') {
                ++position_;
                break;
            }
            if (!budget_.consume(depth)) {
                return std::unexpected(failure("XML structural limit exceeded"));
            }
            auto raw_attribute = name();
            if (!raw_attribute) {
                return std::unexpected(raw_attribute.error());
            }
            skip_space();
            if (position_ == input_.size() || input_[position_++] != '=') {
                return std::unexpected(failure("XML attribute assignment is absent"));
            }
            skip_space();
            auto value = quoted();
            if (!value) {
                return std::unexpected(value.error());
            }
            if (*raw_attribute == "xmlns") {
                namespaces.declared[""] = *value;
            } else if (raw_attribute->starts_with("xmlns:")) {
                namespaces.declared[raw_attribute->substr(6)] = *value;
            } else if (!(omit_relationship_ids_ && raw_attribute->ends_with(":id") &&
                         value->starts_with("rId"))) {
                attributes.emplace_back(expanded_name(*raw_attribute, namespaces, true),
                                        std::move(*value));
            }
        }
        auto output = "<" + expanded_name(*raw_name, namespaces, false);
        std::ranges::sort(attributes);
        for (const auto& [name, value] : attributes) {
            output += " " + name + "=\"" + value + "\"";
        }
        output += ">";
        for (;;) {
            if (position_ >= input_.size()) {
                return std::unexpected(failure("unterminated XML element"));
            }
            if (input_.substr(position_).starts_with("</")) {
                position_ += 2;
                auto close = name();
                if (!close || *close != *raw_name || position_ == input_.size() ||
                    input_[position_++] != '>') {
                    return std::unexpected(failure("mismatched XML close element"));
                }
                return output + "</" + expanded_name(*raw_name, namespaces, false) + ">";
            }
            if (input_[position_] == '<') {
                if (input_.substr(position_).starts_with("<!--")) {
                    const auto end = input_.find("-->", position_ + 4);
                    if (end == std::string_view::npos) {
                        return std::unexpected(failure("unterminated XML comment"));
                    }
                    position_ = end + 3;
                    continue;
                }
                auto child = element(&namespaces, depth + 1);
                if (!child) {
                    return std::unexpected(child.error());
                }
                output += *child;
                continue;
            }
            const auto start = position_;
            while (position_ < input_.size() && input_[position_] != '<') {
                ++position_;
            }
            auto text = input_.substr(start, position_ - start);
            if (text.find_first_not_of(" \t\r\n") != std::string_view::npos) {
                output += text;
            }
        }
    }

    std::string_view input_;
    std::size_t position_{};
    structural_budget budget_;
    bool omit_relationship_ids_{};
};

} // namespace

auto canonicalise(std::string_view input, bool omit_relationship_ids) -> result<std::string> {
    return parser{input, omit_relationship_ids}.parse();
}

} // namespace liquid::formats::xml
