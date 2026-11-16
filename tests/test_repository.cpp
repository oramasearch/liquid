// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/testing.hpp"
#include "temporary_directory.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(liquid::test::fixture_path("liquid-repository-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    ~temporary_repository() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto content(std::string value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto format_fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_FORMAT_FIXTURES} / name;
    auto input = std::ifstream{path, std::ios::binary};
    EXPECT_TRUE(input) << path;
    const auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>{};
    output.reserve(characters.size());
    for (const auto character : characters) {
        output.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return output;
}

auto physical_copies(const std::filesystem::path& packs, const liquid::digest& id) -> std::size_t {
    auto copies = std::size_t{};
    for (const auto& entry : std::filesystem::directory_iterator(packs)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pack") {
            continue;
        }
        auto input = std::ifstream{entry.path(), std::ios::binary};
        const auto raw = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
        auto offset = std::size_t{16};
        while (offset + 49 <= raw.size() &&
               std::string_view{raw.data() + offset, 7} == "LQFRAM\x01") {
            auto compressed = std::uint32_t{};
            for (auto byte = 0U; byte != 4; ++byte) {
                compressed |=
                    static_cast<std::uint32_t>(static_cast<unsigned char>(raw[offset + 12 + byte]))
                    << (byte * 8U);
            }
            if (compressed > raw.size() - offset - 49) {
                break;
            }
            const auto algorithm = static_cast<unsigned char>(raw[offset + 16]);
            const auto* digest_bytes = reinterpret_cast<const std::byte*>(raw.data() + offset + 17);
            if (algorithm == static_cast<unsigned char>(id.alg) &&
                std::ranges::equal(std::span{digest_bytes, id.bytes.size()}, id.bytes)) {
                ++copies;
            }
            offset += 49 + compressed;
        }
    }
    return copies;
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte>;

auto journal_document(const std::filesystem::path& repository) -> std::vector<std::byte> {
    auto journal = read_file(repository / "LIQUID_JOURNAL");
    const auto prefix = std::string_view{"liquid-journal-v2 "};
    const auto pointer = text(journal);
    if (!pointer.starts_with(prefix)) {
        return journal;
    }
    auto digest_text = pointer.substr(prefix.size());
    if (const auto newline = digest_text.find('\n'); newline != std::string_view::npos) {
        digest_text = digest_text.substr(0, newline);
    }
    auto digest = liquid::parse_digest(digest_text);
    EXPECT_TRUE(digest) << digest.error().message;
    auto store = liquid::runtime::block_on(liquid::store::open(repository / ".liquid-store"));
    EXPECT_TRUE(store) << store.error().message;
    auto document = liquid::runtime::block_on(store->get(*digest));
    EXPECT_TRUE(document) << document.error().message;
    return *document;
}

auto has_change(const liquid::tree_diff_entry& entry, liquid::tree_diff_kind kind) -> bool {
    return std::ranges::find(entry.changes, kind) != entry.changes.end();
}

auto sha256_options() -> liquid::repository_options {
    liquid::repository_options options;
    options.storage.hash_algorithm = liquid::algorithm::sha256;
    return options;
}

auto encrypted_options(const std::shared_ptr<liquid::keyring>& keys) -> liquid::repository_options {
    liquid::repository_options options;
    options.storage.keys = keys;
    return options;
}

auto exclusive_checkout_options(std::string pattern = "**") -> liquid::repository_options {
    auto options = liquid::repository_options{};
    options.locks.rules.push_back({"exclusive-checkout", {std::move(pattern)}});
    return options;
}

auto peak_rss_bytes() -> std::uint64_t {
#if defined(_WIN32)
    return 0;
#else
    auto usage = rusage{};
    const auto result = getrusage(RUSAGE_SELF, &usage);
    EXPECT_EQ(result, 0);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#endif
}

auto wait_for(std::chrono::milliseconds duration) -> liquid::runtime::task<void> {
    auto [delay_error] = co_await liquid::runtime::delay(duration);
    if (delay_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, delay_error.message(), "repository test"});
    }
    co_return {};
}

auto retry_after_ms(std::string_view context) -> std::uint64_t {
    constexpr auto prefix = std::string_view{"retry_after_ms="};
    const auto start = context.find(prefix);
    if (start == std::string_view::npos) {
        return 0;
    }
    auto value = std::uint64_t{};
    const auto number = context.substr(start + prefix.size());
    const auto end = number.find(' ');
    const auto selected = number.substr(0, end);
    const auto parsed = std::from_chars(selected.data(), selected.data() + selected.size(), value);
    return parsed.ec == std::errc{} ? value : 0;
}

class scoped_liquid_home {
public:
    explicit scoped_liquid_home(const std::filesystem::path& path) {
        if (const auto* configured = std::getenv("LIQUID_HOME")) {
            previous_ = configured;
        }
        set(path);
    }

    scoped_liquid_home(const scoped_liquid_home&) = delete;
    auto operator=(const scoped_liquid_home&) -> scoped_liquid_home& = delete;

    ~scoped_liquid_home() {
        if (previous_) {
            static_cast<void>(set_environment(*previous_));
            return;
        }
#ifdef _WIN32
        static_cast<void>(_putenv_s("LIQUID_HOME", ""));
#else
        static_cast<void>(unsetenv("LIQUID_HOME"));
#endif
    }

    auto set(const std::filesystem::path& path) -> void {
        ASSERT_EQ(set_environment(path), 0);
    }

private:
    static auto set_environment(const std::filesystem::path& path) -> int {
#ifdef _WIN32
        return _putenv_s("LIQUID_HOME", path.string().c_str());
#else
        return setenv("LIQUID_HOME", path.string().c_str(), 1);
#endif
    }

    std::optional<std::string> previous_;
};

auto encryption_keys() -> std::shared_ptr<liquid::keyring> {
    auto keys = std::make_shared<liquid::keyring>();
    EXPECT_TRUE(keys->add("repository", std::make_shared<liquid::passphrase_provider>("secret"))
                    .has_value());
    EXPECT_TRUE(keys->set_default_key_id("repository").has_value());
    return keys;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes);

TEST(Repository, restores_many_journaled_outputs_with_bounded_peak_rss) {
    constexpr auto output_count = std::size_t{96};
    constexpr auto output_size = std::size_t{2048};
    constexpr auto rss_ceiling = std::uint64_t{512 * 1024 * 1024};
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.checkpoint_interval = output_count + 1;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    auto payload = std::vector<std::byte>(output_size, std::byte{0x5a});
    for (auto index = std::size_t{}; index != output_count; ++index) {
        auto written = liquid::runtime::block_on(
            repository.write("outputs/" + std::to_string(index) + ".o", payload));
        ASSERT_TRUE(written) << written.error().message;
    }
    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path(), options));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = liquid::runtime::block_on(
        reopened->read("outputs/" + std::to_string(output_count - 1) + ".o"));
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(*restored, payload);
    EXPECT_LT(peak_rss_bytes(), rss_ceiling);
}

TEST(Repository, import_tree_uses_one_drip_only_for_changed_files) {
    temporary_repository location;
    const auto source = location.path() / "source";
    write_file(source / "kept.txt", content("first"));
    write_file(source / "ignored.txt", content("ignored"));
    write_file(source / "caller-ignored.txt", content("ignored"));
    write_file(source / "caller-ignored-directory" / "nested.txt", content("ignored"));
    write_file(source / "build-wrapped" / "CMakeCache.txt", content("configured"));
    write_file(source / ".gitignore", content("ignored.txt\nbuild-*/\n"));
    auto created =
        liquid::runtime::block_on(liquid::repository::create(location.path() / "target"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    auto options = liquid::import_options{};
    options.ignore_patterns = {
        "caller-ignored.txt", "caller-ignored-directory/**", "!build-wrapped", "!build-wrapped/**"};
    const auto first = liquid::runtime::block_on(repository.import_tree(source, options));
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->drip.has_value());
    EXPECT_EQ(first->files_seen, 3U);
    EXPECT_EQ(first->changed, 3U);
    EXPECT_GT(first->bytes_hashed, 0U);
    EXPECT_GT(first->bytes_written, 0U);
    const auto ignored = liquid::runtime::block_on(repository.read("ignored.txt"));
    EXPECT_FALSE(ignored);
    const auto caller_ignored = liquid::runtime::block_on(repository.read("caller-ignored.txt"));
    EXPECT_FALSE(caller_ignored);
    const auto ignored_directory =
        liquid::runtime::block_on(repository.read("caller-ignored-directory/nested.txt"));
    EXPECT_FALSE(ignored_directory);
    const auto included_build =
        liquid::runtime::block_on(repository.read("build-wrapped/CMakeCache.txt"));
    ASSERT_TRUE(included_build) << included_build.error().message;

    repository = {};
    auto reopened = liquid::runtime::block_on(
        liquid::repository::open(location.path() / "target", {.defer_tree_load = true}));
    ASSERT_TRUE(reopened) << reopened.error().message;
    repository = std::move(*reopened);
    const auto before = repository.status();
    ASSERT_TRUE(before) << before.error().message;
    const auto unchanged = liquid::runtime::block_on(repository.import_tree(source, options));
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_FALSE(unchanged->drip.has_value());
    EXPECT_EQ(unchanged->changed, 0U);
    const auto after = repository.status();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after->pending_drips, before->pending_drips);
}

