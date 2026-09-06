#include "daemon/fanout.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <ranges>
#include <system_error>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/hash.hpp"

namespace liquid::daemon_detail {
namespace {

namespace cbor = encoding::cbor;

auto failure(errc code, std::string message, const std::filesystem::path& path) -> error {
    return {code, std::move(message), path.string()};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* fields = std::get_if<cbor::value::map>(&source.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, value] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto text_field(const cbor::value& source, std::string_view name) -> const std::string* {
    const auto* value = field(source, name);
    return value ? std::get_if<std::string>(&value->data) : nullptr;
}

auto write_atomic(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> result<void> {
    auto temporary = path;
    temporary += ".tmp";
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return std::unexpected(failure(errc::io, "cannot write daemon state", temporary));
        }
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            return std::unexpected(failure(errc::io, "cannot flush daemon state", temporary));
        }
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), path));
    }
    return {};
}

} // namespace

auto cursor_offset(const journal_cursor& cursor) -> result<std::uint64_t> {
    if (cursor.token.empty()) {
        return std::uint64_t{};
    }
    const auto separator = cursor.token.find(':');
    const auto offset_text = std::string_view{cursor.token}.substr(0, separator);
    auto output = std::uint64_t{};
    const auto [end, conversion_error] =
        std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), output);
    if (conversion_error != std::errc{} || end != offset_text.data() + offset_text.size()) {
        return std::unexpected(error{errc::corrupt, "invalid daemon cursor", cursor.token});
    }
    return output;
}

fanout::fanout(std::filesystem::path state_path,
               std::vector<std::shared_ptr<daemon_target>> targets)
    : state_path_(std::move(state_path)), targets_(std::move(targets)) {
    for (const auto& target : targets_) {
        state_.try_emplace(target->name());
    }
}

