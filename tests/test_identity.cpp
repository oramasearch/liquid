#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

#include "liquid/identity.hpp"
#include "unsupported.hpp"

TEST(Identity, FacadeNamesOwningIssue) {
    static_assert(sizeof(liquid::signer) == sizeof(void*));
    liquid::signer signer;
    liquid::passphrase_provider provider{"test passphrase"};
    const liquid::actor actor{"taap:example"};
    std::array<std::byte, 1> bytes{};
    expect_unsupported(signer.sign({bytes.begin(), bytes.end()}), "liquid #4");
    expect_unsupported(liquid::verify(actor, bytes, bytes), "liquid #4");
}

TEST(Identity, Argon2iDerivationIsDeterministicAndSalted) {
    liquid::passphrase_provider provider{"correct horse battery staple"};
    std::array<std::byte, 16> first_salt{};
    std::array<std::byte, 16> second_salt{};
    second_salt[0] = std::byte{1};
    const liquid::argon2_parameters parameters{8, 3, 1};
    const auto first = provider.derive(first_salt, parameters);
    const auto repeated = provider.derive(first_salt, parameters);
    const auto second = provider.derive(second_salt, parameters);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(repeated.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *repeated);
    EXPECT_NE(*first, *second);
}

TEST(Identity, KeyFileProviderRequiresAnExactPrivateKeyFile) {
    const auto path = std::filesystem::temp_directory_path() / "liquid-key-file-provider-test";
    std::filesystem::remove(path);
    std::array<std::byte, 32> key{};
    key[0] = std::byte{0x42};
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key.size()));
    }
#if !defined(_WIN32)
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    const auto opened = liquid::runtime::block_on(liquid::key_file_provider::open(path));
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
#if !defined(_WIN32)
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::replace);
    const auto exposed = liquid::runtime::block_on(liquid::key_file_provider::open(path));
    ASSERT_FALSE(exposed.has_value());
    EXPECT_EQ(exposed.error().code, liquid::errc::unauthorized);
#endif
    std::filesystem::remove(path);
}