// Checkout-local state is never repository content (liquid #420). Release policy under `.liquid/`,
// a repository marker, the store, and build, wrap and data-branch state stay out of the tree even
// when a caller's `!` pattern asks for them, so a push can never hand them to a clone.
TEST(Repository, import_tree_never_records_checkout_local_state) {
    temporary_repository location;
    const auto source = location.path() / "source";
    write_file(source / "kept.txt", content("kept"));
    write_file(source / "LIQUID_REPO", content("marker"));
    write_file(source / "LIQUID_JOURNAL", content("journal"));
    write_file(source / ".liquid" / "release" / "policy.cbor", content("policy"));
    write_file(source / ".liquid-store" / "objects", content("store"));
    write_file(source / ".liquid-build" / "tests" / "local.test", content("test"));
    write_file(source / ".liquid-data" / "actions" / "local", content("data"));
    write_file(source / ".liquid-wrap-cache" / "key" / "state", content("wrap"));
    write_file(source / ".liquid-capture-cookie-1", content("cookie"));
    auto created =
        liquid::runtime::block_on(liquid::repository::create(location.path() / "target"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    auto options = liquid::import_options{};
    options.ignore_patterns = {"!.liquid/**", "!LIQUID_REPO", "!.liquid-build/**"};
    const auto imported = liquid::runtime::block_on(repository.import_tree(source, options));
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported->files_seen, 1U);
    const auto kept = liquid::runtime::block_on(repository.read("kept.txt"));
    ASSERT_TRUE(kept) << kept.error().message;
    EXPECT_EQ(text(*kept), "kept");
    for (const auto* path : {"LIQUID_REPO",
                             "LIQUID_JOURNAL",
                             ".liquid/release/policy.cbor",
                             ".liquid-store/objects",
                             ".liquid-build/tests/local.test",
                             ".liquid-data/actions/local",
                             ".liquid-wrap-cache/key/state",
                             ".liquid-capture-cookie-1"}) {
        EXPECT_FALSE(liquid::runtime::block_on(repository.read(path))) << path;
    }
}

TEST(Repository, import_tree_preserves_file_identity_and_records_removals) {
    temporary_repository location;
    const auto source = location.path() / "source";
    write_file(source / "changed.txt", content("before"));
    write_file(source / "removed.txt", content("remove"));
    auto created =
        liquid::runtime::block_on(liquid::repository::create(location.path() / "target"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.import_tree(source)));
    const auto before = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(before) << before.error().message;

    write_file(source / "changed.txt", content("after"));
    const auto changed = liquid::runtime::block_on(repository.import_tree(source));
    ASSERT_TRUE(changed) << changed.error().message;
    ASSERT_TRUE(changed->drip.has_value());
    EXPECT_EQ(changed->changed, 1U);
    const auto after_change = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(after_change) << after_change.error().message;
    const auto changes = liquid::runtime::block_on(repository.diff(*before, *after_change));
    ASSERT_TRUE(changes) << changes.error().message;
    const auto modified =
        std::ranges::find(*changes, "changed.txt", &liquid::tree_diff_entry::path);
    ASSERT_NE(modified, changes->end());
    EXPECT_TRUE(has_change(*modified, liquid::tree_diff_kind::modified));
    EXPECT_EQ(changes->size(), 1U);

    ASSERT_TRUE(std::filesystem::remove(source / "removed.txt"));
    const auto removed_import = liquid::runtime::block_on(repository.import_tree(source));
    ASSERT_TRUE(removed_import) << removed_import.error().message;
    ASSERT_TRUE(removed_import->drip.has_value());
    EXPECT_EQ(removed_import->changed, 1U);
    const auto after_removal = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(after_removal) << after_removal.error().message;
    const auto removal = liquid::runtime::block_on(repository.diff(*after_change, *after_removal));
    ASSERT_TRUE(removal) << removal.error().message;
    const auto removed = std::ranges::find(*removal, "removed.txt", &liquid::tree_diff_entry::path);
    ASSERT_NE(removed, removal->end());
    EXPECT_TRUE(has_change(*removed, liquid::tree_diff_kind::removed));
}

TEST(Repository, import_tree_has_a_bounded_peak_rss) {
    constexpr auto file_count = std::size_t{1024};
    constexpr auto file_size = std::size_t{1024};
    constexpr auto rss_ceiling = std::uint64_t{512 * 1024 * 1024};
    temporary_repository location;
    const auto source = location.path() / "source";
    const auto payload = std::vector<std::byte>(file_size, std::byte{0x5a});
    for (auto index = std::size_t{}; index != file_count; ++index) {
        write_file(source / "files" / std::to_string(index), payload);
    }
    auto created =
        liquid::runtime::block_on(liquid::repository::create(location.path() / "target"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto imported = liquid::runtime::block_on(repository.import_tree(source));
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported->files_seen, file_count);
    EXPECT_LT(peak_rss_bytes(), rss_ceiling);
}

TEST(Repository, import_tree_preserves_ignored_entries_in_a_paged_tree) {
    constexpr auto file_count = std::size_t{1024};
    temporary_repository location;
    const auto source = location.path() / "source";
    const auto payload = std::vector<std::byte>(1024, std::byte{0x5a});
    for (auto index = std::size_t{}; index != file_count; ++index) {
        write_file(source / "sources" / std::to_string(index), payload);
    }
    auto created =
        liquid::runtime::block_on(liquid::repository::create(location.path() / "target"));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.import_tree(source)));

    repository = {};
    auto reopened = liquid::runtime::block_on(
        liquid::repository::open(location.path() / "target", {.defer_tree_load = true}));
    ASSERT_TRUE(reopened) << reopened.error().message;
    repository = std::move(*reopened);

    const auto retained = liquid::runtime::block_on(repository.read("sources/0"));
    ASSERT_TRUE(retained) << retained.error().message << ": " << retained.error().context;

    write_file(source / "build" / "CMakeCache.txt", content("configured"));
    auto options = liquid::import_options{};
    options.ignore_patterns = {"build", "build/**", "!build", "!build/**"};
    const auto included = liquid::runtime::block_on(repository.import_tree(source, options));
    ASSERT_TRUE(included) << included.error().message << ": " << included.error().context;
    ASSERT_TRUE(included->drip.has_value());
    EXPECT_EQ(included->changed, 1U);
    const auto cache = liquid::runtime::block_on(repository.read("build/CMakeCache.txt"));
    ASSERT_TRUE(cache) << cache.error().message;
    EXPECT_EQ(*cache, content("configured"));

    const auto recorded = liquid::runtime::block_on(repository.cut("record paged import"));
    ASSERT_TRUE(recorded) << recorded.error().message << ": " << recorded.error().context;
    const auto recorded_cache = liquid::runtime::block_on(repository.read("build/CMakeCache.txt"));
    ASSERT_TRUE(recorded_cache) << recorded_cache.error().message;
    EXPECT_EQ(*recorded_cache, content("configured"));

    options.ignore_patterns = {"build/**"};
    const auto unchanged = liquid::runtime::block_on(repository.import_tree(source, options));
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    EXPECT_FALSE(unchanged->drip.has_value());
    EXPECT_EQ(unchanged->changed, 0U);
}

auto test_session(std::byte first) -> liquid::session_id {
    auto output = liquid::session_id{};
    output.bytes.front() = first;
    return output;
}

auto operation_with_kind(liquid::repository& repository, std::string_view kind)
    -> liquid::result<liquid::operation_record> {
    auto operations = liquid::runtime::block_on(repository.operations());
    if (!operations) {
        return std::unexpected(operations.error());
    }
    const auto found = std::ranges::find(*operations, kind, &liquid::operation_record::kind);
    if (found == operations->end()) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "operation kind is absent", std::string{kind}});
    }
    return *found;
}

auto exported_items(liquid::repository& repository)
    -> liquid::result<std::vector<liquid::sync_item>> {
    auto cursor = liquid::journal_cursor{};
    auto output = std::vector<liquid::sync_item>{};
    for (;;) {
        auto page = std::vector<liquid::sync_item>(32);
        auto exported = liquid::runtime::block_on(repository.export_drips(cursor, page));
        if (!exported) {
            return std::unexpected(exported.error());
        }
        output.insert(output.end(), page.begin(), page.begin() + exported->count);
        cursor = exported->cursor;
        if (exported->done) {
            return output;
        }
    }
}

TEST(Repository, SuppliedStoreHostsRepositoriesAndDeduplicatesTheirContent) {
    temporary_repository location;
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / "customer-store"));
    ASSERT_TRUE(opened_store);
    auto shared_store = std::move(*opened_store);
    auto options = liquid::repository_options{};
    options.store = &shared_store;
    auto first = liquid::runtime::block_on(liquid::repository::create("first", options));
    auto second = liquid::runtime::block_on(liquid::repository::create("second", options));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    auto payload = std::vector<std::byte>(4096, std::byte{0x5a});
    auto expected = liquid::hash(liquid::algorithm::blake3, payload);
    ASSERT_TRUE(expected);
    ASSERT_TRUE(liquid::runtime::block_on(first->write("shared.bin", payload)));
    ASSERT_TRUE(liquid::runtime::block_on(second->write("shared.bin", payload)));
    ASSERT_TRUE(liquid::runtime::block_on(first->cut("first")));
    ASSERT_TRUE(liquid::runtime::block_on(second->cut("second")));
    EXPECT_EQ(physical_copies(location.path() / "customer-store" / "packs", *expected), 1U);

    auto collected = liquid::runtime::block_on(first->collect());
    ASSERT_TRUE(collected);
    auto still_present = liquid::runtime::block_on(second->read("shared.bin"));
    ASSERT_TRUE(still_present);
    EXPECT_EQ(*still_present, payload);

    *first = liquid::repository{};
    *second = liquid::repository{};
    auto reopened = liquid::runtime::block_on(liquid::repository::open("second", options));
    ASSERT_TRUE(reopened);
    auto restored = liquid::runtime::block_on(reopened->read("shared.bin"));
    ASSERT_TRUE(restored);
    EXPECT_EQ(*restored, payload);
    auto store_still_open = liquid::runtime::block_on(shared_store.contains(*expected));
    ASSERT_TRUE(store_still_open);
    EXPECT_TRUE(*store_still_open);
}

TEST(RepositoryFlow, eleventh_immediate_write_is_rejected_without_an_operation) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.flow.per_actor = {.rate_per_s = 10, .burst = 10};
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << (created ? "" : created.error().message);
    for (auto index = 0; index != 10; ++index) {
        auto written = liquid::runtime::block_on(
            created->write("file-" + std::to_string(index), content("value")));
        ASSERT_TRUE(written) << (written ? "" : written.error().message);
    }
    auto refused = liquid::runtime::block_on(created->write("file-10", content("value")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::busy);
    EXPECT_NE(refused.error().context.find("scope=actor"), std::string::npos);
    const auto retry = retry_after_ms(refused.error().context);
    ASSERT_GT(retry, 0U);
    auto operations = liquid::runtime::block_on(created->operations());
    ASSERT_TRUE(operations);
    EXPECT_EQ(operations->size(), 10U);

    ASSERT_TRUE(liquid::runtime::block_on(wait_for(std::chrono::milliseconds{retry + 5})));
    EXPECT_TRUE(liquid::runtime::block_on(created->write("file-10", content("value"))));
}

TEST(RepositoryFlow, oversized_update_is_rejected_before_an_operation) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.flow.max_update_bytes = 4;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created);
    auto refused = liquid::runtime::block_on(created->write("file", content("12345")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
    EXPECT_NE(refused.error().message.find("max_update_bytes"), std::string::npos);
    auto operations = liquid::runtime::block_on(created->operations());
    ASSERT_TRUE(operations);
    EXPECT_TRUE(operations->empty());
}

TEST(RepositoryFlow, document_bucket_is_shared_by_actors_and_isolated_by_directory) {
    temporary_repository location;
    const auto first_home = location.path().parent_path() / "flow-identity-first";
    const auto second_home = location.path().parent_path() / "flow-identity-second";
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
    auto identity_home = scoped_liquid_home{first_home};
    auto options = liquid::repository_options{};
    options.flow.per_document = {.rate_per_s = 10, .burst = 1};
    auto first = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(first);
    ASSERT_TRUE(liquid::runtime::block_on(first->write("one/seed", content("seed"))));
    ASSERT_TRUE(liquid::runtime::block_on(wait_for(std::chrono::milliseconds{1005})));
    ASSERT_TRUE(liquid::runtime::block_on(first->write("two/seed", content("seed"))));

    identity_home.set(second_home);
    auto second = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->flow_configuration(), options.flow);
    ASSERT_TRUE(liquid::runtime::block_on(wait_for(std::chrono::milliseconds{1005})));
    ASSERT_TRUE(liquid::runtime::block_on(first->write("one/first", content("one"))));
    auto same_document = liquid::runtime::block_on(second->write("one/second", content("two")));
    ASSERT_FALSE(same_document);
    EXPECT_EQ(same_document.error().code, liquid::errc::busy);
    EXPECT_NE(same_document.error().context.find("scope=document"), std::string::npos);
    EXPECT_TRUE(liquid::runtime::block_on(second->write("two/second", content("two"))));
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
}

void expect_sha256(const liquid::digest& value) {
    EXPECT_EQ(value.alg, liquid::algorithm::sha256);
    EXPECT_TRUE(liquid::to_string(value).starts_with("sha256:"));
}

