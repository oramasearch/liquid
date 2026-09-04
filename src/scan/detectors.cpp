#include "scan/detectors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace liquid::scan_detail {
namespace {

struct detector {
    std::string_view id;
    std::string_view description;
    bool (*matches)(std::string_view);
};

auto is_upper_or_digit(char value) -> bool {
    const auto byte = static_cast<unsigned char>(value);
    return std::isupper(byte) != 0 || std::isdigit(byte) != 0;
}

auto is_alphanumeric(char value) -> bool {
    return std::isalnum(static_cast<unsigned char>(value)) != 0;
}

auto is_base64(char value) -> bool {
    return is_alphanumeric(value) || value == '+' || value == '/' || value == '=';
}

auto is_base64url(char value) -> bool {
    return is_alphanumeric(value) || value == '-' || value == '_';
}

auto lower(std::string_view value) -> std::string {
    auto output = std::string{value};
    std::ranges::transform(output, output.begin(), [](char item) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(item)));
    });
    return output;
}

auto bounded_token(std::string_view line,
                   std::string_view prefix,
                   std::size_t suffix_size,
                   bool (*valid)(char)) -> bool {
    auto at = line.find(prefix);
    while (at != std::string_view::npos) {
        const auto end = at + prefix.size() + suffix_size;
        const auto before_ok = at == 0 || !valid(line[at - 1]);
        const auto after_ok = end == line.size() || (end < line.size() && !valid(line[end]));
        if (end <= line.size() && before_ok && after_ok &&
            std::ranges::all_of(line.substr(at + prefix.size(), suffix_size), valid)) {
            return true;
        }
        at = line.find(prefix, at + 1);
    }
    return false;
}

auto assignment_value(std::string_view line, std::string_view key) -> std::optional<std::string> {
    const auto folded = lower(line);
    auto at = folded.find(key);
    while (at != std::string::npos) {
        const auto boundary =
            at == 0 || (!is_alphanumeric(folded[at - 1]) && folded[at - 1] != '_');
        if (boundary) {
            break;
        }
        at = folded.find(key, at + 1);
    }
    if (at == std::string::npos)
        return std::nullopt;
    at += key.size();
    while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) != 0) {
        ++at;
    }
    if (at >= line.size() || (line[at] != '=' && line[at] != ':')) {
        return std::nullopt;
    }
    ++at;
    while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at])) != 0) {
        ++at;
    }
    auto end = line.size();
    while (end > at && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
        --end;
    }
    if (end > at + 1 && ((line[at] == '"' && line[end - 1] == '"') ||
                         (line[at] == '\'' && line[end - 1] == '\''))) {
        ++at;
        --end;
    }
    return std::string{line.substr(at, end - at)};
}

auto aws_access_key(std::string_view line) -> bool {
    return bounded_token(line, "AKIA", 16, is_upper_or_digit);
}

auto aws_secret_key(std::string_view line) -> bool {
    constexpr std::array keys{"aws_secret_access_key", "secret_access_key", "awssecretaccesskey"};
    for (const auto key : keys) {
        const auto value = assignment_value(line, key);
        if (value && value->size() == 40 && std::ranges::all_of(*value, is_base64)) {
            return true;
        }
    }
    return false;
}

auto github_token(std::string_view line) -> bool {
    if (bounded_token(line, "ghp_", 36, is_alphanumeric) ||
        bounded_token(line, "gho_", 36, is_alphanumeric)) {
        return true;
    }
    auto at = line.find("github_pat_");
    while (at != std::string_view::npos) {
        auto end = at + std::string_view{"github_pat_"}.size();
        while (end < line.size() && (is_alphanumeric(line[end]) || line[end] == '_')) {
            ++end;
        }
        if (end - at >= 32) {
            return true;
        }
        at = line.find("github_pat_", at + 1);
    }
    return false;
}

auto slack_token(std::string_view line) -> bool {
    constexpr std::array prefixes{"xoxb-", "xoxp-", "xoxa-", "xoxr-", "xoxs-", "xapp-"};
    for (const auto prefix : prefixes) {
        auto at = line.find(prefix);
        while (at != std::string_view::npos) {
            auto end = at + std::string_view{prefix}.size();
            while (end < line.size() && (is_alphanumeric(line[end]) || line[end] == '-')) {
                ++end;
            }
            if (end - at >= 20) {
                return true;
            }
            at = line.find(prefix, at + 1);
        }
    }
    return false;
}

auto google_api_key(std::string_view line) -> bool {
    return bounded_token(line, "AIza", 35, is_base64url);
}

auto private_key(std::string_view line) -> bool {
    constexpr std::array labels{"RSA", "EC", "OPENSSH", "PGP"};
    return std::ranges::any_of(labels, [&](const auto label) {
        return line.contains(std::string{"-----BEGIN "} + label + " PRIVATE KEY-----") ||
               (std::string_view{label} == "PGP" &&
                line.contains("-----BEGIN PGP PRIVATE KEY BLOCK-----"));
    });
}

auto jwt(std::string_view line) -> bool {
    auto start = line.find("eyJ");
    while (start != std::string_view::npos) {
        auto end = start;
        auto valid = true;
        for (auto part = 0; part != 3; ++part) {
            const auto part_start = end;
            while (end < line.size() && is_base64url(line[end])) {
                ++end;
            }
            if (end - part_start < 8) {
                valid = false;
            }
            if (part != 2) {
                if (end >= line.size() || line[end] != '.') {
                    valid = false;
                    break;
                }
                ++end;
            }
        }
        if (valid) {
            return true;
        }
        start = line.find("eyJ", start + 1);
    }
    return false;
}

