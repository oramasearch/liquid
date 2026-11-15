#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/identity.hpp"
#include "temporary_directory.hpp"

namespace {

// Points `LIQUID_HOME` at a fresh directory for one test and restores it afterwards.
class trust_home {
public:
    explicit trust_home(std::string_view name)
        : path_(liquid::test::fixture_path("liquid-trust-" + std::string{name})) {
        if (const auto* previous = std::getenv("LIQUID_HOME")) {
            previous_ = previous;
        }
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
        EXPECT_FALSE(code) << code.message();
        set(path_.string());
    }
    trust_home(const trust_home&) = delete;
    auto operator=(const trust_home&) -> trust_home& = delete;
    ~trust_home() {
        if (previous_) {
            set(*previous_);
        } else {
#if defined(_WIN32)
            static_cast<void>(_putenv_s("LIQUID_HOME", ""));
#else
            static_cast<void>(unsetenv("LIQUID_HOME"));
#endif
        }
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    auto write(std::string_view name, std::string_view contents) const -> void {
        std::error_code code;
        std::filesystem::create_directories(path_ / "trust", code);
        ASSERT_FALSE(code) << code.message();
        auto output = std::ofstream{path_ / "trust" / std::string{name}, std::ios::trunc};
        output << contents;
        output.close();
        ASSERT_TRUE(output);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    static auto set(const std::string& value) -> void {
#if defined(_WIN32)
        EXPECT_EQ(_putenv_s("LIQUID_HOME", value.c_str()), 0);
#else
        EXPECT_EQ(setenv("LIQUID_HOME", value.c_str(), 1), 0);
#endif
    }

    std::filesystem::path path_;
    std::optional<std::string> previous_;
};

TEST(TrustFile, PathIsUnderTheLiquidHome) {
    const auto home = trust_home{"path"};
    EXPECT_EQ(liquid::trust_file_path("hot-update-signers"),
              home.path() / "trust" / "hot-update-signers");
}

TEST(TrustFile, MissingFileRefusesAndNamesTheFile) {
    const auto home = trust_home{"missing"};
    auto loaded = liquid::load_trusted_identities("hot-update-signers");
    ASSERT_FALSE(loaded) << "a missing trust file produced a trust list";
    EXPECT_EQ(loaded.error().code, liquid::errc::unauthorized);
    EXPECT_NE(loaded.error().message.find(liquid::trust_file_path("hot-update-signers").string()),
              std::string::npos)
        << loaded.error().message;
}

TEST(TrustFile, EmptyOrCommentOnlyFileRefuses) {
    const auto home = trust_home{"empty"};
    home.write("release-policy-issuers", "");
    auto empty = liquid::load_trusted_identities("release-policy-issuers");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, liquid::errc::unauthorized);

    home.write("release-policy-issuers", "# nobody yet\n\n   \n");
    auto comments = liquid::load_trusted_identities("release-policy-issuers");
    ASSERT_FALSE(comments);
    EXPECT_EQ(comments.error().code, liquid::errc::unauthorized);
}

TEST(TrustFile, ReadsOneIdentityPerLine) {
    const auto home = trust_home{"lines"};
    home.write("hot-update-signers",
               "# release engineering\n"
               "taap:1:ed25519:first\n"
               "  taap:1:ed25519:second  # build machine\r\n"
               "\n"
               "taap:1:ed25519:first\n");
    auto loaded = liquid::load_trusted_identities("hot-update-signers");
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(*loaded, (std::vector<std::string>{"taap:1:ed25519:first", "taap:1:ed25519:second"}));
}

TEST(TrustFile, LineWithTwoValuesIsRefused) {
    const auto home = trust_home{"two-values"};
    home.write("hot-update-signers", "taap:1:ed25519:first taap:1:ed25519:second\n");
    auto loaded = liquid::load_trusted_identities("hot-update-signers");
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, liquid::errc::invalid_argument);
}

TEST(TrustFile, NameMustBeAPlainFileName) {
    const auto home = trust_home{"name"};
    for (const auto* name : {"", ".", "..", "../identity", "nested/file"}) {
        auto loaded = liquid::load_trusted_identities(name);
        ASSERT_FALSE(loaded) << name;
        EXPECT_EQ(loaded.error().code, liquid::errc::invalid_argument) << name;
    }
}

} // namespace