auto field(const liquid::encoding::cbor::value& value, std::string_view name)
    -> const liquid::encoding::cbor::value* {
    const auto* fields = std::get_if<liquid::encoding::cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (const auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto mutable_field(liquid::encoding::cbor::value& value, std::string_view name)
    -> liquid::encoding::cbor::value* {
    auto* fields = std::get_if<liquid::encoding::cbor::value::map>(&value.data);
    if (!fields) {
        return nullptr;
    }
    for (auto& [key, item] : *fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &item;
        }
    }
    return nullptr;
}

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    EXPECT_TRUE(input.good());
    const auto size = input.tellg();
    EXPECT_GE(size, 0);
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    EXPECT_TRUE(input.good());
    return output;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

TEST(Repository, create_write_move_cut_open_and_time_travel) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("one.txt", content("one"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("dir/two.txt", content("two"))));
    auto before = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(before);
    ASSERT_EQ(before->entries.size(), 2U);
    const auto identity = before->entries.front().node;
    auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    auto verified = liquid::repository_detail::testing::verify_history(repository);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(*verified, 3U);
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("one.txt", "renamed.txt")));
    auto after = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(after);
    ASSERT_EQ(after->entries.front().node, identity);
    auto second = liquid::runtime::block_on(repository.cut("rename"));
    ASSERT_TRUE(second);
    auto snapshot = liquid::runtime::block_on(repository.at(*first));
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(std::ranges::any_of(snapshot->root.entries,
                                    [](const auto& entry) { return entry.name == "one.txt"; }));
    auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs);
    ASSERT_EQ(refs->size(), 1U);
    EXPECT_EQ(refs->front().id.value.bytes, second->value.bytes);

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("renamed.txt"));
    ASSERT_TRUE(value);
    EXPECT_EQ(text(*value), "one");
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_REPO"));
}

TEST(Repository, ReadabilityReportsExactBlobKeyVersionsAcrossRotation) {
    temporary_repository location;
    auto keys = encryption_keys();
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), encrypted_options(keys)));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("old.txt", content("old"))));
    const auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    const auto rotated = liquid::runtime::block_on(keys->rotate("repository"));
    ASSERT_TRUE(rotated) << rotated.error().message;
    const auto new_source = location.path() / "new-source";
    const auto new_contents = content("new");
    write_file(new_source, new_contents);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write_file("new.txt", new_source)));
    const auto second = liquid::runtime::block_on(repository.cut("second"));
    ASSERT_TRUE(second) << second.error().message;

    const auto report = liquid::runtime::block_on(repository.readability());
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->revisions.size(), 2U);
    EXPECT_EQ(liquid::to_string(report->revisions[0].revision.value),
              liquid::to_string(second->value));
    ASSERT_EQ(report->revisions[0].keys.size(), 2U);
    EXPECT_EQ(report->revisions[0].keys[0].key_id, "repository");
    EXPECT_EQ(report->revisions[0].keys[0].version, 1U);
    EXPECT_TRUE(report->revisions[0].keys[0].readable);
    EXPECT_EQ(report->revisions[0].keys[1].key_id, "repository");
    EXPECT_EQ(report->revisions[0].keys[1].version, 2U);
    EXPECT_TRUE(report->revisions[0].keys[1].readable);
    EXPECT_EQ(liquid::to_string(report->revisions[1].revision.value),
              liquid::to_string(first->value));
    ASSERT_EQ(report->revisions[1].keys.size(), 1U);
    EXPECT_EQ(report->revisions[1].keys[0].key_id, "repository");
    EXPECT_EQ(report->revisions[1].keys[0].version, 1U);
    EXPECT_TRUE(report->revisions[1].keys[0].readable);
    ASSERT_EQ(report->limits.size(), 3U);
    EXPECT_EQ(report->limits[1],
              "Revoking a collaborator prevents future access; it does not recall what they "
              "have.");

    const auto revoked = liquid::runtime::block_on(keys->revoke("repository", 1));
    ASSERT_TRUE(revoked) << revoked.error().message;
    const auto current = liquid::runtime::block_on(repository.readability(*second));
    ASSERT_TRUE(current) << current.error().message;
    ASSERT_EQ(current->revisions.size(), 1U);
    ASSERT_EQ(current->revisions[0].keys.size(), 2U);
    EXPECT_FALSE(current->revisions[0].keys[0].readable);
    EXPECT_TRUE(current->revisions[0].keys[1].readable);
    const auto old_value = liquid::runtime::block_on(repository.read("old.txt"));
    ASSERT_FALSE(old_value);
    EXPECT_EQ(old_value.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(old_value.error().context, "repository@1");
    const auto new_value = liquid::runtime::block_on(repository.read("new.txt"));
    ASSERT_TRUE(new_value) << new_value.error().message;
    EXPECT_EQ(text(*new_value), "new");
}

TEST(Repository, record_preserves_imported_change_fields) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("contents"))));

    liquid::change_input input{};
    input.author = liquid::actor{"git:author@example.test"};
    input.at = std::chrono::sys_seconds{std::chrono::seconds{123456789}};
    input.message = "imported Git change";
    input.metadata.emplace("git_commit", "0123456789012345678901234567890123456789");
    input.ref = "imported";
    const auto id = liquid::runtime::block_on(repository.record(std::move(input)));
    ASSERT_TRUE(id) << id.error().message;

    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    EXPECT_EQ(history->front().id.value.alg, id->value.alg);
    EXPECT_EQ(history->front().id.value.bytes, id->value.bytes);
    EXPECT_TRUE(history->front().parents.empty());
    EXPECT_EQ(history->front().author.taap_id, "git:author@example.test");
    EXPECT_EQ(history->front().at, std::chrono::sys_seconds{std::chrono::seconds{123456789}});
    EXPECT_EQ(history->front().message, "imported Git change");
    EXPECT_EQ(history->front().metadata.at("git_commit"),
              "0123456789012345678901234567890123456789");
    const auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs) << refs.error().message;
    ASSERT_EQ(refs->size(), 1U);
    EXPECT_EQ(refs->front().name, "imported");
    EXPECT_EQ(refs->front().id.value.alg, id->value.alg);
    EXPECT_EQ(refs->front().id.value.bytes, id->value.bytes);
}

TEST(Repository, metadata_persists_and_lists_a_prefix) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.set_metadata("git.commit.0123", content("blake3:change"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.set_metadata("git.change.blake3", content("0123"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.set_metadata("other", content("ignored"))));

    const auto keys = liquid::runtime::block_on(repository.metadata_keys("git."));
    ASSERT_TRUE(keys) << keys.error().message;
    EXPECT_EQ(*keys, (std::vector<std::string>{"git.change.blake3", "git.commit.0123"}));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto stored = liquid::runtime::block_on(reopened->metadata("git.commit.0123"));
    ASSERT_TRUE(stored) << stored.error().message;
    ASSERT_TRUE(*stored);
    EXPECT_EQ(text(**stored), "blake3:change");
    const auto absent = liquid::runtime::block_on(reopened->metadata("git.commit.missing"));
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_FALSE(*absent);
}

TEST(Repository, log_walks_the_full_graph_or_the_first_parent) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto author = liquid::actor{"git:author@example.test"};

    liquid::change_input base_input{
        {}, author, std::chrono::sys_seconds{std::chrono::seconds{1}}, "base", {}, "main"};
    const auto base = liquid::runtime::block_on(repository.record(std::move(base_input)));
    ASSERT_TRUE(base) << base.error().message;
    liquid::change_input left_input{{{{}, *base}},
                                    author,
                                    std::chrono::sys_seconds{std::chrono::seconds{2}},
                                    "left",
                                    {},
                                    "left"};
    const auto left = liquid::runtime::block_on(repository.record(std::move(left_input)));
    ASSERT_TRUE(left) << left.error().message;
    liquid::change_input right_input{{{{}, *base}},
                                     author,
                                     std::chrono::sys_seconds{std::chrono::seconds{3}},
                                     "right",
                                     {},
                                     "right"};
    const auto right = liquid::runtime::block_on(repository.record(std::move(right_input)));
    ASSERT_TRUE(right) << right.error().message;
    liquid::change_input merge_input{{{{}, *left}, {{}, *right}},
                                     author,
                                     std::chrono::sys_seconds{std::chrono::seconds{4}},
                                     "merge",
                                     {},
                                     "main"};
    const auto merge = liquid::runtime::block_on(repository.record(std::move(merge_input)));
    ASSERT_TRUE(merge) << merge.error().message;

    const auto graph = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(graph) << graph.error().message;
    ASSERT_EQ(graph->size(), 4U);
    EXPECT_EQ(graph->at(0).message, "merge");
    EXPECT_EQ(graph->at(1).message, "right");
    EXPECT_EQ(graph->at(2).message, "left");
    EXPECT_EQ(graph->at(3).message, "base");

    const auto first_parent = liquid::runtime::block_on(repository.log({.first_parent = true}));
    ASSERT_TRUE(first_parent) << first_parent.error().message;
    ASSERT_EQ(first_parent->size(), 3U);
    EXPECT_EQ(first_parent->at(0).message, "merge");
    EXPECT_EQ(first_parent->at(1).message, "left");
    EXPECT_EQ(first_parent->at(2).message, "base");

    const auto from_right = liquid::runtime::block_on(repository.log({.from = *right}));
    ASSERT_TRUE(from_right) << from_right.error().message;
    ASSERT_EQ(from_right->size(), 2U);
    EXPECT_EQ(from_right->at(0).message, "right");
    EXPECT_EQ(from_right->at(1).message, "base");
}

TEST(Repository, tree_and_blob_read_historical_records) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("directory/file", content("contents"))));
    const auto change = liquid::runtime::block_on(repository.cut("record tree"));
    ASSERT_TRUE(change) << change.error().message;
    const auto snapshot = liquid::runtime::block_on(repository.at(*change));
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(snapshot->root.entries.size(), 1U);

    const auto directory =
        liquid::runtime::block_on(repository.tree(snapshot->root.entries.front().ref));
    ASSERT_TRUE(directory) << directory.error().message;
    ASSERT_EQ(directory->entries.size(), 1U);
    const auto contents =
        liquid::runtime::block_on(repository.blob(directory->entries.front().ref));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(text(*contents), "contents");
}