auto high_entropy_assignment(std::string_view line) -> bool {
    constexpr std::array keys{"password", "secret", "token"};
    for (const auto key : keys) {
        const auto value = assignment_value(line, key);
        if (value && value->size() >= 16 && shannon_entropy(*value) >= 3.5) {
            return true;
        }
    }
    return false;
}

auto dotenv_assignment(std::string_view line) -> bool {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos || equals < 2) {
        return false;
    }
    const auto key = line.substr(0, equals);
    if (!std::ranges::all_of(key, [](char value) {
            return std::isupper(static_cast<unsigned char>(value)) != 0 ||
                   std::isdigit(static_cast<unsigned char>(value)) != 0 || value == '_';
        })) {
        return false;
    }
    auto value = line.substr(equals + 1);
    if (value.size() > 1 && ((value.front() == '"' && value.back() == '"') ||
                             (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value.size() >= 20 && shannon_entropy(value) >= 4.0;
}

constexpr std::array detectors{
    detector{"aws-access-key", "AWS access key identifier", aws_access_key},
    detector{"aws-secret-key", "AWS secret key assignment", aws_secret_key},
    detector{"github-token", "GitHub access token", github_token},
    detector{"slack-token", "Slack access token", slack_token},
    detector{"google-api-key", "Google API key", google_api_key},
    detector{"private-key", "private key PEM header", private_key},
    detector{"jwt", "JSON Web Token", jwt},
    detector{"generic-assignment", "high-entropy secret assignment", high_entropy_assignment},
    detector{"dotenv-assignment", "high-entropy environment assignment", dotenv_assignment},
};

auto detector_allowed(const scan_allow_rule& rule, std::string_view detector_id) -> bool {
    return rule.detectors.empty() ||
           std::ranges::find(rule.detectors, detector_id) != rule.detectors.end();
}

} // namespace

auto shannon_entropy(std::string_view value) -> double {
    if (value.empty()) {
        return 0.0;
    }
    auto frequencies = std::array<std::size_t, 256>{};
    for (const auto item : value) {
        ++frequencies[static_cast<unsigned char>(item)];
    }
    auto entropy = 0.0;
    for (const auto frequency : frequencies) {
        if (frequency == 0) {
            continue;
        }
        const auto probability = static_cast<double>(frequency) / static_cast<double>(value.size());
        entropy -= probability * std::log2(probability);
    }
    return entropy;
}

auto glob_matches(std::string_view pattern, std::string_view path) -> bool {
    auto pattern_at = std::size_t{};
    auto path_at = std::size_t{};
    auto star = std::string_view::npos;
    auto retry = std::size_t{};
    while (path_at < path.size()) {
        if (pattern_at < pattern.size() && pattern[pattern_at] == '?') {
            if (path[path_at] == '/') {
                return false;
            }
            ++pattern_at;
            ++path_at;
            continue;
        }
        if (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
            const auto recursive =
                pattern_at + 1 < pattern.size() && pattern[pattern_at + 1] == '*';
            star = pattern_at;
            pattern_at += recursive ? 2 : 1;
            retry = path_at;
            continue;
        }
        if (pattern_at < pattern.size() && pattern[pattern_at] == path[path_at]) {
            ++pattern_at;
            ++path_at;
            continue;
        }
        if (star == std::string_view::npos) {
            return false;
        }
        const auto recursive = star + 1 < pattern.size() && pattern[star + 1] == '*';
        if (!recursive && retry < path.size() && path[retry] == '/') {
            return false;
        }
        path_at = ++retry;
        pattern_at = star + (recursive ? 2 : 1);
    }
    while (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
        ++pattern_at;
    }
    return pattern_at == pattern.size();
}

auto detector_set::scan(std::string_view path,
                        std::span<const std::byte> contents,
                        const scan_configuration& config,
                        std::optional<std::span<const byte_range>> changed) const -> report {
    auto output = report{};
    for (const auto& candidate : detectors) {
        output.detectors.emplace_back(candidate.id);
    }
    const auto text =
        std::string_view{reinterpret_cast<const char*>(contents.data()), contents.size()};
    auto line_number = std::size_t{1};
    auto at = std::size_t{};
    while (at <= text.size()) {
        const auto end = text.find('\n', at);
        const auto line =
            text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        const auto line_end = end == std::string_view::npos ? text.size() : end + 1;
        const auto selected = !changed || std::ranges::any_of(*changed, [&](const auto& range) {
            return range.offset < line_end && range.offset + range.size > at;
        });
        if (!selected) {
            if (end == std::string_view::npos) {
                break;
            }
            at = end + 1;
            ++line_number;
            continue;
        }
        const auto marker = !config.ignore_lines.empty() && line.contains(config.ignore_lines);
        for (const auto& candidate : detectors) {
            if (!candidate.matches(line)) {
                continue;
            }
            auto value =
                finding{std::string{candidate.id}, std::string{candidate.description}, line_number};
            if (marker) {
                output.allowed.push_back({std::move(value), "line-marker"});
                continue;
            }
            const auto rule = std::ranges::find_if(config.allow, [&](const auto& item) {
                return glob_matches(item.path, path) && detector_allowed(item, candidate.id);
            });
            if (rule != config.allow.end()) {
                output.allowed.push_back({std::move(value), "allow-list"});
            } else {
                output.blocked.push_back(std::move(value));
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
        ++line_number;
    }
    return output;
}

} // namespace liquid::scan_detail
