#include "flow_control.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <utility>

namespace liquid::flow_detail {
namespace {

auto flow_error(std::string scope, std::string key, std::uint64_t retry_after_ms) -> error {
    return {errc::busy,
            "write flow limit exhausted",
            std::format("scope={} key={} retry_after_ms={}", scope, key, retry_after_ms)};
}

auto retry_after(const controller::bucket& value, std::chrono::steady_clock::time_point now)
    -> std::uint64_t {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - value.refilled_at).count();
    const auto within_second =
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)) % 1000;
    return 1000 - within_second;
}

auto bucket_status(flow_scope scope, const std::string& key, const controller::bucket& value)
    -> flow_bucket_status {
    return {.scope = scope,
            .key = key,
            .tokens = value.tokens,
            .rate = value.config.rate_per_s,
            .burst = value.config.burst,
            .rejected = value.rejected};
}

} // namespace

auto validate(const flow_config& config) -> result<void> {
    const auto valid_bucket = [](const token_bucket_config& bucket) {
        return bucket.rate_per_s > 0 && bucket.burst > 0;
    };
    if (!valid_bucket(config.per_actor) || !valid_bucket(config.per_document) ||
        !valid_bucket(config.per_repository)) {
        return std::unexpected(error{
            errc::invalid_argument, "flow bucket rate and burst must be positive", "config.flow"});
    }
    for (const auto& [actor, bucket] : config.actor_overrides) {
        if (actor.empty() || !valid_bucket(bucket)) {
            return std::unexpected(error{errc::invalid_argument,
                                         "flow actor override must have an id, rate, and burst",
                                         "config.flow.actor_overrides"});
        }
    }
    if (config.max_update_bytes == 0 || config.max_unacknowledged == 0 || config.pause_above == 0 ||
        config.resume_below >= config.pause_above) {
        return std::unexpected(
            error{errc::invalid_argument, "flow limits and watermarks are invalid", "config.flow"});
    }
    return {};
}

auto node_key(const node_id& node) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(node.size() * 2);
    for (const auto value : node) {
        const auto byte = std::to_integer<unsigned char>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

controller::controller(flow_config config)
    : config_(std::move(config)), repository_{config_.per_repository,
                                              static_cast<double>(config_.per_repository.burst),
                                              std::chrono::steady_clock::now()} {}

auto controller::actor_bucket(const std::string& actor) -> bucket& {
    auto found = actors_.find(actor);
    if (found != actors_.end()) {
        return found->second;
    }
    const auto override = config_.actor_overrides.find(actor);
    const auto selected =
        override == config_.actor_overrides.end() ? config_.per_actor : override->second;
    return actors_
        .emplace(
            actor,
            bucket{selected, static_cast<double>(selected.burst), std::chrono::steady_clock::now()})
        .first->second;
}

auto controller::document_bucket(const std::string& document) -> bucket& {
    auto found = documents_.find(document);
    if (found != documents_.end()) {
        return found->second;
    }
    return documents_
        .emplace(document,
                 bucket{config_.per_document,
                        static_cast<double>(config_.per_document.burst),
                        std::chrono::steady_clock::now()})
        .first->second;
}

auto controller::refill(bucket& value, std::chrono::steady_clock::time_point now) -> void {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - value.refilled_at);
    if (elapsed.count() == 0) {
        return;
    }
    value.tokens =
        std::min(static_cast<double>(value.config.burst),
                 value.tokens + static_cast<double>(elapsed.count()) * value.config.rate_per_s);
    value.refilled_at += elapsed;
}

auto controller::admit(std::string actor,
                       std::string document,
                       std::uint64_t update_bytes,
                       std::uint64_t unacknowledged) -> result<void> {
    if (update_bytes > config_.max_update_bytes) {
        return std::unexpected(
            error{errc::invalid_argument,
                  "write update exceeds max_update_bytes",
                  std::format("limit={} update_bytes={}", config_.max_update_bytes, update_bytes)});
    }
    std::lock_guard lock(mutex_);
    if (unacknowledged >= config_.max_unacknowledged) {
        ++repository_.rejected;
        return std::unexpected(flow_error("repository", "unacknowledged", 1));
    }
    if (pause_reason_ != flow_pause_reason::none) {
        ++repository_.rejected;
        const auto reason =
            pause_reason_ == flow_pause_reason::persistence ? "persistence" : "consumers";
        return std::unexpected(error{
            errc::busy,
            "writes are paused above the flow watermark",
            std::format("scope=repository key=repository retry_after_ms=1 paused_for={}", reason)});
    }
    auto& actor_value = actor_bucket(actor);
    auto& document_value = document_bucket(document);
    const auto now = std::chrono::steady_clock::now();
    refill(actor_value, now);
    refill(document_value, now);
    refill(repository_, now);
    if (actor_value.tokens < 1.0) {
        ++actor_value.rejected;
        return std::unexpected(flow_error("actor", actor, retry_after(actor_value, now)));
    }
    if (document_value.tokens < 1.0) {
        ++document_value.rejected;
        return std::unexpected(flow_error("document", document, retry_after(document_value, now)));
    }
    if (repository_.tokens < 1.0) {
        ++repository_.rejected;
        return std::unexpected(
            flow_error("repository", "repository", retry_after(repository_, now)));
    }
    actor_value.tokens -= 1.0;
    document_value.tokens -= 1.0;
    repository_.tokens -= 1.0;
    return {};
}

auto controller::status() const -> std::vector<flow_bucket_status> {
    std::lock_guard lock(mutex_);
    auto output = std::vector<flow_bucket_status>{};
    output.reserve(actors_.size() + documents_.size() + 1);
    for (const auto& [key, value] : actors_) {
        output.push_back(bucket_status(flow_scope::actor, key, value));
    }
    for (const auto& [key, value] : documents_) {
        output.push_back(bucket_status(flow_scope::document, key, value));
    }
    output.push_back(bucket_status(flow_scope::repository, "repository", repository_));
    return output;
}

const flow_config& controller::config() const noexcept {
    return config_;
}

auto controller::set_pause(flow_pause_reason reason) -> void {
    std::lock_guard lock(mutex_);
    pause_reason_ = reason;
}

auto shared_controller(std::string repository_key, flow_config config)
    -> std::shared_ptr<controller> {
    static auto mutex = std::mutex{};
    static auto controllers = std::map<std::string, std::weak_ptr<controller>>{};
    auto path = std::filesystem::path{repository_key};
    auto code = std::error_code{};
    const auto normalized =
        std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(path), code), code);
    if (!code) {
        repository_key = normalized.generic_string();
    }
    std::lock_guard lock(mutex);
    if (const auto found = controllers.find(repository_key); found != controllers.end()) {
        if (auto existing = found->second.lock()) {
            return existing;
        }
    }
    auto created = std::make_shared<controller>(std::move(config));
    controllers.insert_or_assign(std::move(repository_key), created);
    return created;
}

} // namespace liquid::flow_detail