TEST(Repository, diff_reports_path_changes_and_optional_text_hunks) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("old-name", content("move\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("modified", content("one\ntwo\nthree\nfour\nfive\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("removed", content("remove\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("mode", content("mode\n"))));
    const auto before = liquid::runtime::block_on(repository.cut("before"));
    ASSERT_TRUE(before) << before.error().message;

    ASSERT_TRUE(liquid::runtime::block_on(repository.move("old-name", "new-name")));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("modified", content("one\nchanged\nthree\nchanged-again\nfive\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.remove("removed")));
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("mode", content("mode\n"), {.mode = 0100755})));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("added", content("add\n"))));
    const auto after = liquid::runtime::block_on(repository.cut("after"));
    ASSERT_TRUE(after) << after.error().message;

    const auto changes =
        liquid::runtime::block_on(repository.diff(*before, *after, {.text = true}));
    ASSERT_TRUE(changes) << changes.error().message;
    ASSERT_EQ(changes->size(), 5U);

    const auto moved = std::ranges::find(*changes, "new-name", &liquid::tree_diff_entry::path);
    ASSERT_NE(moved, changes->end());
    EXPECT_EQ(moved->old_path, "old-name");
    EXPECT_TRUE(has_change(*moved, liquid::tree_diff_kind::moved));
    EXPECT_TRUE(moved->old_blob);
    EXPECT_TRUE(moved->new_blob);

    const auto modified = std::ranges::find(*changes, "modified", &liquid::tree_diff_entry::path);
    ASSERT_NE(modified, changes->end());
    EXPECT_TRUE(has_change(*modified, liquid::tree_diff_kind::modified));
    ASSERT_TRUE(modified->line_hunks);
    ASSERT_EQ(modified->line_hunks->size(), 2U);
    EXPECT_EQ(modified->line_hunks->at(0).old_line, 2U);
    EXPECT_EQ(modified->line_hunks->at(1).old_line, 4U);

    const auto removed = std::ranges::find(*changes, "removed", &liquid::tree_diff_entry::path);
    ASSERT_NE(removed, changes->end());
    EXPECT_TRUE(has_change(*removed, liquid::tree_diff_kind::removed));
    EXPECT_TRUE(removed->old_blob);
    EXPECT_FALSE(removed->new_blob);
    ASSERT_TRUE(removed->line_hunks);
    ASSERT_EQ(removed->line_hunks->size(), 1U);
    EXPECT_EQ(removed->line_hunks->front().old_count, 1U);

    const auto added = std::ranges::find(*changes, "added", &liquid::tree_diff_entry::path);
    ASSERT_NE(added, changes->end());
    ASSERT_TRUE(added->line_hunks);
    ASSERT_EQ(added->line_hunks->size(), 1U);
    EXPECT_EQ(added->line_hunks->front().new_count, 1U);

    const auto mode = std::ranges::find(*changes, "mode", &liquid::tree_diff_entry::path);
    ASSERT_NE(mode, changes->end());
    EXPECT_TRUE(has_change(*mode, liquid::tree_diff_kind::mode_changed));
    EXPECT_FALSE(has_change(*mode, liquid::tree_diff_kind::modified));

    const auto old_snapshot = liquid::runtime::block_on(repository.at(*before));
    const auto new_snapshot = liquid::runtime::block_on(repository.at(*after));
    ASSERT_TRUE(old_snapshot);
    ASSERT_TRUE(new_snapshot);
    const auto tree_changes =
        liquid::runtime::block_on(repository.diff(old_snapshot->root, new_snapshot->root));
    ASSERT_TRUE(tree_changes);
    EXPECT_EQ(tree_changes->size(), changes->size());
    EXPECT_TRUE(std::ranges::all_of(
        *tree_changes, [](const auto& entry) { return !entry.line_hunks.has_value(); }));
}

TEST(Repository, format_diff_reportsZipMembersAndHdf5Datasets) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("archive.zip", format_fixture("archive-a.zip"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("measurements.h5", format_fixture("sample-a.h5"))));
    auto before = liquid::runtime::block_on(repository.cut("before format changes"));
    ASSERT_TRUE(before) << before.error().message;

    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("archive.zip", format_fixture("archive-b.zip"))));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("measurements.h5", format_fixture("sample-b.h5"))));
    auto after = liquid::runtime::block_on(repository.cut("after format changes"));
    ASSERT_TRUE(after) << after.error().message;

    auto changes = liquid::runtime::block_on(
        repository.diff(*before, *after, liquid::tree_diff_options{.formats = true}));
    ASSERT_TRUE(changes) << changes.error().message;
    ASSERT_EQ(changes->size(), 2);
    const auto archive = std::ranges::find(*changes, "archive.zip", &liquid::tree_diff_entry::path);
    ASSERT_NE(archive, changes->end());
    ASSERT_TRUE(archive->container);
    EXPECT_EQ(archive->container->codec, "zip");
    ASSERT_EQ(archive->container->entries.size(), 1);
    EXPECT_EQ(archive->container->entries.front().path, "deflated.txt");
    const auto hdf5 =
        std::ranges::find(*changes, "measurements.h5", &liquid::tree_diff_entry::path);
    ASSERT_NE(hdf5, changes->end());
    ASSERT_TRUE(hdf5->container);
    EXPECT_EQ(hdf5->container->codec, "hdf5");
    ASSERT_EQ(hdf5->container->entries.size(), 1);
    EXPECT_EQ(hdf5->container->entries.front().path, "measurements/chunked");
}

TEST(Repository, formatEntriesAreStoredOnceAndRetainedWithTheirContainers) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("first.zip", format_fixture("archive-a.zip"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("second.zip", format_fixture("archive-b.zip"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("store format entries")));
    const auto payload = content("shared vendored payload\n");
    auto id = liquid::hash(liquid::algorithm::blake3, payload);
    ASSERT_TRUE(id) << id.error().message;
    const auto packs = location.path() / ".liquid-store" / "packs";
    EXPECT_EQ(physical_copies(packs, *id), 1);

    auto collected = liquid::runtime::block_on(repository.collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_EQ(physical_copies(packs, *id), 1);
}

TEST(Repository, text_anchors_follow_hunks_and_node_identity_moves) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write(
        "notes.txt", content("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("removed.txt", content("gone\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("stable.txt", content("same\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("shrink.txt", content("a\nb\nc\n"))));
    const auto before = liquid::runtime::block_on(repository.cut("before anchors"));
    ASSERT_TRUE(before) << before.error().message;

    ASSERT_TRUE(liquid::runtime::block_on(repository.move("notes.txt", "renamed.txt")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write(
        "renamed.txt", content("zero\none\ntwo\nthree\nFOUR\nfive\nseven\neight\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.remove("removed.txt")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("shrink.txt", content("b\nc\n"))));
    const auto after = liquid::runtime::block_on(repository.cut("after anchors"));
    ASSERT_TRUE(after) << after.error().message;

    const auto anchors = std::to_array<liquid::text_anchor>({
        {*before, "notes.txt", 7, 2},
        {*before, "notes.txt", 4, 3},
        {*before, "notes.txt", 1, 1},
        {*before, "notes.txt", 6, 4},
        {*before, "removed.txt", 1, 1},
        {*before, "stable.txt", 1, 3},
        {*before, "shrink.txt", 3, 1},
    });
    const auto mapped = liquid::runtime::block_on(repository.map_anchors(anchors, *after));
    ASSERT_TRUE(mapped) << mapped.error().message;
    ASSERT_EQ(mapped->size(), anchors.size());

    EXPECT_EQ(mapped->at(0).path, "renamed.txt");
    EXPECT_EQ(mapped->at(0).line, 7U);
    EXPECT_EQ(mapped->at(0).column, 2U);
    EXPECT_TRUE(mapped->at(0).moved);
    EXPECT_FALSE(mapped->at(0).approximate);
    EXPECT_FALSE(mapped->at(0).deleted);

    EXPECT_EQ(mapped->at(1).path, "renamed.txt");
    EXPECT_EQ(mapped->at(1).line, 5U);
    EXPECT_EQ(mapped->at(1).column, 3U);
    EXPECT_TRUE(mapped->at(1).moved);
    EXPECT_TRUE(mapped->at(1).approximate);
    EXPECT_FALSE(mapped->at(1).deleted);

    EXPECT_EQ(mapped->at(2).line, 2U);
    EXPECT_TRUE(mapped->at(2).moved);
    EXPECT_FALSE(mapped->at(2).approximate);
    EXPECT_FALSE(mapped->at(2).deleted);

    EXPECT_EQ(mapped->at(3).path, "renamed.txt");
    EXPECT_EQ(mapped->at(3).line, 7U);
    EXPECT_EQ(mapped->at(3).column, 1U);
    EXPECT_TRUE(mapped->at(3).moved);
    EXPECT_FALSE(mapped->at(3).approximate);
    EXPECT_TRUE(mapped->at(3).deleted);

    EXPECT_EQ(mapped->at(4).path, "removed.txt");
    EXPECT_TRUE(mapped->at(4).deleted);

    EXPECT_EQ(mapped->at(5).path, "stable.txt");
    EXPECT_EQ(mapped->at(5).line, 1U);
    EXPECT_EQ(mapped->at(5).column, 3U);
    EXPECT_FALSE(mapped->at(5).moved);
    EXPECT_FALSE(mapped->at(5).approximate);
    EXPECT_FALSE(mapped->at(5).deleted);

    EXPECT_EQ(mapped->at(6).path, "shrink.txt");
    EXPECT_EQ(mapped->at(6).line, 2U);
    EXPECT_TRUE(mapped->at(6).moved);
    EXPECT_FALSE(mapped->at(6).approximate);
    EXPECT_FALSE(mapped->at(6).deleted);

    const auto single = liquid::runtime::block_on(repository.map_anchor(
        {.change = *before, .path = "notes.txt", .line = 8, .column = 1}, *after));
    ASSERT_TRUE(single) << single.error().message;
    EXPECT_EQ(single->path, "renamed.txt");
    EXPECT_EQ(single->line, 8U);
    EXPECT_TRUE(single->moved);
    EXPECT_FALSE(single->approximate);
    EXPECT_FALSE(single->deleted);
}

TEST(Repository, diff_surfaces_recorded_conflict_values) {
    temporary_repository left_location;
    temporary_repository right_location;
    auto left = liquid::runtime::block_on(liquid::repository::create(left_location.path()));
    auto right = liquid::runtime::block_on(liquid::repository::create(right_location.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("same", content("left"))));
    const auto before = liquid::runtime::block_on(left->cut("before"));
    ASSERT_TRUE(before);
    ASSERT_TRUE(liquid::runtime::block_on(right->write("same", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->cut("right")));
    ASSERT_TRUE(liquid::runtime::block_on(left->combine(std::move(*right), "same")));
    const auto conflicted = liquid::runtime::block_on(left->cut("conflicted"));
    ASSERT_TRUE(conflicted);

    const auto changes = liquid::runtime::block_on(left->diff(*before, *conflicted));
    ASSERT_TRUE(changes) << changes.error().message;
    const auto same = std::ranges::find(*changes, "same", &liquid::tree_diff_entry::path);
    ASSERT_NE(same, changes->end());
    EXPECT_TRUE(has_change(*same, liquid::tree_diff_kind::conflicted));
    EXPECT_TRUE(same->old_conflicts.empty());
    ASSERT_EQ(same->new_conflicts.size(), 1U);
    auto value = liquid::runtime::block_on(left->conflict_at("same"));
    ASSERT_TRUE(value);
    EXPECT_EQ(liquid::to_string(same->new_conflicts.front()), liquid::to_string(value->id));
}

TEST(Repository, path_history_follows_moves_and_restore_appends_a_drip) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("notes.txt", content("first"), {.mode = 0100755})));
    auto first = liquid::runtime::block_on(repository.cut("first version"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.txt", content("second"))));
    auto second = liquid::runtime::block_on(repository.cut("second version"));
    ASSERT_TRUE(second);
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("notes.txt", "renamed.txt")));
    auto moved = liquid::runtime::block_on(repository.cut("rename notes"));
    ASSERT_TRUE(moved);
    ASSERT_TRUE(liquid::runtime::block_on(repository.remove("renamed.txt")));
    auto removed = liquid::runtime::block_on(repository.cut("remove notes"));
    ASSERT_TRUE(removed);

    auto history = liquid::runtime::block_on(repository.history("renamed.txt"));
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 4U);
    EXPECT_TRUE(history->at(0).deleted);
    EXPECT_EQ(liquid::to_string(history->at(0).change.value), liquid::to_string(removed->value));
    EXPECT_EQ(history->at(1).path, "renamed.txt");
    EXPECT_EQ(liquid::to_string(history->at(1).change.value), liquid::to_string(moved->value));
    EXPECT_EQ(history->at(2).path, "notes.txt");
    EXPECT_EQ(liquid::to_string(history->at(2).change.value), liquid::to_string(second->value));
    EXPECT_EQ(liquid::to_string(history->at(3).change.value), liquid::to_string(first->value));

    const auto drips_before = repository.status()->pending_drips;
    auto restored = liquid::runtime::block_on(repository.restore("renamed.txt", *first));
    ASSERT_TRUE(restored) << restored.error().message;
    auto contents = liquid::runtime::block_on(repository.read("renamed.txt"));
    ASSERT_TRUE(contents);
    EXPECT_EQ(text(*contents), "first");
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    const auto restored_entry =
        std::ranges::find(root->entries, "renamed.txt", &liquid::tree_entry::name);
    ASSERT_NE(restored_entry, root->entries.end());
    EXPECT_EQ(restored_entry->mode, 0100755U);
    EXPECT_EQ(repository.status()->pending_drips, drips_before + 1);
    auto log = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(log);
    EXPECT_EQ(liquid::to_string(log->front().id.value), liquid::to_string(removed->value));
}

TEST(Repository, write_modes_and_nested_references_are_preserved) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.write("script", content("#!/bin/sh\n"), {.mode = 0100755})));
    ASSERT_TRUE(
        liquid::runtime::block_on(repository.write("link", content("target"), {.mode = 0120000})));
    liquid::repo_ref nested{};
    nested.repo_id.front() = std::byte{1};
    nested.id.value.alg = liquid::algorithm::blake3;
    nested.id.value.bytes.front() = std::byte{2};
    ASSERT_TRUE(liquid::runtime::block_on(repository.nest_ref("module", nested)));

    const auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_EQ(root->entries.size(), 3U);
    EXPECT_EQ(root->entries.at(0).mode, 0120000U);
    EXPECT_EQ(root->entries.at(1).kind, liquid::node_kind::repo);
    EXPECT_EQ(root->entries.at(1).mode, 0160000U);
    ASSERT_TRUE(root->entries.at(1).nested);
    EXPECT_EQ(root->entries.at(1).nested->repo_id, nested.repo_id);
    EXPECT_EQ(root->entries.at(2).mode, 0100755U);
}

TEST(Repository, write_undo_and_locks_are_recorded) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), exclusive_checkout_options()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    auto operation = liquid::repository_detail::testing::last_operation(repository);
    ASSERT_TRUE(liquid::runtime::block_on(repository.undo(operation)));
    auto missing = liquid::runtime::block_on(repository.read("file"));
    EXPECT_FALSE(missing);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("file")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("replacement"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.unlock("file")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("after unlock"))));
}

