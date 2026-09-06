#include "admission/internal.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/json.hpp"
#include "http1/client.hpp"
#include "liquid/store.hpp"

namespace liquid::admission_detail {
namespace {

namespace json = formats::json;

auto source_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto hex_value(char value) -> int {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

auto decode_hex(std::string_view input) -> result<std::vector<std::byte>> {
    if (input.size() % 2 != 0) {
        return std::unexpected(source_error(errc::corrupt, "attestation hex is truncated"));
    }
    auto output = std::vector<std::byte>{};
    output.reserve(input.size() / 2);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 2) {
        const auto high = hex_value(input[offset]);
        const auto low = hex_value(input[offset + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(source_error(errc::corrupt, "attestation hex is invalid"));
        }
        output.push_back(std::byte{static_cast<unsigned char>((high << 4) | low)});
    }
    return output;
}

auto json_field(const json::value& value, std::string_view name) -> const json::value* {
    const auto* object = std::get_if<json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto percent_encode(std::string_view input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789ABCDEF"};
    auto output = std::string{};
    for (const auto character : input) {
        const auto value = static_cast<unsigned char>(character);
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
            value == '~') {
            output.push_back(character);
        } else {
            output.push_back('%');
            output.push_back(digits[value >> 4]);
            output.push_back(digits[value & 0x0f]);
        }
    }
    return output;
}

class local_source final : public attestation_source {
public:
    explicit local_source(liquid::store value) : store_(std::move(value)) {}

    runtime::task<std::vector<attestation>> by_subject(digest subject) override {
        auto output = std::vector<attestation>{};
        for (const auto& id : store_.attestations(subject)) {
            auto bytes = co_await store_.get(id);
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            auto statement = decode_attestation(*bytes);
            if (!statement) {
                co_return std::unexpected(statement.error());
            }
            output.push_back(std::move(*statement));
        }
        co_return output;
    }

private:
    liquid::store store_;
};

class node_source final : public attestation_source {
public:
    explicit node_source(std::string url) : url_(std::move(url)) {}

    runtime::task<std::vector<attestation>> by_subject(digest subject) override {
        auto response = co_await http1::request(
            {.url = url_,
             .suffix = "/v1/attestations?subject=" + percent_encode(to_string(subject))});
        if (!response) {
            co_return std::unexpected(source_error(
                errc::unavailable, response.error().message, response.error().context));
        }
        if (response->status < 200 || response->status >= 300) {
            co_return std::unexpected(
                source_error(errc::unavailable,
                             "attestation source returned HTTP " + std::to_string(response->status),
                             url_));
        }
        const auto text = std::string{reinterpret_cast<const char*>(response->body.data()),
                                      response->body.size()};
        auto document = json::parse(text);
        const auto* values = document ? json_field(*document, "attestations") : nullptr;
        const auto* array = values ? std::get_if<json::value::array>(&values->data) : nullptr;
        if (!array) {
            co_return std::unexpected(
                source_error(errc::corrupt, "invalid attestation source response", url_));
        }
        auto output = std::vector<attestation>{};
        output.reserve(array->size());
        for (const auto& item : *array) {
            const auto* encoded = std::get_if<std::string>(&item.data);
            if (!encoded) {
                co_return std::unexpected(
                    source_error(errc::corrupt, "attestation source entry is not text", url_));
            }
            auto bytes = decode_hex(*encoded);
            auto statement = bytes ? decode_attestation(*bytes)
                                   : result<attestation>{std::unexpected(bytes.error())};
            if (!statement) {
                co_return std::unexpected(statement.error());
            }
            output.push_back(std::move(*statement));
        }
        co_return output;
    }

private:
    std::string url_;
};

class checkpoint_log_verifier final : public log_verifier {
public:
    checkpoint_log_verifier(admission_log_policy policy, std::filesystem::path cache)
        : policy_(std::move(policy)), cache_(std::move(cache)) {}

    runtime::task<log_verdict> verify_inclusion(digest attestation) override {
        static_cast<void>(attestation);
        if (!policy_.lake.empty()) {
            auto response = co_await http1::request(
                {.url = policy_.lake, .suffix = "/v1/transparency_checkpoint"});
            if (response && response->status >= 200 && response->status < 300 && !cache_.empty()) {
                auto stored = co_await runtime::offload([path = cache_, body = response->body] {
                    auto code = std::error_code{};
                    if (path.has_parent_path()) {
                        std::filesystem::create_directories(path.parent_path(), code);
                    }
                    if (code) {
                        return false;
                    }
                    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
                    output.write(reinterpret_cast<const char*>(body.data()),
                                 static_cast<std::streamsize>(body.size()));
                    return static_cast<bool>(output);
                });
                static_cast<void>(stored);
            }
        }
        if (!cache_.empty()) {
            auto cache_fresh = co_await runtime::offload(
                [path = cache_, maximum_age = policy_.max_checkpoint_age_seconds] {
                    auto code = std::error_code{};
                    const auto modified = std::filesystem::last_write_time(path, code);
                    if (code) {
                        return false;
                    }
                    const auto now = std::filesystem::file_time_type::clock::now();
                    return maximum_age == 0 || now - modified <= std::chrono::seconds{maximum_age};
                });
            static_cast<void>(cache_fresh);
        }
        co_return log_verdict::unavailable;
    }

private:
    admission_log_policy policy_;
    std::filesystem::path cache_;
};

} // namespace

runtime::task<std::shared_ptr<attestation_source>> open_source(std::string location) {
    if (location.starts_with("http://") || location.starts_with("https://")) {
        auto valid = http1::validate_url(location);
        if (!valid) {
            co_return std::unexpected(valid.error());
        }
        co_return std::make_shared<node_source>(std::move(location));
    }
    auto opened = co_await store::open(location);
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    co_return std::make_shared<local_source>(std::move(*opened));
}

std::shared_ptr<log_verifier> make_log_verifier(admission_log_policy policy,
                                                std::filesystem::path cache) {
    return std::make_shared<checkpoint_log_verifier>(std::move(policy), std::move(cache));
}

} // namespace liquid::admission_detail
