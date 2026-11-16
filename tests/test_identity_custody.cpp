// Custody of the local identity's signing key at rest (liquid #402): key files are created
// owner-only and atomically, and a new identity's wrapping key is sealed rather than stored in
// clear unless file-permission-only custody is chosen explicitly.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"
#include "temporary_directory.hpp"

#if !defined(_WIN32)

namespace {

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto mode_of(const std::filesystem::path& path) -> unsigned {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
        return 0;
    }
    return static_cast<unsigned>(status.st_mode & 07777U);
}

// Sets LIQUID_HOME and the custody variables for one test and restores them afterwards.
class custody_environment {
public:
    custody_environment(std::string_view name, std::string_view custody)
        : home_(liquid::test::fixture_path("liquid-identity-custody-" + std::string{name})) {
        remember("LIQUID_HOME");
        remember("LIQUID_IDENTITY_KEY_CUSTODY");
        remember("LIQUID_IDENTITY_PASSPHRASE_FILE");
        std::error_code code;
        std::filesystem::remove_all(home_, code);
        std::filesystem::create_directories(home_, code);
        setenv("LIQUID_HOME", home_.c_str(), 1);
        setenv("LIQUID_IDENTITY_KEY_CUSTODY", std::string{custody}.c_str(), 1);
        unsetenv("LIQUID_IDENTITY_PASSPHRASE_FILE");
        liquid::set_identity_custody_options({});
    }

    ~custody_environment() {
        liquid::set_identity_custody_options({});
        for (const auto& [name, value] : saved_) {
            if (value) {
                setenv(name.c_str(), value->c_str(), 1);
            } else {
                unsetenv(name.c_str());
            }
        }
        std::error_code code;
        std::filesystem::remove_all(home_, code);
    }

    custody_environment(const custody_environment&) = delete;
    auto operator=(const custody_environment&) -> custody_environment& = delete;

    [[nodiscard]] auto identity() const -> std::filesystem::path {
        return home_ / "identity";
    }

    [[nodiscard]] auto home() const -> const std::filesystem::path& {
        return home_;
    }

    // Writes `passphrase` to a file beside the identity directory and selects it.
    auto use_passphrase(std::string_view passphrase,
                        std::filesystem::perms mode = std::filesystem::perms::owner_read |
                                                      std::filesystem::perms::owner_write) -> void {
        const auto path = home_ / "passphrase";
        {
            auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
            output << passphrase << '\n';
        }
        std::filesystem::permissions(path, mode, std::filesystem::perm_options::replace);
        setenv("LIQUID_IDENTITY_PASSPHRASE_FILE", path.c_str(), 1);
    }

private:
    auto remember(const char* name) -> void {
        const auto* value = std::getenv(name);
        saved_.emplace_back(name, value ? std::optional<std::string>{value} : std::nullopt);
    }