TEST(RepositoryLocks, another_identity_is_refused_with_holder_time_and_reason) {
    temporary_repository location;
    const auto first_home = location.path().parent_path() / "lock-identity-first";
    const auto second_home = location.path().parent_path() / "lock-identity-second";
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
    auto identity_home = scoped_liquid_home{first_home};
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), exclusive_checkout_options("assets/**")));
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(created->write("assets/model.bin", content("original"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(created->lock("assets/model.bin", "large binary update")));
    auto locks = liquid::runtime::block_on(created->locks());
    ASSERT_TRUE(locks) << locks.error().message;
    ASSERT_EQ(locks->size(), 1U);
    const auto holder = locks->front().holder.taap_id;
    const auto since = std::to_string(locks->front().since.time_since_epoch().count());
    EXPECT_EQ(locks->front().reason, "large binary update");
    EXPECT_EQ(locks->front().authority, liquid::lock_authority::advisory);
    EXPECT_TRUE(
        liquid::runtime::block_on(created->write("assets/model.bin", content("holder update"))));

    identity_home.set(second_home);
    auto other = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(other) << other.error().message;
    auto refused =
        liquid::runtime::block_on(other->write("assets/model.bin", content("other update")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    EXPECT_NE(refused.error().message.find(holder), std::string::npos);
    EXPECT_NE(refused.error().message.find(since), std::string::npos);
    EXPECT_NE(refused.error().message.find("large binary update"), std::string::npos);
    auto nested_lock =
        liquid::runtime::block_on(other->lock("assets/model.bin/part", "nested claim"));
    ASSERT_FALSE(nested_lock);
    EXPECT_EQ(nested_lock.error().code, liquid::errc::locked);
    auto wrong_release = liquid::runtime::block_on(other->unlock("assets/model.bin"));
    ASSERT_FALSE(wrong_release);
    EXPECT_EQ(wrong_release.error().code, liquid::errc::unauthorized);

    ASSERT_TRUE(liquid::runtime::block_on(created->unlock("assets/model.bin")));
    other = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(other) << other.error().message;
    EXPECT_TRUE(
        liquid::runtime::block_on(other->write("assets/model.bin", content("after release"))));
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
}

TEST(RepositoryLocks, connected_authority_approves_acquire_and_release) {
    temporary_repository location;
    auto requests = std::vector<liquid::lock_authorization_request>{};
    auto options = exclusive_checkout_options("generated/**");
    options.authorize_lock =
        [&requests](liquid::lock_authorization_request request) -> liquid::runtime::task<void> {
        requests.push_back(std::move(request));
        co_return liquid::result<void>{};
    };
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(
        liquid::runtime::block_on(created->lock("generated/archive.zip", "regenerate archive")));
    auto locks = liquid::runtime::block_on(created->locks());
    ASSERT_TRUE(locks) << locks.error().message;
    ASSERT_EQ(locks->size(), 1U);
    EXPECT_EQ(locks->front().authority, liquid::lock_authority::lake);
    auto offline = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(offline) << offline.error().message;
    auto offline_release = liquid::runtime::block_on(offline->unlock("generated/archive.zip"));
    ASSERT_FALSE(offline_release);
    EXPECT_EQ(offline_release.error().code, liquid::errc::unavailable);
    ASSERT_TRUE(liquid::runtime::block_on(created->unlock("generated/archive.zip")));
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests.front().operation, liquid::lock_operation::acquire);
    EXPECT_EQ(requests.back().operation, liquid::lock_operation::release);
    EXPECT_EQ(requests.front().value.path, "generated/archive.zip");
}

TEST(RepositoryLocks, concurrent_offline_locks_reconcile_as_a_conflict_value) {
    temporary_repository source_path;
    temporary_repository peer_path;
    const auto first_home = source_path.path().parent_path() / "sync-lock-identity-first";
    const auto second_home = source_path.path().parent_path() / "sync-lock-identity-second";
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
    auto identity_home = scoped_liquid_home{first_home};
    auto source = liquid::runtime::block_on(
        liquid::repository::create(source_path.path(), exclusive_checkout_options("models/**")));
    ASSERT_TRUE(source) << source.error().message;
    auto cloned = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, peer_path.path()));
    ASSERT_TRUE(cloned) << cloned.error().message;
    cloned = liquid::repository{};

    identity_home.set(second_home);
    auto peer = liquid::runtime::block_on(liquid::repository::open(peer_path.path()));
    ASSERT_TRUE(peer) << peer.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(peer->lock("models/catalog.bin", "peer rebuild")));
    ASSERT_TRUE(liquid::runtime::block_on(source->lock("models/catalog.bin", "source rebuild")));
    // The source exports from its own identity home, so its author chain travels with the drips
    // and the peer can verify them (liquid#383).
    identity_home.set(first_home);
    const auto items = exported_items(*source);
    ASSERT_TRUE(items) << items.error().message;
    identity_home.set(second_home);
    auto imported = liquid::runtime::block_on(peer->import_drips(*items));
    ASSERT_TRUE(imported) << imported.error().message;

    auto conflicts = liquid::runtime::block_on(peer->conflicts());
    ASSERT_TRUE(conflicts) << conflicts.error().message;
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().region.path, "models/catalog.bin");
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::crdt_suspicious);
    EXPECT_NE(liquid::to_string(conflicts->front().left), liquid::to_string(liquid::digest{}));
    EXPECT_NE(liquid::to_string(conflicts->front().right), liquid::to_string(liquid::digest{}));
    auto peer_locks = liquid::runtime::block_on(peer->locks());
    ASSERT_TRUE(peer_locks) << peer_locks.error().message;
    ASSERT_EQ(peer_locks->size(), 1U);
    EXPECT_EQ(peer_locks->front().reason, "peer rebuild");
    std::filesystem::remove_all(first_home);
    std::filesystem::remove_all(second_home);
}

TEST(Repository, journal_replays_a_write_before_the_next_checkpoint) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("pending.txt", content("journal"))));
    EXPECT_TRUE(std::filesystem::exists(location.path() / "LIQUID_JOURNAL"));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto value = liquid::runtime::block_on(reopened->read("pending.txt"));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(text(*value), "journal");
}

TEST(Repository, journal_replay_restores_a_working_tree_for_mutation) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first.txt", content("first"))));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = liquid::runtime::block_on(reopened->root());
    ASSERT_TRUE(restored);
    EXPECT_TRUE(std::ranges::any_of(restored->entries, [](const liquid::tree_entry& entry) {
        return entry.name == "first.txt";
    }));
    auto moved = liquid::runtime::block_on(reopened->move("first.txt", "moved.txt"));
    ASSERT_TRUE(moved) << moved.error().message << " (" << moved.error().context << ")";
    auto root = liquid::runtime::block_on(reopened->root());
    ASSERT_TRUE(root);
    EXPECT_TRUE(std::ranges::any_of(
        root->entries, [](const liquid::tree_entry& entry) { return entry.name == "moved.txt"; }));
    EXPECT_FALSE(std::ranges::any_of(
        root->entries, [](const liquid::tree_entry& entry) { return entry.name == "first.txt"; }));
}

TEST(Repository, collect_retains_checkpointed_content_and_reclaims_orphans) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("cut.txt", content("cut"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("cut")));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("pending.txt", content("pending"))));

    auto store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store);
    const auto orphan = liquid::runtime::block_on(store->put(content("orphan")));
    ASSERT_TRUE(orphan);

    const auto collected = liquid::runtime::block_on(repository.collect());
    ASSERT_TRUE(collected) << collected.error().message;
    EXPECT_EQ(collected->roots, 4U);
    store = {};
    store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store);
    const auto removed = liquid::runtime::block_on(store->get(*orphan));
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, liquid::errc::not_found);

    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto cut = liquid::runtime::block_on(reopened->read("cut.txt"));
    ASSERT_TRUE(cut) << cut.error().message;
    EXPECT_EQ(text(*cut), "cut");
    const auto pending = liquid::runtime::block_on(reopened->read("pending.txt"));
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_EQ(text(*pending), "pending");
    const auto moved = liquid::runtime::block_on(reopened->move("pending.txt", "moved.txt"));
    ASSERT_TRUE(moved) << moved.error().message;
}

TEST(Repository, Sha256StoreRoundTripsRecords) {
    temporary_repository location;
    const auto options = sha256_options();
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("value"))));
    auto written = liquid::runtime::block_on(repository.read("file"));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text(*written), "value");

    const auto id = liquid::runtime::block_on(repository.cut("record sha256"));
    ASSERT_TRUE(id) << id.error().message;
    expect_sha256(id->value);

    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    expect_sha256(history->front().id.value);
    expect_sha256(history->front().root);

    const auto snapshot = liquid::runtime::block_on(repository.at(history->front().id));
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    expect_sha256(snapshot->id.value);
    ASSERT_EQ(snapshot->root.entries.size(), 1U);
    expect_sha256(snapshot->root.entries.front().id);
    expect_sha256(snapshot->root.entries.front().ref);

    auto store = liquid::runtime::block_on(
        liquid::store::open(location.path() / ".liquid-store", options.storage));
    ASSERT_TRUE(store) << store.error().message;
    const auto encoded_blob =
        liquid::runtime::block_on(store->get(snapshot->root.entries.front().ref));
    ASSERT_TRUE(encoded_blob) << encoded_blob.error().message;
    const auto blob = liquid::objects::decode_blob(*encoded_blob);
    ASSERT_TRUE(blob) << blob.error().message;
    ASSERT_EQ(blob->chunks.size(), 1U);
    expect_sha256(blob->chunks.front());

    repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path(), options));
    ASSERT_TRUE(reopened) << reopened.error().message;
    written = liquid::runtime::block_on(reopened->read("file"));
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(text(*written), "value");
    const auto reopened_snapshot = liquid::runtime::block_on(reopened->at(history->front().id));
    ASSERT_TRUE(reopened_snapshot) << reopened_snapshot.error().message;
    expect_sha256(reopened_snapshot->id.value);
}

