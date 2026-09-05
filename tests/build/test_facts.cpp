#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "../temporary_directory.hpp"
#include "build/facts.hpp"
#include "formats/json.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace {

class fact_directory {
public:
    fact_directory()
        : path_(liquid::test::fixture_path("liquid-fact-test-" + std::to_string(process_id()) +
                                           "-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~fact_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    static auto process_id() -> std::uint64_t {
#if defined(_WIN32)
        return 0;
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }

    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto digest_for(std::string_view value) -> liquid::digest {
    const auto bytes =
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    auto digest = liquid::hash(liquid::algorithm::blake3, bytes);
    EXPECT_TRUE(digest);
    return *digest;
}

TEST(BuildFacts, exact_content_key_controls_sharing) {
    fact_directory directory;
    auto base = liquid::build_detail::fact_base{directory.path()};
    ASSERT_TRUE(base.open());
    const auto key = liquid::fact_key{
        digest_for("compiler-a"), digest_for("sysroot-a"), "aarch64-unknown-linux-gnu"};
    auto answer = liquid::fact_answer{.exit = 0};
    answer.variables.insert_or_assign("ac_cv_header_stdint_h", "yes");
    auto stored = base.put("autoconf-cache-v1\nac_cv_header_stdint_h", key, answer);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_EQ(base.matching(key).size(), 1U);

    auto changed_toolchain = key;
    changed_toolchain.toolchain = digest_for("compiler-b");
    EXPECT_TRUE(base.matching(changed_toolchain).empty());
    auto changed_sysroot = key;
    changed_sysroot.sysroot = digest_for("sysroot-b");
    EXPECT_TRUE(base.matching(changed_sysroot).empty());
    auto changed_target = key;
    changed_target.target = "x86_64-unknown-linux-gnu";
    EXPECT_TRUE(base.matching(changed_target).empty());

    auto reopened = liquid::build_detail::fact_base{directory.path()};
    auto reopened_result = reopened.open();
    ASSERT_TRUE(reopened_result) << reopened_result.error().message << ": "
                                 << reopened_result.error().context;
    auto shown = reopened.show(stored->id);
    ASSERT_TRUE(shown);
    EXPECT_EQ(shown->answer.variables.at("ac_cv_header_stdint_h"), "yes");
    auto bundle = reopened.export_bundle();
    ASSERT_TRUE(bundle);
    EXPECT_NE(bundle->find("\"kind\":\"fact-bundle/1\""), std::string::npos);
    EXPECT_NE(bundle->find(liquid::to_string(stored->id)), std::string::npos);
    auto public_list = liquid::runtime::block_on(liquid::build::facts_list(directory.path()));
    ASSERT_TRUE(public_list);
    ASSERT_EQ(public_list->size(), 1U);
    auto public_show =
        liquid::runtime::block_on(liquid::build::facts_show(stored->id, directory.path()));
    ASSERT_TRUE(public_show);
    EXPECT_EQ(public_show->probe, stored->probe);
    auto public_export = liquid::runtime::block_on(liquid::build::facts_export(directory.path()));
    ASSERT_TRUE(public_export);
    EXPECT_NE(public_export->find(liquid::to_string(stored->id)), std::string::npos);
    auto parsed_export = liquid::formats::json::parse(*public_export);
    ASSERT_TRUE(parsed_export) << parsed_export.error().message;
}

} // namespace
