#pragma once

#include <memory>
#include <string>

#include "liquid/error.hpp"
#include "liquid/forge.hpp"

namespace liquid::test {

class forge_double {
public:
    explicit forge_double(std::string bearer_token = {},
                          std::string listen = "127.0.0.1:0",
                          std::string certificate = {},
                          std::string private_key = {});
    ~forge_double();
    forge_double(forge_double&&) noexcept;
    forge_double& operator=(forge_double&&) noexcept;
    forge_double(const forge_double&) = delete;
    forge_double& operator=(const forge_double&) = delete;

    [[nodiscard]] auto url() const -> std::string;
    [[nodiscard]] auto last_authorization() const -> std::string;
    [[nodiscard]] auto add_comment(std::string review, std::string body, text_anchor anchor)
        -> std::string;
    [[nodiscard]] auto advance_head(std::string path, std::string contents) -> result<change_id>;
    void add_branch_protection(forge_branch_protection rule);
    void fail_next(std::string code, std::string message, std::string path = {});

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid::test