TEST(Repository, RehashesBlake3BlobsForSha256Resolution) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first blob"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("second", content("second blob"))));

    const auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_EQ(root->entries.size(), 2U);
    repository = {};

    auto store = liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(store) << store.error().message;
    auto blobs = std::vector<std::pair<liquid::digest, std::vector<std::byte>>>{};
    for (const auto& entry : root->entries) {
        ASSERT_EQ(entry.kind, liquid::node_kind::blob);
        EXPECT_EQ(entry.ref.alg, liquid::algorithm::blake3);
        const auto blob = liquid::runtime::block_on(store->get(entry.ref));
        ASSERT_TRUE(blob) << blob.error().message;
        const auto record = liquid::objects::decode_blob(*blob);
        ASSERT_TRUE(record) << record.error().message;
        blobs.emplace_back(entry.ref, *blob);
    }
    *store = {};

    liquid::store_options sha256_store_options;
    sha256_store_options.hash_algorithm = liquid::algorithm::sha256;
    store = liquid::runtime::block_on(
        liquid::store::open(location.path() / ".liquid-store", sha256_store_options));
    ASSERT_TRUE(store) << store.error().message;
    for (const auto& [blake3_id, blob] : blobs) {
        const auto sha256_id = liquid::hash(liquid::algorithm::sha256, blob);
        ASSERT_TRUE(sha256_id) << sha256_id.error().message;
        const auto indexed = liquid::runtime::block_on(store->put(blob));
        ASSERT_TRUE(indexed) << indexed.error().message;
        EXPECT_EQ(indexed->alg, sha256_id->alg);
        EXPECT_EQ(indexed->bytes, sha256_id->bytes);

        const auto parsed_blake3 = liquid::parse_digest(liquid::to_string(blake3_id));
        const auto parsed_sha256 = liquid::parse_digest(liquid::to_string(*sha256_id));
        ASSERT_TRUE(parsed_blake3) << parsed_blake3.error().message;
        ASSERT_TRUE(parsed_sha256) << parsed_sha256.error().message;

        const auto resolved_blake3 = liquid::runtime::block_on(store->get(*parsed_blake3));
        const auto resolved_sha256 = liquid::runtime::block_on(store->get(*parsed_sha256));
        ASSERT_TRUE(resolved_blake3) << resolved_blake3.error().message;
        ASSERT_TRUE(resolved_sha256) << resolved_sha256.error().message;
        EXPECT_EQ(*resolved_blake3, blob);
        EXPECT_EQ(*resolved_sha256, blob);
    }
}

TEST(Repository, DripAndChangeSignaturesCarryEd25519Tags) {
    namespace cbor = liquid::encoding::cbor;
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("second", content("second"))));

    const auto journal = journal_document(location.path());
    const auto document = cbor::decode(journal);
    ASSERT_TRUE(document) << document.error().message;
    const auto* drips_value = field(*document, "drips");
    ASSERT_NE(drips_value, nullptr);
    const auto* drips = std::get_if<cbor::value::array>(&drips_value->data);
    ASSERT_NE(drips, nullptr);
    ASSERT_EQ(drips->size(), 2U);
    for (const auto& drip : *drips) {
        const auto* signature_value = field(drip, "sig");
        ASSERT_NE(signature_value, nullptr);
        const auto* signature_bytes = std::get_if<cbor::value::bytes>(&signature_value->data);
        ASSERT_NE(signature_bytes, nullptr);
        const auto signature = cbor::decode(*signature_bytes);
        ASSERT_TRUE(signature) << signature.error().message;
        const auto* algorithm_value = field(*signature, "signature_algorithm");
        ASSERT_NE(algorithm_value, nullptr);
        const auto* algorithm = std::get_if<std::string>(&algorithm_value->data);
        ASSERT_NE(algorithm, nullptr);
        EXPECT_EQ(*algorithm, "ed25519");
    }

    const auto id = liquid::runtime::block_on(repository.cut("signed change"));
    ASSERT_TRUE(id) << id.error().message;
    const auto history = liquid::runtime::block_on(repository.log());
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    const auto signature = cbor::decode(history->front().sig);
    ASSERT_TRUE(signature) << signature.error().message;
    const auto* algorithm_value = field(*signature, "signature_algorithm");
    ASSERT_NE(algorithm_value, nullptr);
    const auto* algorithm = std::get_if<std::string>(&algorithm_value->data);
    ASSERT_NE(algorithm, nullptr);
    EXPECT_EQ(*algorithm, "ed25519");
}

TEST(Repository, RejectsAnUnknownSignatureAlgorithm) {
    namespace cbor = liquid::encoding::cbor;
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("first", content("first"))));

    auto journal = cbor::decode(journal_document(location.path()));
    ASSERT_TRUE(journal) << journal.error().message;
    auto* drips_value = mutable_field(*journal, "drips");
    ASSERT_NE(drips_value, nullptr);
    auto* drips = std::get_if<cbor::value::array>(&drips_value->data);
    ASSERT_NE(drips, nullptr);
    ASSERT_EQ(drips->size(), 1U);
    auto* signature_value = mutable_field(drips->front(), "sig");
    ASSERT_NE(signature_value, nullptr);
    auto* signature_bytes = std::get_if<cbor::value::bytes>(&signature_value->data);
    ASSERT_NE(signature_bytes, nullptr);
    auto signature = cbor::decode(*signature_bytes);
    ASSERT_TRUE(signature) << signature.error().message;
    auto* algorithm = mutable_field(*signature, "signature_algorithm");
    ASSERT_NE(algorithm, nullptr);
    algorithm->data = std::string{"mldsa"};
    const auto encoded_signature = cbor::encode(*signature);
    ASSERT_TRUE(encoded_signature) << encoded_signature.error().message;
    signature_value->data = *encoded_signature;
    const auto encoded_journal = cbor::encode(*journal);
    ASSERT_TRUE(encoded_journal) << encoded_journal.error().message;
    auto output =
        std::ofstream{location.path() / "LIQUID_JOURNAL", std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output.good());
    output.write(reinterpret_cast<const char*>(encoded_journal->data()),
                 static_cast<std::streamsize>(encoded_journal->size()));
    ASSERT_TRUE(output.good());
    output.close();

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto verified = liquid::repository_detail::testing::verify_history(*reopened);
    ASSERT_FALSE(verified.has_value());
    EXPECT_EQ(verified.error().code, liquid::errc::unsupported);
    EXPECT_EQ(verified.error().context, "mldsa");
}

TEST(Repository, split_nest_and_combine_are_composable) {
    temporary_repository parent_path;
    temporary_repository child_path;
    temporary_repository other_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("component"))));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(parent->nest(std::move(*split), "nested")));
    auto other = liquid::runtime::block_on(liquid::repository::create(other_path.path()));
    ASSERT_TRUE(other);
    ASSERT_TRUE(liquid::runtime::block_on(other->write("other", content("other"))));
    ASSERT_TRUE(liquid::runtime::block_on(parent->combine(std::move(*other), "other-repository")));
    auto root = liquid::runtime::block_on(parent->root());
    ASSERT_TRUE(root);
    EXPECT_EQ(root->entries.size(), 3U);
}

TEST(Repository, split_owns_current_and_historical_bytes_after_source_removal) {
    temporary_repository parent_path;
    temporary_repository child_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("first"))));
    const auto first = liquid::runtime::block_on(parent->cut("first version"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("second"))));
    const auto second = liquid::runtime::block_on(parent->cut("second version"));
    ASSERT_TRUE(second);
    const auto original = liquid::runtime::block_on(parent->root());
    ASSERT_TRUE(original);
    const auto subtree = liquid::runtime::block_on(parent->tree(original->entries.front().ref));
    ASSERT_TRUE(subtree);
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    *split = {};
    *parent = {};
    std::filesystem::remove_all(parent_path.path());

    auto reopened = liquid::runtime::block_on(liquid::repository::open(child_path.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(reopened->collect()));
    const auto current = liquid::runtime::block_on(reopened->read("file"));
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(*current, content("second"));
    const auto root = liquid::runtime::block_on(reopened->root());
    ASSERT_TRUE(root);
    ASSERT_EQ(root->entries.size(), 1U);
    EXPECT_EQ(root->entries.front().node, subtree->entries.front().node);
    EXPECT_EQ(liquid::to_string(root->entries.front().ref),
              liquid::to_string(subtree->entries.front().ref));
    const auto history = liquid::runtime::block_on(reopened->history("file"));
    ASSERT_TRUE(history) << history.error().message;
    EXPECT_TRUE(std::ranges::any_of(*history, [&](const auto& entry) {
        return liquid::to_string(entry.change.value) == liquid::to_string(first->value) &&
               entry.message == "first version";
    }));
    const auto log = liquid::runtime::block_on(reopened->log());
    ASSERT_TRUE(log) << log.error().message;
    EXPECT_TRUE(std::ranges::any_of(*log, [&](const auto& entry) {
        return liquid::to_string(entry.id.value) == liquid::to_string(first->value) &&
               entry.message == "first version";
    }));
    EXPECT_TRUE(std::ranges::any_of(*log, [&](const auto& entry) {
        return liquid::to_string(entry.id.value) == liquid::to_string(second->value) &&
               entry.message == "second version";
    }));
    const auto historical = liquid::runtime::block_on(reopened->at(*first));
    ASSERT_TRUE(historical) << historical.error().message;
    const auto old_tree =
        liquid::runtime::block_on(reopened->tree(historical->root.entries.front().ref));
    ASSERT_TRUE(old_tree);
    const auto old_bytes = liquid::runtime::block_on(reopened->blob(old_tree->entries.front().ref));
    ASSERT_TRUE(old_bytes) << old_bytes.error().message;
    EXPECT_EQ(*old_bytes, content("first"));
}

TEST(Repository, split_failure_removes_destination_and_preserves_source) {
    temporary_repository parent_path;
    temporary_repository child_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    auto missing = liquid::repo_ref{};
    missing.id.value.alg = liquid::algorithm::blake3;
    ASSERT_TRUE(liquid::runtime::block_on(parent->nest_ref("component/missing", missing)));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_FALSE(split);
    EXPECT_FALSE(std::filesystem::exists(child_path.path()));
    const auto root = liquid::runtime::block_on(parent->root());
    ASSERT_TRUE(root);
    ASSERT_EQ(root->entries.size(), 1U);
    EXPECT_EQ(root->entries.front().kind, liquid::node_kind::tree);
}

