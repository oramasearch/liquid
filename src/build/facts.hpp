// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"

namespace liquid::build_detail {

class LIQUID_EXPORT fact_base {
public:
    explicit fact_base(std::filesystem::path directory);

    [[nodiscard]] result<void> open();
    [[nodiscard]] result<configure_fact> put(std::string probe, fact_key key, fact_answer answer);
    [[nodiscard]] const std::vector<configure_fact>& facts() const noexcept;
    [[nodiscard]] result<configure_fact> show(digest id) const;
    [[nodiscard]] std::vector<configure_fact> matching(const fact_key& key) const;
    [[nodiscard]] result<std::string> export_bundle() const;

private:
    std::filesystem::path directory_;
    std::vector<configure_fact> facts_;

    [[nodiscard]] result<void> write_index() const;
};

[[nodiscard]] result<std::filesystem::path>
default_fact_directory(const std::filesystem::path& working_tree);
[[nodiscard]] result<fact_key> detect_fact_key(std::optional<std::string> target = std::nullopt);
[[nodiscard]] std::string host_target();
[[nodiscard]] bool foreign_target(const fact_key& key);
[[nodiscard]] bool probe_process(std::span<const std::string> command, std::string_view cwd);
[[nodiscard]] std::string canonical_probe(std::span<const std::string> command,
                                          std::string_view cwd,
                                          std::string_view test_program);
[[nodiscard]] std::map<std::string, std::string> cached_variables(const fact_base& base,
                                                                  const fact_key& key);
[[nodiscard]] result<void> record_configuration_facts(const std::filesystem::path& working_tree,
                                                      const fact_key& key,
                                                      fact_base& base);

} // namespace liquid::build_detail