auto fanout::load() -> result<void> {
    if (!std::filesystem::exists(state_path_)) {
        return {};
    }
    auto input = std::ifstream{state_path_, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read daemon state", state_path_));
    }
    auto encoded = std::vector<std::byte>{};
    for (char value; input.get(value);) {
        encoded.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    if (!input.eof()) {
        return std::unexpected(failure(errc::io, "cannot read daemon state", state_path_));
    }
    auto decoded = cbor::decode(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* version_value = field(*decoded, "v");
    const auto* targets_value = field(*decoded, "targets");
    const auto* version =
        version_value ? std::get_if<std::uint64_t>(&version_value->data) : nullptr;
    const auto* targets =
        targets_value ? std::get_if<cbor::value::array>(&targets_value->data) : nullptr;
    if (!version || *version != 1 || !targets) {
        return std::unexpected(failure(errc::corrupt, "invalid daemon state", state_path_));
    }
    for (const auto& item : *targets) {
        const auto* name_value = field(item, "name");
        const auto* cursor_value = field(item, "cursor");
        const auto* deliveries_value = field(item, "deliveries");
        const auto* name = name_value ? std::get_if<std::string>(&name_value->data) : nullptr;
        const auto* cursor = cursor_value ? std::get_if<std::string>(&cursor_value->data) : nullptr;
        const auto* deliveries =
            deliveries_value ? std::get_if<std::uint64_t>(&deliveries_value->data) : nullptr;
        if (!name || !cursor || !deliveries) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon target", state_path_));
        }
        const auto found = state_.find(*name);
        if (found != state_.end()) {
            found->second.cursor.token = *cursor;
            found->second.deliveries = *deliveries;
        }
    }
    if (const auto* cursor = text_field(*decoded, "last_cursor")) {
        last_cursor_.token = *cursor;
    }
    if (const auto* drip_value = text_field(*decoded, "last_drip");
        drip_value && !drip_value->empty()) {
        auto parsed = parse_digest(*drip_value);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        last_drip_ = drip_id{*parsed};
    }
    if (const auto* paths_value = field(*decoded, "invalidated")) {
        const auto* paths = std::get_if<cbor::value::array>(&paths_value->data);
        if (!paths) {
            return std::unexpected(failure(errc::corrupt, "invalid daemon paths", state_path_));
        }
        for (const auto& item : *paths) {
            const auto* path = std::get_if<std::string>(&item.data);
            if (!path) {
                return std::unexpected(failure(errc::corrupt, "invalid daemon path", state_path_));
            }
            invalidated_paths_.push_back(*path);
        }
    }
    return {};
}

runtime::task<void> fanout::deliver(daemon_delivery delivery) {
    auto incoming = cursor_offset(delivery.cursor);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    const auto last_offset = cursor_offset(last_cursor_).value_or(0);
    if (*incoming > last_offset) {
        std::lock_guard lock(mutex_);
        invalidated_paths_.clear();
        for (const auto& operation : delivery.value.ops) {
            invalidated_paths_.push_back(operation.path);
        }
        std::ranges::sort(invalidated_paths_);
        const auto unique_end = std::ranges::unique(invalidated_paths_).begin();
        invalidated_paths_.erase(unique_end, invalidated_paths_.end());
        last_drip_ = delivery.value.id;
        last_cursor_ = delivery.cursor;
    }
    for (const auto& target : targets_) {
        auto current_cursor = journal_cursor{};
        {
            std::lock_guard lock(mutex_);
            current_cursor = state_.at(target->name()).cursor;
        }
        auto current = cursor_offset(current_cursor);
        if (!current) {
            co_return std::unexpected(current.error());
        }
        if (*incoming <= *current) {
            continue;
        }
        auto delivered = co_await target->deliver(delivery);
        if (!delivered) {
            {
                std::lock_guard lock(mutex_);
                state_.at(target->name()).last_error = delivered.error().message;
            }
            auto saved = co_await runtime::offload([this] { return persist(); });
            if (!saved) {
                co_return std::unexpected(saved.error());
            }
            if (!*saved) {
                co_return std::unexpected(saved->error());
            }
            co_return std::unexpected(delivered.error());
        }
        {
            std::lock_guard lock(mutex_);
            auto& target_state = state_.at(target->name());
            target_state.cursor = delivery.cursor;
            ++target_state.deliveries;
            target_state.last_error.reset();
        }
        auto saved = co_await runtime::offload([this] { return persist(); });
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        if (!*saved) {
            co_return std::unexpected(saved->error());
        }
    }
    co_return {};
}

auto fanout::target_status(const journal_cursor& capture_cursor) const
    -> std::vector<daemon_target_status> {
    std::lock_guard lock(mutex_);
    auto capture_end = cursor_offset(capture_cursor).value_or(0);
    auto output = std::vector<daemon_target_status>{};
    output.reserve(targets_.size());
    for (const auto& target : targets_) {
        const auto& value = state_.at(target->name());
        const auto target_end = cursor_offset(value.cursor).value_or(0);
        output.push_back({.name = target->name(),
                          .cursor = value.cursor,
                          .behind = capture_end > target_end ? capture_end - target_end : 0,
                          .deliveries = value.deliveries,
                          .last_error = value.last_error,
                          .detail = target->detail()});
    }
    return output;
}

auto fanout::replay_cursor() const -> journal_cursor {
    std::lock_guard lock(mutex_);
    auto output = journal_cursor{};
    auto minimum = std::numeric_limits<std::uint64_t>::max();
    for (const auto& target : targets_) {
        const auto& value = state_.at(target->name());
        const auto offset = cursor_offset(value.cursor).value_or(0);
        if (offset < minimum) {
            minimum = offset;
            output = value.cursor;
        }
    }
    return output;
}

auto fanout::last_drip() const -> std::optional<drip_id> {
    std::lock_guard lock(mutex_);
    return last_drip_;
}

auto fanout::invalidated_paths() const -> std::vector<std::string> {
    std::lock_guard lock(mutex_);
    return invalidated_paths_;
}

auto fanout::persist() const -> result<void> {
    std::lock_guard lock(mutex_);
    auto targets = cbor::value::array{};
    targets.reserve(targets_.size());
    for (const auto& target : targets_) {
        const auto& value = state_.at(target->name());
        targets.push_back(map({{"name", cbor::value{target->name()}},
                               {"cursor", cbor::value{value.cursor.token}},
                               {"deliveries", cbor::value{value.deliveries}}}));
    }
    auto invalidated = cbor::value::array{};
    for (const auto& path : invalidated_paths_) {
        invalidated.emplace_back(path);
    }
    auto encoded = cbor::encode(
        map({{"v", cbor::value{std::uint64_t{1}}},
             {"targets", cbor::value{std::move(targets)}},
             {"last_cursor", cbor::value{last_cursor_.token}},
             {"last_drip", cbor::value{last_drip_ ? to_string(last_drip_->value) : ""}},
             {"invalidated", cbor::value{std::move(invalidated)}}}));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return write_atomic(state_path_, *encoded);
}

} // namespace liquid::daemon_detail