TEST(Repository, split_from_shared_store_reopens_without_the_borrowed_store) {
    temporary_repository shared_path;
    temporary_repository child_path;
    auto shared = liquid::runtime::block_on(liquid::store::open(shared_path.path()));
    ASSERT_TRUE(shared);
    auto options = liquid::repository_options{};
    options.store = &*shared;
    auto parent = liquid::runtime::block_on(liquid::repository::create("parent", options));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("owned"))));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    *split = {};
    *parent = {};
    *shared = {};
    std::filesystem::remove_all(shared_path.path());
    auto reopened = liquid::runtime::block_on(liquid::repository::open(child_path.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto bytes = liquid::runtime::block_on(reopened->read("file"));
    ASSERT_TRUE(bytes) << bytes.error().message;
    EXPECT_EQ(*bytes, content("owned"));
}

TEST(Repository, split_nested_preserves_repository_and_change_identity) {
    temporary_repository parent_path;
    temporary_repository nested_path;
    temporary_repository child_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    auto nested = liquid::runtime::block_on(liquid::repository::create(nested_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(nested);
    const auto nested_identity = nested->id();
    ASSERT_TRUE(nested_identity);
    ASSERT_TRUE(liquid::runtime::block_on(nested->write("asset", content("asset bytes"))));
    auto first = liquid::runtime::block_on(nested->cut("asset history"));
    ASSERT_TRUE(first);
    ASSERT_TRUE(liquid::runtime::block_on(parent->nest(std::move(*nested), "vendor/assets")));
    ASSERT_TRUE(liquid::runtime::block_on(parent->cut("nest")));
    ASSERT_TRUE(liquid::runtime::block_on(parent->collect()));
    std::filesystem::remove_all(nested_path.path());
    *parent = {};
    parent = liquid::runtime::block_on(liquid::repository::open(parent_path.path()));
    ASSERT_TRUE(parent);
    auto split = liquid::runtime::block_on(parent->split("vendor/assets", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    *parent = {};
    std::filesystem::remove_all(parent_path.path());
    *split = {};
    split = liquid::runtime::block_on(liquid::repository::open(child_path.path()));
    ASSERT_TRUE(split);
    ASSERT_TRUE(split->id());
    EXPECT_EQ(*split->id(), *nested_identity);
    auto history = liquid::runtime::block_on(split->log({.first_parent = true}));
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history->size(), 1U);
    EXPECT_EQ(liquid::to_string(history->front().id.value), liquid::to_string(first->value));
    auto bytes = liquid::runtime::block_on(split->read("asset"));
    ASSERT_TRUE(bytes) << bytes.error().message;
    EXPECT_EQ(*bytes, content("asset bytes"));
}

TEST(Repository, split_refuses_existing_destination_without_replacing_it) {
    temporary_repository parent_path;
    temporary_repository child_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("source"))));
    std::filesystem::create_directories(child_path.path());
    const auto sentinel = child_path.path() / "keep";
    std::ofstream{sentinel} << "existing data";
    const auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_FALSE(split);
    EXPECT_EQ(split.error().code, liquid::errc::already_exists);
    auto input = std::ifstream{sentinel};
    EXPECT_EQ((std::string{std::istreambuf_iterator<char>{input}, {}}), "existing data");
    EXPECT_FALSE(std::filesystem::exists(child_path.path() / "LIQUID_REPO"));
}

TEST(Repository, combining_a_split_does_not_duplicate_the_nested_repository_archive) {
    temporary_repository parent_path;
    temporary_repository child_path;
    auto parent = liquid::runtime::block_on(liquid::repository::create(parent_path.path()));
    ASSERT_TRUE(parent);
    ASSERT_TRUE(liquid::runtime::block_on(parent->write("component/file", content("content"))));
    ASSERT_TRUE(liquid::runtime::block_on(parent->cut("content history")));
    auto split = liquid::runtime::block_on(parent->split("component", child_path.path()));
    ASSERT_TRUE(split) << split.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(parent->combine(std::move(*split), "combined")));
    *parent = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(parent_path.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto tree = liquid::runtime::block_on(reopened->root());
    ASSERT_TRUE(tree);
    EXPECT_EQ(tree->entries.size(), 2U);
}

TEST(Repository, combine_collision_is_a_conflict_value) {
    temporary_repository left_path;
    temporary_repository right_path;
    auto left = liquid::runtime::block_on(liquid::repository::create(left_path.path()));
    auto right = liquid::runtime::block_on(liquid::repository::create(right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("same", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("same", content("right"))));
    ASSERT_TRUE(liquid::runtime::block_on(left->combine(std::move(*right), "same")));
    auto conflicts = liquid::runtime::block_on(left->conflicts());
    ASSERT_TRUE(conflicts);
    ASSERT_EQ(conflicts->size(), 1U);
    EXPECT_EQ(conflicts->front().region.path, "same");
    EXPECT_EQ(conflicts->front().kind, liquid::conflict_kind::structural);
    EXPECT_EQ(liquid::to_string(conflicts->front().base),
              liquid::to_string(conflicts->front().left));
    EXPECT_NE(liquid::to_string(conflicts->front().left),
              liquid::to_string(conflicts->front().right));
}

TEST(Repository, cut_loads_only_the_changed_directory_document) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/c/first", content("first"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("a/b/d/second", content("second"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.cut("initial tree")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    liquid::repository_detail::testing::reset_directory_loads(*reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->write("a/b/c/first", content("updated"))));
    EXPECT_EQ(liquid::repository_detail::testing::directory_loads(*reopened), 1U);
}

TEST(Repository, undo_move_restores_the_original_node_identity) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("before", content("value"))));
    auto root = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(root);
    const auto identity = root->entries.front().node;
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("before", "after")));
    ASSERT_TRUE(liquid::runtime::block_on(
        repository.undo(liquid::repository_detail::testing::last_operation(repository))));
    auto restored = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(restored);
    ASSERT_EQ(restored->entries.size(), 1U);
    EXPECT_EQ(restored->entries.front().name, "before");
    EXPECT_EQ(restored->entries.front().node, identity);
}

TEST(Repository, operations_are_newest_first_with_their_metadata) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("before", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("before", "after")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    if (!reopened) {
        ADD_FAILURE() << reopened.error().context << ": " << reopened.error().message;
        return;
    }

    auto operations = liquid::runtime::block_on(reopened->operations());
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 2U);
    EXPECT_NE((*operations)[0].id.value.bytes, (*operations)[1].id.value.bytes);
    EXPECT_EQ(operations->front().kind, "move");
    EXPECT_EQ(operations->front().from, "before");
    EXPECT_EQ(operations->front().to, "after");
    EXPECT_FALSE(operations->front().by.taap_id.empty());
    EXPECT_GT(operations->front().at.time_since_epoch().count(), 0);
    EXPECT_EQ(operations->back().kind, "write");
}

TEST(Repository, undo_by_operation_id_reverses_write_record_and_lock) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), exclusive_checkout_options()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("drip.txt", content("value"))));
    const auto write = operation_with_kind(repository, "write");
    ASSERT_TRUE(write) << write.error().message;
    const auto write_undo = liquid::runtime::block_on(repository.undo(write->id));
    ASSERT_TRUE(write_undo) << write_undo.error().message;
    ASSERT_EQ(write_undo->applied.size(), 1U);
    EXPECT_FALSE(liquid::runtime::block_on(repository.read("drip.txt")));

    ASSERT_TRUE(liquid::runtime::block_on(repository.write("drip.txt", content("base"))));
    const auto base = liquid::runtime::block_on(repository.cut("base"));
    ASSERT_TRUE(base) << base.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("drip.txt", content("reparented"))));
    const auto rebased = liquid::runtime::block_on(repository.cut("reparent"));
    ASSERT_TRUE(rebased) << rebased.error().message;
    const auto record = operation_with_kind(repository, "record");
    ASSERT_TRUE(record) << record.error().message;
    const auto record_undo = liquid::runtime::block_on(repository.undo(record->id));
    ASSERT_TRUE(record_undo) << record_undo.error().message;
    const auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs) << refs.error().message;
    const auto main = std::ranges::find(*refs, "main", &liquid::ref::name);
    ASSERT_NE(main, refs->end());
    EXPECT_EQ(main->id.value.bytes, base->value.bytes);

    ASSERT_TRUE(liquid::runtime::block_on(repository.lock("drip.txt")));
    const auto lock = operation_with_kind(repository, "lock");
    ASSERT_TRUE(lock) << lock.error().message;
    const auto lock_undo = liquid::runtime::block_on(repository.undo(lock->id));
    ASSERT_TRUE(lock_undo) << lock_undo.error().message;
    ASSERT_EQ(lock_undo->applied.size(), 1U);
    EXPECT_TRUE(
        liquid::runtime::block_on(repository.write("drip.txt", content("after lock undo"))));
}

TEST(Repository, undo_by_operation_id_reverses_import_merge) {
    temporary_repository source_location;
    temporary_repository destination_location;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_location.path()));
    auto destination =
        liquid::runtime::block_on(liquid::repository::create(destination_location.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(destination) << destination.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("remote.txt", content("remote"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("remote")));
    const auto items = exported_items(*source);
    ASSERT_TRUE(items) << items.error().message;
    const auto imported = liquid::runtime::block_on(destination->import_drips(*items));
    ASSERT_TRUE(imported) << imported.error().message;
    const auto before_undo = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(before_undo) << before_undo.error().message;
    EXPECT_EQ(before_undo->entries.size(), 1U);

    const auto merge = operation_with_kind(*destination, "merge");
    ASSERT_TRUE(merge) << merge.error().message;
    const auto undone = liquid::runtime::block_on(destination->undo(merge->id));
    ASSERT_TRUE(undone) << undone.error().message;
    ASSERT_EQ(undone->applied.size(), 1U);
    const auto after_undo = liquid::runtime::block_on(destination->root());
    ASSERT_TRUE(after_undo) << after_undo.error().message;
    EXPECT_TRUE(after_undo->entries.empty());
}

TEST(Repository, undo_skips_a_path_changed_by_a_newer_operation) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file.txt", content("first"))));
    const auto first = operation_with_kind(repository, "write");
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file.txt", content("second"))));

    const auto undone = liquid::runtime::block_on(repository.undo(first->id));
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_TRUE(undone->applied.empty());
    ASSERT_EQ(undone->skipped.size(), 1U);
    EXPECT_EQ(undone->skipped.front().path, "file.txt");
    const auto current = liquid::runtime::block_on(repository.read("file.txt"));
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_EQ(text(*current), "second");
}

TEST(Repository, local_undo_preserves_a_concurrent_remote_write) {
    temporary_repository source_location;
    temporary_repository left_location;
    temporary_repository right_location;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_location.path()));
    ASSERT_TRUE(source) << source.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, left_location.path()));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, right_location.path()));
    ASSERT_TRUE(left) << left.error().message;
    ASSERT_TRUE(right) << right.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("left"))));
    const auto local_write = operation_with_kind(*left, "write");
    ASSERT_TRUE(local_write) << local_write.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(right->write("b.txt", content("right"))));
    auto synchronized =
        liquid::runtime::block_on(liquid::repository_detail::testing::sync(*left, *right));
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;

    const auto undone = liquid::runtime::block_on(left->undo(local_write->id));
    ASSERT_TRUE(undone) << undone.error().message;
    ASSERT_EQ(undone->applied.size(), 1U);
    EXPECT_FALSE(liquid::runtime::block_on(left->read("a.txt")));
    synchronized =
        liquid::runtime::block_on(liquid::repository_detail::testing::sync(*left, *right));
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;
    EXPECT_FALSE(liquid::runtime::block_on(left->read("a.txt")));
    EXPECT_FALSE(liquid::runtime::block_on(right->read("a.txt")));
    const auto left_remote = liquid::runtime::block_on(left->read("b.txt"));
    const auto right_remote = liquid::runtime::block_on(right->read("b.txt"));
    ASSERT_TRUE(left_remote) << left_remote.error().message;
    ASSERT_TRUE(right_remote) << right_remote.error().message;
    EXPECT_EQ(text(*left_remote), "right");
    EXPECT_EQ(text(*right_remote), "right");
}

