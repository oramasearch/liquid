#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"

namespace liquid {

struct LIQUID_EXPORT actor {
    std::string taap_id;
};

class LIQUID_EXPORT signer {
public:
    signer() noexcept;
    ~signer();
    signer(signer&&) noexcept;
    signer& operator=(signer&&) noexcept;
    signer(const signer&) = delete;
    signer& operator=(const signer&) = delete;

    [[nodiscard]] result<std::vector<std::byte>> sign(std::span<const std::byte> payload);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

[[nodiscard]] LIQUID_EXPORT result<bool> verify(
    const actor& by, std::span<const std::byte> payload,
    std::span<const std::byte> signature);

class LIQUID_EXPORT key_provider {
public:
    key_provider() noexcept;
    ~key_provider();
    key_provider(key_provider&&) noexcept;
    key_provider& operator=(key_provider&&) noexcept;
    key_provider(const key_provider&) = delete;
    key_provider& operator=(const key_provider&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace liquid
