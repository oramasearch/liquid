#pragma once

#include <memory>
#include <utility>

#include "liquid/build.hpp"

namespace liquid::build_detail {

struct event_state;

class event_producer {
public:
    event_producer() = default;

    LIQUID_EXPORT void publish(build_event event) const;
    LIQUID_EXPORT void finish(build_summary summary) const;
    LIQUID_EXPORT void fail(error failure, build_summary summary) const;
    [[nodiscard]] LIQUID_EXPORT runtime::task<void> await_observer() const;

private:
    explicit event_producer(std::shared_ptr<event_state> state) : state_(std::move(state)) {}

    std::shared_ptr<event_state> state_;
    friend struct event_access;
};

struct started_event_stream {
    build_handle handle;
    event_producer producer;
};

struct event_access {
    [[nodiscard]] LIQUID_EXPORT static auto create() -> started_event_stream;
};

} // namespace liquid::build_detail
