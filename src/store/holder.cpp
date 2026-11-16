#include "holder.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace liquid::store_detail {
namespace {

constexpr std::string_view holder_prefix = "holder ";

std::string host_name() {
#if defined(_WIN32)
    std::array<char, MAX_COMPUTERNAME_LENGTH + 1> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (GetComputerNameA(name.data(), &size) == 0)
        return "unknown";
    return std::string(name.data(), size);
#else
    std::array<char, 256> name{};
    if (::gethostname(name.data(), name.size() - 1) != 0)
        return "unknown";
    return std::string(name.data());
#endif
}

std::uint64_t current_pid() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// The start time of process `pid`, or nothing when no such process exists.
std::optional<std::string> start_time(std::uint64_t pid) {
#if defined(__APPLE__)
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc information{};
    auto size = sizeof(information);
    if (::sysctl(name, 4, &information, &size, nullptr, 0) != 0 || size == 0)
        return std::nullopt;
    const auto& started = information.kp_proc.p_starttime;
    return std::to_string(started.tv_sec) + "." + std::to_string(started.tv_usec);
#elif defined(_WIN32)
    const auto process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return std::nullopt;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    DWORD code = 0;
    const auto timed = GetProcessTimes(process, &created, &exited, &kernel, &user) != 0;
    const auto running = GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(process);
    if (!timed || !running)
        return std::nullopt;
    const auto value =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
    return std::to_string(value);
#else
    // Field 22 of /proc/<pid>/stat: clock ticks since boot. The command name (field 2) may hold
    // spaces and parentheses, so the fields are counted after its last ')'.
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    if (!input)
        return std::nullopt;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto name_end = text.rfind(')');
    if (name_end == std::string::npos)
        return std::nullopt;
    auto field = 2;
    auto at = name_end + 1;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ')
            ++at;
        const auto end = text.find(' ', at);
        ++field;
        if (field == 22)
            return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (end == std::string::npos)
            break;
        at = end;
    }
    return std::nullopt;
#endif
}

} // namespace

std::string process_holder() {
    static const auto holder = [] {
        const auto pid = current_pid();
        return std::string{holder_prefix} + host_name() + " " + std::to_string(pid) + " " +
               start_time(pid).value_or("0");
    }();
    return holder;
}

holder_state lease_holder_state(std::string_view owner) {
    const auto line_start = owner.rfind('\n');
    const auto line = line_start == std::string_view::npos ? owner : owner.substr(line_start + 1);
    if (!line.starts_with(holder_prefix))
        return holder_state::unknown;
    const auto fields = line.substr(holder_prefix.size());
    const auto first = fields.find(' ');
    const auto second = first == std::string_view::npos ? first : fields.find(' ', first + 1);
    if (second == std::string_view::npos)
        return holder_state::unknown;
    const auto host = fields.substr(0, first);
    const auto pid_text = fields.substr(first + 1, second - first - 1);
    const auto start = fields.substr(second + 1);
    if (host != host_name())
        return holder_state::unknown;
    auto pid = std::uint64_t{};
    const auto [end, code] =
        std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
    if (code != std::errc{} || end != pid_text.data() + pid_text.size() || pid == 0)
        return holder_state::unknown;
    if (start == "0")
        return holder_state::unknown;
    const auto running = start_time(pid);
    if (!running || *running != start)
        return holder_state::dead;
    return holder_state::alive;
}

} // namespace liquid::store_detail
