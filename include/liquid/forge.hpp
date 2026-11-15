// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class forge_review_state : std::uint8_t { open, approved, closed };

struct LIQUID_EXPORT forge_review {
    std::string id;
    change_id change;
    forge_review_state state{forge_review_state::open};
    std::vector<std::string> reviewers;
    std::chrono::sys_seconds created{};
    std::chrono::sys_seconds updated{};
};

struct LIQUID_EXPORT forge_comment {
    std::string id;
    std::string review;
    std::optional<actor> author;
    std::string body;
    text_anchor anchor;
    std::optional<mapped_anchor> mapped;
    std::chrono::sys_seconds created{};
};

struct LIQUID_EXPORT forge_branch_protection {
    std::string pattern;
    bool required_review{};
    bool required_verification{};
    bool fast_forward_only{};
    std::vector<std::string> allowed_pushers;
};

struct LIQUID_EXPORT forge_client_options {
    std::string bearer_token;
    std::chrono::milliseconds poll_interval{2000};
};

enum class forge_event_kind : std::uint8_t { review_updated, comment_added, head_moved };

struct LIQUID_EXPORT forge_event {
    forge_event_kind kind{forge_event_kind::review_updated};
    std::optional<forge_review> review;
    std::optional<forge_comment> comment;
    std::optional<change_id> head;
};

class LIQUID_EXPORT forge_watch {
public:
    forge_watch() noexcept;
    ~forge_watch();
    forge_watch(forge_watch&&) noexcept;
    forge_watch& operator=(forge_watch&&) noexcept;
    forge_watch(const forge_watch&) = delete;
    forge_watch& operator=(const forge_watch&) = delete;

    [[nodiscard]] runtime::task<forge_event> next();

private:
    struct impl;
    std::unique_ptr<impl> impl_;

    explicit forge_watch(std::unique_ptr<impl> implementation) noexcept;
    friend class forge_client;
};

class LIQUID_EXPORT forge_client {
public:
    explicit forge_client(std::string lake_repository_url, forge_client_options options = {});
    ~forge_client();
    forge_client(forge_client&&) noexcept;
    forge_client& operator=(forge_client&&) noexcept;
    forge_client(const forge_client&) = delete;
    forge_client& operator=(const forge_client&) = delete;

    [[nodiscard]] runtime::task<std::vector<forge_review>>
    list_reviews(std::optional<forge_review_state> state = {});
    [[nodiscard]] runtime::task<forge_review> get_review(std::string id);
    [[nodiscard]] runtime::task<forge_review>
    create_review(change_id change, std::vector<std::string> reviewers = {});
    [[nodiscard]] runtime::task<forge_review> set_review_state(std::string id,
                                                               forge_review_state state);
    [[nodiscard]] runtime::task<std::vector<forge_comment>> list_comments(std::string review);
    [[nodiscard]] runtime::task<forge_comment>
    create_comment(std::string review, std::string body, text_anchor anchor);
    [[nodiscard]] runtime::task<std::vector<forge_comment>>
    advance_anchors(repository& repository, std::string review, change_id target);
    [[nodiscard]] runtime::task<std::vector<forge_branch_protection>> branch_protections();
    [[nodiscard]] runtime::task<forge_watch> watch(std::string review);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
