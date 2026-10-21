#include "net/retry_after.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>

namespace liquid::net_detail {
namespace {

// A delay larger than this is clamped: it is far beyond any retry budget, and clamping keeps the
// conversion to milliseconds from overflowing.
constexpr auto maximum_delay = std::chrono::seconds{std::int64_t{1} << 32};

constexpr auto month_names = std::array<std::string_view, 12>{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr auto short_days =
    std::array<std::string_view, 7>{"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
constexpr auto long_days = std::array<std::string_view, 7>{
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

class cursor {
public:
    explicit cursor(std::string_view input) : input_(input) {}

    auto literal(std::string_view expected) -> bool {
        if (!input_.starts_with(expected)) {
            return false;
        }
        input_.remove_prefix(expected.size());
        return true;
    }

    auto one_of(std::span<const std::string_view> names) -> std::optional<unsigned> {
        for (auto index = std::size_t{}; index < names.size(); ++index) {
            if (literal(names[index])) {
                return static_cast<unsigned>(index);
            }
        }
        return std::nullopt;
    }

    // Exactly `digits` decimal digits.
    auto number(std::size_t digits) -> std::optional<unsigned> {
        if (input_.size() < digits) {
            return std::nullopt;
        }
        auto output = 0U;
        for (auto index = std::size_t{}; index < digits; ++index) {
            const auto character = input_[index];
            if (character < '0' || character > '9') {
                return std::nullopt;
            }
            output = output * 10U + static_cast<unsigned>(character - '0');
        }
        input_.remove_prefix(digits);
        return output;
    }

    auto done() const -> bool {
        return input_.empty();
    }

private:
    std::string_view input_;
};

struct civil_time {
    int year{};
    unsigned month{};
    unsigned day{};
    unsigned hour{};
    unsigned minute{};
    unsigned second{};
};

// `HH:MM:SS`
auto time_of_day(cursor& input, civil_time& output) -> bool {
    const auto hour = input.number(2);
    if (!hour || !input.literal(":")) {
        return false;
    }
    const auto minute = input.number(2);
    if (!minute || !input.literal(":")) {
        return false;
    }
    const auto second = input.number(2);
    if (!second) {
        return false;
    }
    output.hour = *hour;
    output.minute = *minute;
    output.second = *second;
    return true;
}

// `Sun, 06 Nov 1994 08:49:37 GMT`
auto imf_fixdate(std::string_view value) -> std::optional<civil_time> {
    auto input = cursor{value};
    auto output = civil_time{};
    if (!input.one_of(short_days) || !input.literal(", ")) {
        return std::nullopt;
    }
    const auto day = input.number(2);
    if (!day || !input.literal(" ")) {
        return std::nullopt;
    }
    const auto month = input.one_of(month_names);
    if (!month || !input.literal(" ")) {
        return std::nullopt;
    }
    const auto year = input.number(4);
    if (!year || !input.literal(" ") || !time_of_day(input, output) || !input.literal(" GMT") ||
        !input.done()) {
        return std::nullopt;
    }
    output.year = static_cast<int>(*year);
    output.month = *month + 1;
    output.day = *day;
    return output;
}

// `Sunday, 06-Nov-94 08:49:37 GMT`
auto rfc850_date(std::string_view value, int current_year) -> std::optional<civil_time> {
    auto input = cursor{value};
    auto output = civil_time{};
    if (!input.one_of(long_days) || !input.literal(", ")) {
        return std::nullopt;
    }
    const auto day = input.number(2);
    if (!day || !input.literal("-")) {
        return std::nullopt;
    }
    const auto month = input.one_of(month_names);
    if (!month || !input.literal("-")) {
        return std::nullopt;
    }
    const auto year = input.number(2);
    if (!year || !input.literal(" ") || !time_of_day(input, output) || !input.literal(" GMT") ||
        !input.done()) {
        return std::nullopt;
    }
    // RFC 9110 section 5.6.7: a two-digit year that appears to be more than 50 years in the
    // future is the most recent past year with the same last two digits.
    output.year = current_year - current_year % 100 + static_cast<int>(*year);
    if (output.year > current_year + 50) {
        output.year -= 100;
    }
    output.month = *month + 1;
    output.day = *day;
    return output;
}

// `Sun Nov  6 08:49:37 1994`
auto asctime_date(std::string_view value) -> std::optional<civil_time> {
    auto input = cursor{value};
    auto output = civil_time{};
    if (!input.one_of(short_days) || !input.literal(" ")) {
        return std::nullopt;
    }
    const auto month = input.one_of(month_names);
    if (!month || !input.literal(" ")) {
        return std::nullopt;
    }
    auto day = std::optional<unsigned>{};
    if (input.literal(" ")) {
        day = input.number(1);
    } else {
        day = input.number(2);
    }
    if (!day || !input.literal(" ") || !time_of_day(input, output) || !input.literal(" ")) {
        return std::nullopt;
    }
    const auto year = input.number(4);
    if (!year || !input.done()) {
        return std::nullopt;
    }
    output.year = static_cast<int>(*year);
    output.month = *month + 1;
    output.day = *day;
    return output;
}

auto to_time_point(const civil_time& value)
    -> std::optional<std::chrono::system_clock::time_point> {
    const auto date = std::chrono::year_month_day{std::chrono::year{value.year},
                                                  std::chrono::month{value.month},
                                                  std::chrono::day{value.day}};
    // A leap second (60) is allowed by the grammar; it is read as the end of the minute.
    if (!date.ok() || value.hour > 23 || value.minute > 59 || value.second > 60) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{value.hour} +
           std::chrono::minutes{value.minute} + std::chrono::seconds{value.second};
}

} // namespace

auto parse_retry_after(std::string_view value, std::chrono::system_clock::time_point now)
    -> std::optional<std::chrono::milliseconds> {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.front() >= '0' && value.front() <= '9') {
        auto seconds = std::uint64_t{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        if (parsed.ec == std::errc::result_out_of_range ||
            seconds > static_cast<std::uint64_t>(maximum_delay.count())) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(maximum_delay);
        }
        if (parsed.ec != std::errc{}) {
            return std::nullopt;
        }
        return std::chrono::seconds{static_cast<std::int64_t>(seconds)};
    }
    const auto current_year = static_cast<int>(
        std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(now)}.year());
    auto civil = imf_fixdate(value);
    if (!civil) {
        civil = rfc850_date(value, current_year);
    }
    if (!civil) {
        civil = asctime_date(value);
    }
    if (!civil) {
        return std::nullopt;
    }
    const auto when = to_time_point(*civil);
    if (!when) {
        return std::nullopt;
    }
    if (*when <= now) {
        return std::chrono::milliseconds{0};
    }
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(*when - now);
    return std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(maximum_delay));
}

} // namespace liquid::net_detail
