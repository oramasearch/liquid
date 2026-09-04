#include "capture/coalesce.hpp"

#include <algorithm>
#include <set>

namespace liquid::capture_detail {

auto is_temporary_path(const std::filesystem::path &path) -> bool {
    const auto name = path.filename().string();
    return name.ends_with(".tmp") || name.ends_with(".swp") || name.ends_with("~") ||
           (name.starts_with(".") && name.find("tmp") != std::string::npos);
}

void coalescer::push(watch_event event) {
    events_.push_back(std::move(event));
}

auto coalescer::take() -> std::vector<watch_event> {
    auto output = std::vector<watch_event>{};
    auto seen = std::set<std::pair<watch_event_kind, std::filesystem::path>>{};
    for (auto &event : events_) {
        if (event.kind == watch_event_kind::write && is_temporary_path(event.path))
            continue;
        if (seen.emplace(event.kind, event.path).second)
            output.push_back(std::move(event));
    }
    events_.clear();
    return output;
}

} // namespace liquid::capture_detail