TEST(Repository, session_undo_reverses_all_writes_or_one_file) {
    temporary_repository location;
    const auto base_session = test_session(std::byte{1});
    const auto work_session = test_session(std::byte{2});
    auto base_options = liquid::repository_options{};
    base_options.session = base_session;
    auto created = liquid::runtime::block_on(
        liquid::repository::create(location.path(), std::move(base_options)));
    ASSERT_TRUE(created) << created.error().message;
    for (auto file = 0; file < 5; ++file) {
        ASSERT_TRUE(liquid::runtime::block_on(created->write(
            "file-" + std::to_string(file), content("base-" + std::to_string(file)))));
    }
    *created = {};
    auto work_options = liquid::repository_options{};
    work_options.session = work_session;
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(location.path(), work_options));
    ASSERT_TRUE(repository) << repository.error().message;
    for (auto write = 0; write < 40; ++write) {
        const auto file = write % 5;
        ASSERT_TRUE(liquid::runtime::block_on(repository->write(
            "file-" + std::to_string(file), content("write-" + std::to_string(write)))));
    }
    const auto undone = liquid::runtime::block_on(repository->undo_session(work_session));
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_EQ(undone->applied.size(), 40U);
    for (auto file = 0; file < 5; ++file) {
        const auto value =
            liquid::runtime::block_on(repository->read("file-" + std::to_string(file)));
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_EQ(text(*value), "base-" + std::to_string(file));
    }

    const auto selective_session = test_session(std::byte{3});
    auto selective_options = liquid::repository_options{};
    selective_options.session = selective_session;
    *repository = {};
    repository = liquid::runtime::block_on(
        liquid::repository::open(location.path(), std::move(selective_options)));
    ASSERT_TRUE(repository) << repository.error().message;
    for (auto write = 0; write < 40; ++write) {
        const auto file = write % 5;
        ASSERT_TRUE(liquid::runtime::block_on(repository->write(
            "file-" + std::to_string(file), content("selective-" + std::to_string(write)))));
    }
    const auto selective = liquid::runtime::block_on(
        repository->undo_session(selective_session, liquid::undo_options{.paths = {"file-2"}}));
    ASSERT_TRUE(selective) << selective.error().message;
    EXPECT_EQ(selective->applied.size(), 8U);
    const auto restored = liquid::runtime::block_on(repository->read("file-2"));
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(text(*restored), "base-2");
    const auto untouched = liquid::runtime::block_on(repository->read("file-1"));
    ASSERT_TRUE(untouched) << untouched.error().message;
    EXPECT_EQ(text(*untouched), "selective-36");
}

TEST(Repository, document_at_returns_historical_references) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("first"))));
    const auto first = liquid::runtime::block_on(repository.cut("first"));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository.set_ref("saved", *first)));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("file", content("second"))));
    const auto second = liquid::runtime::block_on(repository.cut("second"));
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(repository.set_ref("saved", *second)));

    const auto historical = liquid::runtime::block_on(repository.document_at(*second));
    ASSERT_TRUE(historical) << historical.error().message;
    const auto saved = historical->refs.find("saved");
    ASSERT_NE(saved, historical->refs.end());
    EXPECT_EQ(saved->second.value.bytes, first->value.bytes);
}

TEST(Repository, cross_directory_move_is_recovered_before_cut) {
    temporary_repository location;
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(created);
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("from/file", content("value"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("to/anchor", content("anchor"))));
    ASSERT_TRUE(liquid::runtime::block_on(repository.move("from/file", "to/file")));
    repository = {};

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    ASSERT_TRUE(liquid::runtime::block_on(reopened->cut("recover cross-directory move")));
    auto moved = liquid::runtime::block_on(reopened->read("to/file"));
    ASSERT_TRUE(moved);
    EXPECT_EQ(text(*moved), "value");
}

TEST(Repository, synchronized_replicas_turn_divergent_writes_into_a_conflict_value) {
    temporary_repository source_path;
    temporary_repository left_path;
    temporary_repository right_path;
    auto source = liquid::runtime::block_on(liquid::repository::create(source_path.path()));
    ASSERT_TRUE(source) << source.error().message << ": " << source.error().context;
    ASSERT_TRUE(liquid::runtime::block_on(source->write("a.txt", content("base"))));
    ASSERT_TRUE(liquid::runtime::block_on(source->cut("base")));
    auto left = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, left_path.path()));
    auto right = liquid::runtime::block_on(
        liquid::repository_detail::testing::clone(*source, right_path.path()));
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("left"))));
    ASSERT_TRUE(liquid::runtime::block_on(right->write("a.txt", content("right"))));
    auto synchronized =
        liquid::runtime::block_on(liquid::repository_detail::testing::sync(*left, *right));
    ASSERT_TRUE(synchronized) << synchronized.error().message << ": "
                              << synchronized.error().context;

    auto left_conflicts = liquid::runtime::block_on(left->conflicts());
    auto right_conflicts = liquid::runtime::block_on(right->conflicts());
    ASSERT_TRUE(left_conflicts);
    ASSERT_TRUE(right_conflicts);
    ASSERT_EQ(left_conflicts->size(), 1U);
    ASSERT_EQ(right_conflicts->size(), 1U);
    EXPECT_EQ(left_conflicts->front().region.path, "a.txt");
    EXPECT_EQ(left_conflicts->front().kind, liquid::conflict_kind::text);
    ASSERT_TRUE(left_conflicts->front().region.bytes);
    EXPECT_EQ(left_conflicts->front().region.bytes->offset, 0U);
    EXPECT_EQ(liquid::to_string(left_conflicts->front().id),
              liquid::to_string(right_conflicts->front().id));
    EXPECT_EQ(liquid::to_string(left_conflicts->front().base),
              liquid::to_string(right_conflicts->front().base));
    EXPECT_EQ(liquid::to_string(left_conflicts->front().left),
              liquid::to_string(right_conflicts->front().left));
    EXPECT_EQ(liquid::to_string(left_conflicts->front().right),
              liquid::to_string(right_conflicts->front().right));

    auto cut = liquid::runtime::block_on(left->cut("record conflict"));
    ASSERT_TRUE(cut);
    auto history = liquid::runtime::block_on(left->log());
    ASSERT_TRUE(history);
    ASSERT_FALSE(history->empty());
    EXPECT_EQ(history->front().conflicts.size(), 1U);
    EXPECT_EQ(liquid::to_string(history->front().conflicts.front().id),
              liquid::to_string(left_conflicts->front().id));
    auto conflicted_tree = liquid::runtime::block_on(left->tree(history->front().root));
    ASSERT_TRUE(conflicted_tree);
    ASSERT_EQ(conflicted_tree->entries.size(), 1U);
    EXPECT_EQ(conflicted_tree->entries.front().kind, liquid::node_kind::conflict);
    EXPECT_EQ(liquid::to_string(conflicted_tree->entries.front().ref),
              liquid::to_string(left_conflicts->front().id));
    auto reopened = liquid::runtime::block_on(liquid::repository::open(left_path.path()));
    ASSERT_TRUE(reopened);
    auto reopened_conflicts = liquid::runtime::block_on(reopened->conflicts());
    ASSERT_TRUE(reopened_conflicts);
    ASSERT_EQ(reopened_conflicts->size(), 1U);
    EXPECT_EQ(reopened_conflicts->front().region.path, "a.txt");
    EXPECT_EQ(liquid::to_string(reopened_conflicts->front().id),
              liquid::to_string(left_conflicts->front().id));
    ASSERT_TRUE(liquid::runtime::block_on(left->write("a.txt", content("resolved"))));
    *left = {};
    *reopened = {};
    auto edited = liquid::runtime::block_on(liquid::repository::open(left_path.path()));
    ASSERT_TRUE(edited);
    auto edited_conflicts = liquid::runtime::block_on(edited->conflicts());
    ASSERT_TRUE(edited_conflicts);
    ASSERT_EQ(edited_conflicts->size(), 1U);
    auto edited_contents = liquid::runtime::block_on(edited->read("a.txt"));
    ASSERT_TRUE(edited_contents);
    EXPECT_EQ(text(*edited_contents), "resolved");
    auto edited_status = edited->status();
    ASSERT_TRUE(edited_status);
    ASSERT_EQ(edited_status->conflicts.size(), 1U);
    const auto selected_resolution = edited_status->conflicts.front().selected;
    auto resolution = liquid::runtime::block_on(edited->resolve_conflict("a.txt"));
    ASSERT_TRUE(resolution);
    auto resolved = liquid::runtime::block_on(edited->conflicts());
    ASSERT_TRUE(resolved);
    EXPECT_TRUE(resolved->empty());
    auto resolved_history = liquid::runtime::block_on(edited->log());
    ASSERT_TRUE(resolved_history);
    ASSERT_EQ(resolved_history->front().resolutions.size(), 1U);
    EXPECT_EQ(liquid::to_string(resolved_history->front().resolutions.front().conflict),
              liquid::to_string(left_conflicts->front().id));
    EXPECT_EQ(liquid::to_string(resolved_history->front().resolutions.front().value),
              liquid::to_string(selected_resolution));
}

TEST(Repository, WriteFileMatchesWriteBlobAndRecordsOneDrip) {
    temporary_repository source_path;
    temporary_repository memory_path;
    temporary_repository file_path;
    auto bytes = std::vector<std::byte>(10U * 1024U * 1024U + 91U);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = std::byte{static_cast<unsigned char>((index * 37U + 11U) & 0xffU)};
    const auto input = source_path.path() / "input.bin";
    write_file(input, bytes);

    auto memory_created = liquid::runtime::block_on(liquid::repository::create(memory_path.path()));
    auto file_created = liquid::runtime::block_on(liquid::repository::create(file_path.path()));
    ASSERT_TRUE(memory_created) << memory_created.error().message;
    ASSERT_TRUE(file_created) << file_created.error().message;
    auto memory_repository = std::move(*memory_created);
    auto file_repository = std::move(*file_created);
    const auto memory_before = memory_repository.status()->pending_drips;
    const auto file_before = file_repository.status()->pending_drips;
    ASSERT_TRUE(liquid::runtime::block_on(memory_repository.write("input.bin", bytes)));
    ASSERT_TRUE(liquid::runtime::block_on(file_repository.write_file("input.bin", input)));
    EXPECT_EQ(memory_repository.status()->pending_drips, memory_before + 1);
    EXPECT_EQ(file_repository.status()->pending_drips, file_before + 1);

    const auto memory_root = liquid::runtime::block_on(memory_repository.root());
    const auto file_root = liquid::runtime::block_on(file_repository.root());
    ASSERT_TRUE(memory_root) << memory_root.error().message;
    ASSERT_TRUE(file_root) << file_root.error().message;
    ASSERT_EQ(memory_root->entries.size(), 1U);
    ASSERT_EQ(file_root->entries.size(), 1U);
    EXPECT_EQ(memory_root->entries.front().id.alg, file_root->entries.front().id.alg);
    EXPECT_EQ(memory_root->entries.front().id.bytes, file_root->entries.front().id.bytes);
    const auto read = liquid::runtime::block_on(file_repository.read("input.bin"));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(*read, bytes);
}

TEST(Repository, RemoteConfigurationPersistsAcrossOpen) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.remote = liquid::remote_config{.endpoint = "https://cache.example.test:443",
                                           .instance_name = "liquid",
                                           .tls = {.enabled = true,
                                                   .verify_peer = true,
                                                   .certificate_authority = "ca.pem",
                                                   .client_certificate = "client.pem",
                                                   .client_key = "client-key.pem"},
                                           .headers = {{"authorization", "Bearer test-token"}},
                                           .deadline = std::chrono::milliseconds{2500},
                                           .batch_blob_limit = 1024,
                                           .publish = true,
                                           .backend = liquid::remote_backend::liquid_scheduler};
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(created->remote_configuration());
    EXPECT_EQ(*created->remote_configuration(), *options.remote);

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_TRUE(reopened->remote_configuration());
    EXPECT_EQ(*reopened->remote_configuration(), *options.remote);
}

TEST(Repository, HotUpdateConfigurationPersistsAcrossOpen) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.hot_update.restart_hook = {"/usr/bin/env", "restart-demo"};
    options.hot_update.targets = {{"demo", location.path() / "demo.sock"}};
    auto created = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->hot_update_configuration(), options.hot_update);

    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened->hot_update_configuration(), options.hot_update);
}

} // namespace