    std::filesystem::path home_;
    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

auto open_local() -> liquid::result<liquid::signer> {
    return liquid::runtime::block_on(liquid::identity_detail::local_signer());
}

auto expect_signs_and_verifies(liquid::signer& signer) -> void {
    const auto body = bytes("custody body");
    const auto signed_value = liquid::runtime::block_on(signer.sign("drip", body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    const auto valid = liquid::verify(signer.identity(), "drip", body, *signed_value);
    ASSERT_TRUE(valid) << valid.error().message;
    EXPECT_TRUE(*valid);
}

// Every regular file under `root`, by path.
auto regular_files(const std::filesystem::path& root) -> std::vector<std::filesystem::path> {
    auto output = std::vector<std::filesystem::path>{};
    std::error_code code;
    for (auto entry = std::filesystem::recursive_directory_iterator{root, code};
         !code && entry != std::filesystem::recursive_directory_iterator{};
         entry.increment(code)) {
        if (entry->is_regular_file(code)) {
            output.push_back(entry->path());
        }
    }
    return output;
}

} // namespace

// With a umask that would leave files world-readable, a watcher stats every file in the identity
// directory while the identity is created. No key file may ever be seen with a mode other than
// 0600, the final files are 0600, the directory is 0700, and no temporary file is left behind.
TEST(IdentityCustody, KeyFilesNeverExistWithAWiderMode) {
    auto environment = custody_environment{"modes", "file"};
    const auto previous_umask = ::umask(0);
    auto done = std::atomic<bool>{false};
    auto widest = std::atomic<unsigned>{0};
    auto observed = std::atomic<std::size_t>{0};
    auto watcher = std::thread{[&] {
        while (!done.load()) {
            for (const auto& path : regular_files(environment.identity())) {
                const auto mode = mode_of(path);
                if (mode == 0) {
                    continue;
                }
                observed.fetch_add(1);
                if ((mode & 0177U) != 0) {
                    widest.store(mode);
                }
            }
        }
    }};
    auto signer = open_local();
    done.store(true);
    watcher.join();
    ::umask(previous_umask);
    ASSERT_TRUE(signer) << signer.error().message;
    EXPECT_EQ(widest.load(), 0U) << "a key file was visible with mode " << std::oct
                                 << widest.load();

    const auto files = regular_files(environment.identity());
    ASSERT_FALSE(files.empty());
    auto names = std::vector<std::string>{};
    for (const auto& path : files) {
        EXPECT_EQ(mode_of(path), 0600U) << path;
        EXPECT_EQ(path.filename().string().find(".tmp-"), std::string::npos)
            << "temporary file left behind: " << path;
        names.push_back(path.filename().string());
    }
    EXPECT_NE(std::ranges::find(names, ".wrapping.key"), names.end());
    EXPECT_NE(std::ranges::find(names, signer->identity().fingerprint + ".key"), names.end());
    EXPECT_EQ(mode_of(environment.identity()), 0700U);
    expect_signs_and_verifies(*signer);
}

TEST(IdentityCustody, FileCustodyIsReportedAsFilePermissionsOnly) {
    auto environment = custody_environment{"file", "file"};
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;
    const auto status = liquid::local_identity_custody();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->custody, "file");
    EXPECT_EQ(status->kind, "file");
    EXPECT_TRUE(status->file_permissions_only);
    EXPECT_TRUE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.sealed"));
}

// A new identity under passphrase custody never writes its wrapping key in clear: only the sealed
// form exists, it reopens with the passphrase, and a wrong or missing passphrase fails.
TEST(IdentityCustody, PassphraseCustodyStoresNoClearWrappingKey) {
    auto environment = custody_environment{"passphrase", "passphrase"};
    environment.use_passphrase("correct horse battery staple");
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;
    const auto sealed = environment.identity() / ".wrapping.sealed";
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    ASSERT_TRUE(std::filesystem::exists(sealed));
    EXPECT_EQ(mode_of(sealed), 0600U);
    const auto status = liquid::local_identity_custody();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->custody, "passphrase");
    EXPECT_EQ(status->kind, "passphrase");
    EXPECT_FALSE(status->file_permissions_only);
    EXPECT_FALSE(status->clear_copy_present);
    expect_signs_and_verifies(*signer);
    const auto fingerprint = signer->identity().key_fingerprint;

    auto reopened = open_local();
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->identity().key_fingerprint, fingerprint);
    expect_signs_and_verifies(*reopened);

    environment.use_passphrase("wrong horse battery staple");
    const auto wrong = open_local();
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, liquid::errc::unauthorized);

    unsetenv("LIQUID_IDENTITY_PASSPHRASE_FILE");
    const auto missing = open_local();
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::unauthorized);
}

TEST(IdentityCustody, PassphraseFileMustBeOwnerOnly) {
    auto environment = custody_environment{"passphrase-mode", "passphrase"};
    environment.use_passphrase("readable by the group",
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read);
    const auto refused = open_local();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.sealed"));
}

// A headless host (a Lake pod) names its passphrase file through the library option instead of
// the environment. The option wins over LIQUID_IDENTITY_PASSPHRASE_FILE and over a prompt, and the
// same owner-only rule applies to it.
TEST(IdentityCustody, PassphraseFileOptionCreatesAHeadlessIdentity) {
    auto environment = custody_environment{"passphrase-option", "passphrase"};
    const auto path = environment.home() / "pod-passphrase";
    {
        auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
        output << "mounted from a secret\n";
    }
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace);
    environment.use_passphrase("not the option's passphrase");
    auto asked = false;
    liquid::set_identity_custody_options(
        {.custody = {},
         .passphrase = [&asked](bool) -> liquid::result<std::string> {
             asked = true;
             return std::string{"from a prompt"};
         },
         .passphrase_file = path});
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;
    EXPECT_FALSE(asked);
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    EXPECT_TRUE(std::filesystem::exists(environment.identity() / ".wrapping.sealed"));
    expect_signs_and_verifies(*signer);

    // Reopening reads the same file; with only the environment's file it is the wrong passphrase.
    auto reopened = open_local();
    ASSERT_TRUE(reopened) << reopened.error().message;
    liquid::set_identity_custody_options({});
    const auto wrong = open_local();
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, liquid::errc::unauthorized);
}

