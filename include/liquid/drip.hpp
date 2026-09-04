#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct LIQUID_EXPORT drip_id {
    digest value;
};

struct LIQUID_EXPORT path_op {
    std::string path;
};

struct LIQUID_EXPORT drip {
    drip_id id;
    change_id base;
    std::vector<path_op> ops;
    actor by;
    std::vector<std::byte> signature;
    std::chrono::sys_seconds at;
};

enum class capture_status : std::uint8_t { running, paused };

class LIQUID_EXPORT subscription {
public:
    subscription() noexcept;
    ~subscription();
    subscription(subscription&&) noexcept;
    subscription& operator=(subscription&&) noexcept;
    subscription(const subscription&) = delete;
    subscription& operator=(const subscription&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT capture {
public:
    capture() noexcept;
    ~capture();
    capture(capture&&) noexcept;
    capture& operator=(capture&&) noexcept;
    capture(const capture&) = delete;
    capture& operator=(const capture&) = delete;

    [[nodiscard]] result<subscription> subscribe(std::function<void(const drip&)> callback);
    [[nodiscard]] runtime::task<void> pause();
    [[nodiscard]] runtime::task<void> resume();
    [[nodiscard]] result<capture_status> status();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
