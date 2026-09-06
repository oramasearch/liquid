#include "formats/xml/xml.hpp"

#include <algorithm>
#include <map>
#include <string>
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
        auto namespaces = namespace_scope{};
        auto output = element(namespaces);
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
    using namespace_scope = std::map<std::string, std::string>;

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
        const auto found = namespaces.find(prefix);
        if (found == namespaces.end() || (attribute && prefix.empty())) {
            return local;
        }
        return "{" + found->second + "}" + local;
    }

    auto element(namespace_scope namespaces) -> result<std::string> {
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
                namespaces[""] = *value;
            } else if (raw_attribute->starts_with("xmlns:")) {
                namespaces[raw_attribute->substr(6)] = *value;
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
                auto child = element(namespaces);
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
    bool omit_relationship_ids_{};
};

} // namespace

auto canonicalise(std::string_view input, bool omit_relationship_ids) -> result<std::string> {
    return parser{input, omit_relationship_ids}.parse();
}

} // namespace liquid::formats::xml