// A symbolic link to an owner-only file is followed, as a Kubernetes Secret volume needs; the
// checks apply to the file it reaches, so a link to a group-readable file is refused.
TEST(IdentityCustody, PassphraseFileLinkIsCheckedAtItsTarget) {
    auto environment = custody_environment{"passphrase-link", "passphrase"};
    environment.use_passphrase("behind a link");
    const auto target = environment.home() / "passphrase";
    const auto link = environment.home() / "passphrase-link";
    std::filesystem::create_symlink(target, link);
    setenv("LIQUID_IDENTITY_PASSPHRASE_FILE", link.c_str(), 1);
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;

    std::filesystem::permissions(target,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace);
    const auto refused = open_local();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}

// Only a regular file is accepted: a directory, a missing path and an oversized file are refused
// and no identity is created.
TEST(IdentityCustody, PassphraseFileMustBeASmallRegularFile) {
    auto environment = custody_environment{"passphrase-kind", "passphrase"};
    const auto directory = environment.home() / "a-directory";
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(
        directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    setenv("LIQUID_IDENTITY_PASSPHRASE_FILE", directory.c_str(), 1);
    auto refused = open_local();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);

    setenv("LIQUID_IDENTITY_PASSPHRASE_FILE", (environment.home() / "missing").c_str(), 1);
    refused = open_local();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);

    environment.use_passphrase(std::string(65 * 1024, 'x'));
    refused = open_local();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.sealed"));
}

// The library asks an injected passphrase source when no passphrase file is set: once with
// confirmation when the identity is created, and not again for a rotation by the same signer.
TEST(IdentityCustody, InjectedPassphraseSourceIsAskedOnce) {
    auto environment = custody_environment{"callback", "passphrase"};
    auto asked = std::vector<bool>{};
    liquid::set_identity_custody_options(
        {.custody = {}, .passphrase = [&asked](bool confirm) -> liquid::result<std::string> {
             asked.push_back(confirm);
             return std::string{"from the injected source"};
         }});
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;
    ASSERT_EQ(asked.size(), 1U);
    EXPECT_TRUE(asked.front());
    const auto rotated = liquid::runtime::block_on(signer->rotate());
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(asked.size(), 1U);
    auto reopened = open_local();
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(asked.size(), 2U);
    EXPECT_FALSE(asked.back());
    EXPECT_EQ(reopened->identity().key_fingerprint, rotated->subject.key_fingerprint);
    expect_signs_and_verifies(*reopened);
}

// An identity created before #402 (or with file custody) migrates to a sealed wrapping key without
// changing its signing key, and the clear copy is removed.
TEST(IdentityCustody, FileCustodyMigratesToPassphrase) {
    auto environment = custody_environment{"migrate", "file"};
    auto signer = open_local();
    ASSERT_TRUE(signer) << signer.error().message;
    const auto identity = signer->identity();
    ASSERT_TRUE(std::filesystem::exists(environment.identity() / ".wrapping.key"));

    environment.use_passphrase("migrated passphrase");
    const auto migrated =
        liquid::runtime::block_on(liquid::migrate_local_identity_custody("passphrase"));
    ASSERT_TRUE(migrated) << migrated.error().message;
    EXPECT_EQ(migrated->kind, "passphrase");
    EXPECT_FALSE(migrated->file_permissions_only);
    EXPECT_FALSE(migrated->clear_copy_present);
    EXPECT_FALSE(std::filesystem::exists(environment.identity() / ".wrapping.key"));
    EXPECT_EQ(mode_of(environment.identity() / ".wrapping.sealed"), 0600U);

    auto reopened = open_local();
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->identity().taap_id, identity.taap_id);
    EXPECT_EQ(reopened->identity().key_fingerprint, identity.key_fingerprint);
    expect_signs_and_verifies(*reopened);

    environment.use_passphrase("not the migrated passphrase");
    EXPECT_FALSE(open_local());
}

#endif
