#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "remote_blob_source.hpp"

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-build-test-" + std::to_string(next_++))) {
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

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto environment_entries(std::string_view value) -> std::map<std::string, std::string> {
    auto output = std::map<std::string, std::string>{};
    auto offset = std::size_t{};
    while (offset < value.size()) {
        const auto newline = value.find('\n', offset);
        const auto line = value.substr(offset, newline - offset);
        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            output.emplace(std::string{line.substr(0, separator)},
                           std::string{line.substr(separator + 1)});
        }
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return output;
}

struct action_fixture {
    temporary_repository location;
    liquid::repository repository;
    liquid::digest root;

    explicit action_fixture(bool publish = true,
                            std::uint32_t verification_rate = 20,
                            bool retain_results = true,
                            std::shared_ptr<liquid::remote_blob_source> remote = {}) {
        auto options = liquid::repository_options{};
        options.cache.publish = publish;
        options.verify.rate = verification_rate;
        if (!retain_results) {
            std::erase_if(options.retention.rules, [](const auto& rule) {
                return std::holds_alternative<liquid::result_retention_rule>(rule);
            });
        }
        if (remote) {
            options.storage.remote_sources.emplace("disk", std::move(remote));
        }
        auto opened =
            liquid::runtime::block_on(liquid::repository::create(location.path(), options));
        EXPECT_TRUE(opened) << opened.error().message;
        repository = std::move(*opened);
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("input.txt", bytes("input"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("other.txt", bytes("other"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/copy.txt", {})));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/env.txt", {})));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/random.bin", {})));
        EXPECT_TRUE(
            liquid::runtime::block_on(repository.write_file("bin/fixture", LIQUID_BUILD_FIXTURE)));
        EXPECT_TRUE(liquid::runtime::block_on(
            liquid::repository_detail::vfs_access::set_mode(repository, "bin/fixture", 0100755)));
        EXPECT_TRUE(liquid::runtime::block_on(repository.cut("build fixture")));
        auto head = liquid::runtime::block_on(repository.head());
        EXPECT_TRUE(head);
        auto snapshot = liquid::runtime::block_on(repository.at(*head));
        EXPECT_TRUE(snapshot);
        auto log = liquid::runtime::block_on(repository.log());
        EXPECT_TRUE(log);
        root = log->front().root;
    }

    void cut(std::string message) {
        EXPECT_TRUE(liquid::runtime::block_on(repository.cut(std::move(message))));
        auto log = liquid::runtime::block_on(repository.log());
        ASSERT_TRUE(log);
        root = log->front().root;
    }

    auto input(std::vector<std::string> command, std::vector<std::string> outputs)
        -> liquid::action_input {
        return {.command = std::move(command),
                .cwd = "",
                .env = {{"ACTION_VALUE", "present"}},
                .outputs = std::move(outputs),
                .inputs = root,
                .toolchain = root,
                .platform = host(),
                .timeout = std::chrono::seconds{3}};
    }

    static auto host() -> liquid::platform {
#if defined(__linux__)
#if defined(__aarch64__)
        return {"linux", "arm64"};
#else
        return {"linux", "x86_64"};
#endif
#elif defined(__APPLE__)
        return {"macos", "arm64"};
#else
        return {"windows", "x86_64"};
#endif
    }
};

auto example_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

TEST(BuildCache, action_index_survives_reopen_and_rebuild) {
    temporary_repository location;
    auto index = liquid::build_detail::action_index{location.path()};
    ASSERT_TRUE(liquid::runtime::block_on(index.open()));
    ASSERT_TRUE(index.insert(example_digest(1), example_digest(2)));
    ASSERT_TRUE(liquid::runtime::block_on(index.persist(example_digest(3))));

    auto reopened = liquid::build_detail::action_index{location.path()};
    ASSERT_TRUE(liquid::runtime::block_on(reopened.open()));
    ASSERT_EQ(reopened.candidates(example_digest(1)).size(), 1U);
    EXPECT_EQ(liquid::to_string(reopened.candidates(example_digest(1)).front()),
              liquid::to_string(example_digest(2)));

    auto rebuilt_entries = reopened.entries();
    std::filesystem::remove(location.path() / "INDEX");
    auto rebuilt = liquid::build_detail::action_index{location.path()};
    ASSERT_TRUE(
        liquid::runtime::block_on(rebuilt.rebuild(std::move(rebuilt_entries), example_digest(3))));
    ASSERT_EQ(rebuilt.candidates(example_digest(1)).size(), 1U);
}

TEST(BuildCache, publish_is_refused_by_the_read_only_default) {
    temporary_repository location;
    auto opened = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(opened);
    auto cache = liquid::build_detail::disk_cache{*opened};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
    auto published = liquid::runtime::block_on(cache.publish(
        example_digest(1), liquid::result_id{example_digest(2)}, liquid::action_result{}));
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, liquid::errc::unauthorized);
}

TEST(BuildCache, unsigned_publish_is_refused_when_writes_are_enabled) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.cache.publish = true;
    auto opened = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(opened);
    auto cache = liquid::build_detail::disk_cache{*opened};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
    auto published = liquid::runtime::block_on(cache.publish(
        example_digest(1), liquid::result_id{example_digest(2)}, liquid::action_result{}));
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(published.error().message, "cache result signature is invalid");
}

TEST(BuildCache, size_cap_evicts_a_cheap_large_result_before_an_expensive_small_result) {
    temporary_repository location;
    auto options = liquid::repository_options{};
    options.retention.rules = {
        liquid::result_retention_rule{"cost weighted results", std::chrono::days{1}},
        liquid::size_cap_retention_rule{"local cache cap", 1024U * 1024U}};
    auto opened = liquid::runtime::block_on(liquid::repository::create(location.path(), options));
    ASSERT_TRUE(opened) << opened.error().message;
    *opened = {};
    auto object_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(object_store) << object_store.error().message;

    const auto put_blob = [&](std::size_t size, unsigned char seed) -> liquid::digest {
        auto payload = std::vector<std::byte>(size);
        auto random = std::mt19937_64{seed};
        for (auto& byte : payload) {
            byte = std::byte{static_cast<unsigned char>(random())};
        }
        auto payload_id = liquid::runtime::block_on(object_store->put(std::move(payload)));
        EXPECT_TRUE(payload_id) << payload_id.error().message;
        auto encoded = liquid::objects::encode(
            liquid::blob{{*payload_id}, static_cast<std::uint64_t>(size), {size}});
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto blob_id = liquid::runtime::block_on(object_store->put(std::move(*encoded)));
        EXPECT_TRUE(blob_id) << blob_id.error().message;
        return *blob_id;
    };
    const auto empty_blob = put_blob(0, 0);
    const auto cheap_blob = put_blob(2U * 1024U * 1024U, 11);
    const auto expensive_blob = put_blob(16U * 1024U, 29);
    const auto put_result = [&](liquid::digest output,
                                std::chrono::milliseconds cost,
                                unsigned char action_byte) -> liquid::digest {
        auto value = liquid::action_result{};
        value.action = example_digest(action_byte);
        value.outputs.push_back({"output.bin", 0100644, output});
        value.stdout_output = empty_blob;
        value.stderr_output = empty_blob;
        value.timing.exec = cost;
        value.observed_at =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto encoded = liquid::objects::encode(value);
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto id = liquid::runtime::block_on(object_store->put(std::move(*encoded)));
        EXPECT_TRUE(id) << id.error().message;
        return *id;
    };
    const auto cheap_result = put_result(cheap_blob, std::chrono::milliseconds{1}, 41);
    const auto expensive_result = put_result(expensive_blob, std::chrono::minutes{40}, 42);

    const auto action_directory = location.path() / ".liquid-store" / "actions";
    auto index = liquid::build_detail::action_index{action_directory};
    ASSERT_TRUE(liquid::runtime::block_on(index.open()));
    ASSERT_TRUE(index.insert(example_digest(51), cheap_result));
    ASSERT_TRUE(index.insert(example_digest(52), expensive_result));
    auto root_record = liquid::objects::encode(liquid::objects::action_index_root{index.entries()});
    ASSERT_TRUE(root_record) << root_record.error().message;
    auto root = liquid::runtime::block_on(object_store->put(std::move(*root_record)));
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(index.persist(*root)));
    std::ofstream pointer(action_directory / "ROOT", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(pointer.is_open());
    pointer << liquid::to_string(*root) << '\n';
    pointer.close();
    *object_store = {};

    opened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    auto cache = liquid::build_detail::disk_cache{*opened};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));

    auto report = liquid::runtime::block_on(opened->collect());
    ASSERT_TRUE(report) << report.error().message;
    *opened = {};
    object_store =
        liquid::runtime::block_on(liquid::store::open(location.path() / ".liquid-store"));
    ASSERT_TRUE(object_store) << object_store.error().message;
    auto cheap_present = liquid::runtime::block_on(object_store->contains(cheap_result));
    auto expensive_present = liquid::runtime::block_on(object_store->contains(expensive_result));
    ASSERT_TRUE(cheap_present);
    ASSERT_TRUE(expensive_present);
    EXPECT_FALSE(*cheap_present);
    EXPECT_TRUE(*expensive_present);
    EXPECT_EQ(report->retained_by_rule["cost weighted results"], 1U);
}

#if LIQUID_BUILD_HAS_FUSE
auto blob_contents(liquid::repository& repository, liquid::digest id)
    -> liquid::result<std::vector<std::byte>> {
    auto record =
        liquid::runtime::block_on(liquid::repository_detail::build_access::get(repository, id));
    if (!record) {
        return std::unexpected(record.error());
    }
    auto blob = liquid::objects::decode_blob(*record);
    if (!blob) {
        return std::unexpected(blob.error());
    }
    if (blob->chunks.size() != 1U) {
        return std::unexpected(
            liquid::error{liquid::errc::corrupt, "test output has multiple chunks", "build test"});
    }
    return liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(repository, blob->chunks.front()));
}

TEST(Build, copies_declared_output_and_records_only_read_input) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->exit, 0);
    ASSERT_EQ(result->outputs.size(), 1U);
    EXPECT_EQ(result->outputs.front().path, "out/copy.txt");
    ASSERT_EQ(result->manifest.size(), 1U);
    EXPECT_EQ(result->manifest.front().path, "input.txt");
}

TEST(Build, records_an_undeclared_input_read) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "other.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->manifest.size(), 1U);
    EXPECT_EQ(result->manifest.front().path, "other.txt");
}

TEST(Build, denies_reads_outside_the_mount) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(
        liquid::build::execute(fixture.repository, fixture.input({"fixture", "outside-read"}, {})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->exit, 0);
    EXPECT_TRUE(result->manifest.empty());
}

#if defined(__linux__)
TEST(Build, remote_eligible_action_reads_inputs_and_toolchain) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    input.remote_eligible = true;
    auto ran = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->exit, 0);
}

TEST(Build, refuses_non_hermetic_read_for_remote_eligible_action) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "outside-read"}, {});
    input.remote_eligible = true;
    auto ran = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_FALSE(ran);
    EXPECT_EQ(ran.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(ran.error().message, "remote-eligible action read outside its inputs");
    EXPECT_EQ(ran.error().context, "/etc/hostname");
}
#endif

TEST(Build, rejects_writes_outside_declared_outputs) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "outside-write"}, {})));
    ASSERT_FALSE(ran);
    EXPECT_EQ(ran.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(ran.error().message, "action wrote outside declared outputs");
}

TEST(Build, has_fixed_environment) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "env", "out/env.txt"}, {"out/env.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->exit, 0);
    ASSERT_EQ(result->outputs.size(), 1U);
    auto output = blob_contents(fixture.repository, result->outputs.front().blob);
    ASSERT_TRUE(output) << output.error().message;
    auto source_time = liquid::runtime::block_on(
        liquid::repository_detail::build_access::change_time(fixture.repository, fixture.root));
    ASSERT_TRUE(source_time) << source_time.error().message;
    EXPECT_EQ(environment_entries(text(*output)),
              (std::map<std::string, std::string>{
                  {"ACTION_VALUE", "present"},
                  {"HOME", "/liquid/home/" + liquid::to_string(result->action).substr(7)},
                  {"LANG", "C.UTF-8"},
                  {"PATH", "/liquid/toolchain/" + liquid::to_string(fixture.root) + "/bin"},
                  {"SOURCE_DATE_EPOCH", std::to_string(source_time->time_since_epoch().count())},
                  {"TMPDIR", "/liquid/tmp/" + liquid::to_string(result->action).substr(7)},
                  {"TZ", "UTC"},
              }));
}

TEST(Build, preserves_pure_action_results_except_timing) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    auto first_result =
        liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    auto second_result =
        liquid::runtime::block_on(liquid::build::result(fixture.repository, *second));
    ASSERT_TRUE(first_result);
    ASSERT_TRUE(second_result);
    EXPECT_EQ(liquid::to_string(first_result->action), liquid::to_string(second_result->action));
    ASSERT_EQ(first_result->outputs.size(), second_result->outputs.size());
    EXPECT_EQ(first_result->outputs.front().path, second_result->outputs.front().path);
    EXPECT_EQ(liquid::to_string(first_result->outputs.front().blob),
              liquid::to_string(second_result->outputs.front().blob));
    ASSERT_EQ(first_result->manifest.size(), second_result->manifest.size());
    EXPECT_EQ(first_result->manifest.front().path, second_result->manifest.front().path);
    EXPECT_EQ(liquid::to_string(first_result->manifest.front().content),
              liquid::to_string(second_result->manifest.front().content));
}

TEST(BuildCache, identical_second_execution_is_a_hit_without_a_spawn) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto first_value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    ASSERT_TRUE(first_value);
    ASSERT_TRUE(first_value->provenance);
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::to_string(first->value), liquid::to_string(second->value));
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 0U);
    auto value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *second));
    ASSERT_TRUE(value);
    ASSERT_TRUE(value->provenance);
    EXPECT_EQ(liquid::to_string(*value->provenance), liquid::to_string(*first_value->provenance));
    EXPECT_EQ(value->cache, liquid::cache_status::hit);
    auto provenance =
        liquid::runtime::block_on(liquid::build::verify_provenance(fixture.repository, *second));
    ASSERT_TRUE(provenance) << provenance.error().message;
    EXPECT_TRUE(provenance->verified) << provenance->reason;
    auto statistics = liquid::runtime::block_on(fixture.repository.cache_stats());
    ASSERT_TRUE(statistics);
    EXPECT_EQ(statistics->hits, 1U);
    EXPECT_EQ(statistics->misses, 1U);
    EXPECT_GT(statistics->bytes_served, 0U);
    EXPECT_GT(statistics->bytes_stored, 0U);
}

TEST(BuildCache, changing_an_undeclared_read_header_names_both_digests) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "copy", "other.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto first_value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    ASSERT_TRUE(first_value);
    ASSERT_EQ(first_value->manifest.size(), 1U);
    const auto expected = first_value->manifest.front().content;

    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.write("other.txt", bytes("changed"))));
    fixture.cut("change observed header");
    input.inputs = fixture.root;
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 1U);
    auto second_value =
        liquid::runtime::block_on(liquid::build::result(fixture.repository, *second));
    ASSERT_TRUE(second_value);
    ASSERT_TRUE(second_value->why);
    EXPECT_EQ(second_value->why->path, "other.txt");
    ASSERT_TRUE(second_value->why->expected);
    ASSERT_TRUE(second_value->why->actual);
    EXPECT_EQ(liquid::to_string(*second_value->why->expected), liquid::to_string(expected));
    EXPECT_EQ(liquid::to_string(*second_value->why->actual),
              liquid::to_string(second_value->manifest.front().content));
    EXPECT_NE(liquid::to_string(*second_value->why->expected),
              liquid::to_string(*second_value->why->actual));
}

TEST(BuildCache, changing_an_unread_input_remains_a_hit) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.write("other.txt", bytes("changed"))));
    fixture.cut("change unread input");
    input.inputs = fixture.root;
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::to_string(first->value), liquid::to_string(second->value));
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 0U);
}

TEST(BuildCache, collected_output_is_not_served) {
    action_fixture fixture{true, 20, false};
    const auto input =
        fixture.input({"fixture", "copy-tagged", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto first_value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    ASSERT_TRUE(first_value);
    ASSERT_EQ(first_value->outputs.size(), 1U);
    const auto output_blob = first_value->outputs.front().blob;
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::build_detail::testing::release_cache_root(fixture.repository)));
    auto collected = liquid::runtime::block_on(fixture.repository.collect());
    ASSERT_TRUE(collected) << collected.error().message;
    auto output_present = liquid::runtime::block_on(
        liquid::repository_detail::build_access::contains(fixture.repository, output_blob));
    ASSERT_TRUE(output_present);
    EXPECT_FALSE(*output_present);
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 1U);
}

TEST(BuildCache, result_signature_verifies_against_the_device_identity) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto raw = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(fixture.repository, ran->value));
    ASSERT_TRUE(raw);
    auto value = liquid::objects::decode_result(*raw);
    ASSERT_TRUE(value);
    auto body = liquid::objects::encode_result_body(*value);
    ASSERT_TRUE(body);
    auto signature = liquid::identity_detail::parse_signature(value->sig);
    ASSERT_TRUE(signature);
    auto verified = liquid::verify(value->producer, "result", *body, *signature);
    ASSERT_TRUE(verified);
    EXPECT_TRUE(*verified);
}

TEST(BuildVerification, pure_action_gets_a_signed_reproducibility_attestation) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto report = liquid::runtime::block_on(
        liquid::build::verify(fixture.repository, {.selection = liquid::verify_selection::all}));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->actions.size(), 1U);
    EXPECT_TRUE(report->actions.front().agreement);
    auto raw = liquid::runtime::block_on(liquid::repository_detail::build_access::get(
        fixture.repository, report->actions.front().attestation));
    ASSERT_TRUE(raw) << raw.error().message;
    auto attestation = liquid::objects::decode_attestation(*raw);
    ASSERT_TRUE(attestation) << attestation.error().message;
    EXPECT_EQ(attestation->profile, "reproducibility/1");
    auto verified = liquid::verify_attestation(*attestation);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;
}

TEST(BuildReplay, recorded_failure_reproduces_outputs_and_exit_status) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "fail", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto recorded = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(recorded) << recorded.error().message;
    ASSERT_EQ(recorded->exit, 37);
    ASSERT_EQ(recorded->outputs.size(), 1U);
    auto stdout_bytes = blob_contents(fixture.repository, recorded->stdout_output);
    auto stderr_bytes = blob_contents(fixture.repository, recorded->stderr_output);
    ASSERT_TRUE(stdout_bytes) << stdout_bytes.error().message;
    ASSERT_TRUE(stderr_bytes) << stderr_bytes.error().message;
    EXPECT_EQ(text(*stdout_bytes), "recorded failure stdout\n");
    EXPECT_EQ(text(*stderr_bytes), "recorded failure stderr\n");

    auto replayed =
        liquid::runtime::block_on(liquid::build::replay(fixture.repository, ran->value));
    ASSERT_TRUE(replayed) << replayed.error().message << ": " << replayed.error().context;
    EXPECT_TRUE(replayed->agreement);
    EXPECT_EQ(replayed->recorded_exit, 37);
    EXPECT_EQ(replayed->reexecuted_exit, 37);
    EXPECT_TRUE(replayed->differences.empty());
    EXPECT_FALSE(replayed->attestation.has_value());
}

TEST(BuildReplay, cached_hit_replays_by_action_and_records_only_when_requested) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::to_string(first->value), liquid::to_string(second->value));
    auto recorded = liquid::runtime::block_on(liquid::build::result(fixture.repository, *second));
    ASSERT_TRUE(recorded) << recorded.error().message;
    EXPECT_EQ(recorded->cache, liquid::cache_status::hit);

    auto replayed =
        liquid::runtime::block_on(liquid::build::replay(fixture.repository, recorded->action));
    ASSERT_TRUE(replayed) << replayed.error().message;
    EXPECT_TRUE(replayed->agreement);
    EXPECT_FALSE(replayed->attestation.has_value());

    auto sampled = liquid::runtime::block_on(
        liquid::build::replay(fixture.repository, recorded->action, {.record = true}));
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_TRUE(sampled->agreement);
    EXPECT_TRUE(sampled->attestation.has_value());
}

TEST(BuildReplay, tampered_recorded_output_reports_the_first_differing_byte) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto raw = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(fixture.repository, ran->value));
    ASSERT_TRUE(raw) << raw.error().message;
    auto recorded = liquid::objects::decode_result(*raw);
    ASSERT_TRUE(recorded) << recorded.error().message;
    const auto tampered_path = fixture.location.path() / "tampered-output";
    auto tampered_file = std::ofstream{tampered_path, std::ios::binary | std::ios::trunc};
    tampered_file << "Xnput";
    tampered_file.close();
    ASSERT_TRUE(tampered_file);
    auto tampered_blob = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_file(fixture.repository, tampered_path));
    ASSERT_TRUE(tampered_blob) << tampered_blob.error().message;
    recorded->outputs.front().blob = *tampered_blob;
    ASSERT_TRUE(liquid::runtime::block_on(
        liquid::repository_detail::build_access::sign_result(fixture.repository, *recorded)));
    auto encoded = liquid::objects::encode(*recorded);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto tampered_result =
        liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
            fixture.repository, std::move(*encoded)));
    ASSERT_TRUE(tampered_result) << tampered_result.error().message;

    auto replayed =
        liquid::runtime::block_on(liquid::build::replay(fixture.repository, *tampered_result));
    ASSERT_TRUE(replayed) << replayed.error().message;
    ASSERT_FALSE(replayed->agreement);
    ASSERT_EQ(replayed->differences.size(), 1U);
    const auto& difference = replayed->differences.front();
    EXPECT_EQ(difference.output, "out/copy.txt");
    ASSERT_TRUE(difference.byte_offset.has_value());
    EXPECT_EQ(*difference.byte_offset, 0U);
    ASSERT_TRUE(difference.cached_byte.has_value());
    ASSERT_TRUE(difference.reexecuted_byte.has_value());
    EXPECT_EQ(*difference.cached_byte, static_cast<std::uint8_t>('X'));
    EXPECT_EQ(*difference.reexecuted_byte, static_cast<std::uint8_t>('i'));
    EXPECT_FALSE(replayed->attestation.has_value());
    auto statistics = liquid::runtime::block_on(fixture.repository.cache_stats());
    ASSERT_TRUE(statistics) << statistics.error().message;
    EXPECT_TRUE(statistics->critical_alerts.empty());
}

TEST(BuildReplay, remote_input_is_fetched_through_the_blob_source) {
    temporary_repository remote_location;
    auto remote_store = liquid::runtime::block_on(liquid::store::open(remote_location.path()));
    ASSERT_TRUE(remote_store) << remote_store.error().message;
    auto remote =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*remote_store));
    action_fixture fixture{true, 20, true, remote};

    auto input = remote->put(bytes("remote-input"));
    ASSERT_TRUE(input) << input.error().message;
    input->remote = liquid::remote_blob_location{"disk", "worker-a"};
    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.write_remote("other.txt", *input)));
    fixture.cut("remote replay input");

    auto output = remote->put(bytes("recorded failure output\n"));
    ASSERT_TRUE(output) << output.error().message;
    output->remote = liquid::remote_blob_location{"disk", "worker-a"};
    auto output_record = liquid::runtime::block_on(fixture.repository.store_remote_blob(*output));
    ASSERT_TRUE(output_record) << output_record.error().message;
    auto stdout = remote->put(bytes("recorded failure stdout\n"));
    ASSERT_TRUE(stdout) << stdout.error().message;
    stdout->remote = liquid::remote_blob_location{"disk", "worker-a"};
    auto stdout_record = liquid::runtime::block_on(fixture.repository.store_remote_blob(*stdout));
    ASSERT_TRUE(stdout_record) << stdout_record.error().message;
    auto stderr = remote->put(bytes("recorded failure stderr\n"));
    ASSERT_TRUE(stderr) << stderr.error().message;
    stderr->remote = liquid::remote_blob_location{"disk", "worker-a"};
    auto stderr_record = liquid::runtime::block_on(fixture.repository.store_remote_blob(*stderr));
    ASSERT_TRUE(stderr_record) << stderr_record.error().message;

    const auto action = fixture.input({"fixture", "fail", "out/copy.txt"}, {"out/copy.txt"});
    const auto expected =
        liquid::action_result{.exit = 37,
                              .outputs = {{"out/copy.txt", 0100644, *output_record}},
                              .stdout_output = *stdout_record,
                              .stderr_output = *stderr_record};
    auto recorded = liquid::runtime::block_on(
        liquid::build::record_remote_result(fixture.repository, action, expected));
    ASSERT_TRUE(recorded) << recorded.error().message << ": " << recorded.error().context;
    auto status = fixture.repository.status();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->remote_bytes, input->size);
    EXPECT_EQ(remote->reads(), 0U);

    auto replayed =
        liquid::runtime::block_on(liquid::build::replay(fixture.repository, recorded->value));
    ASSERT_TRUE(replayed) << replayed.error().message << ": " << replayed.error().context;
    ASSERT_TRUE(replayed->agreement)
        << (replayed->differences.empty() ? "no reported difference"
                                          : replayed->differences.front().output)
        << " recorded=" << replayed->recorded_exit << " reexecuted=" << replayed->reexecuted_exit;
    EXPECT_GT(remote->reads(), 0U);
    EXPECT_TRUE(std::ranges::any_of(remote->read_blobs(), [&input](const liquid::digest& value) {
        return liquid::to_string(value) == liquid::to_string(input->content);
    }));
    auto hydrated = liquid::runtime::block_on(
        liquid::repository_detail::build_access::contains(fixture.repository, input->content));
    ASSERT_TRUE(hydrated) << hydrated.error().message;
    EXPECT_TRUE(*hydrated);
}

TEST(BuildVerification, nondeterminism_is_alerted_and_the_divergent_result_is_never_served) {
    action_fixture fixture;
    const auto input = fixture.input({"fixture", "random", "out/random.bin"}, {"out/random.bin"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto first_value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    ASSERT_TRUE(first_value) << first_value.error().message;
    ASSERT_EQ(first_value->exit, 0);
    ASSERT_EQ(first_value->outputs.size(), 1U);
    auto report = liquid::runtime::block_on(liquid::build::verify(
        fixture.repository, {.selection = liquid::verify_selection::all, .explain = true}));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->actions.size(), 1U);
    ASSERT_FALSE(report->actions.front().agreement);
    auto raw = liquid::runtime::block_on(liquid::repository_detail::build_access::get(
        fixture.repository, report->actions.front().attestation));
    ASSERT_TRUE(raw) << raw.error().message;
    auto attestation = liquid::objects::decode_attestation(*raw);
    ASSERT_TRUE(attestation) << attestation.error().message;
    auto verified_attestation = liquid::verify_attestation(*attestation);
    ASSERT_TRUE(verified_attestation) << verified_attestation.error().message;
    EXPECT_TRUE(verified_attestation->verified) << verified_attestation->reason;
    const auto difference = std::ranges::find(report->actions.front().differences,
                                              "out/random.bin",
                                              &liquid::reproducibility_difference::output);
    ASSERT_NE(difference, report->actions.front().differences.end())
        << report->actions.front().differences.front().output;
    EXPECT_TRUE(difference->byte_offset.has_value());
    auto value = liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(value->cache, liquid::cache_status::divergent);
    auto statistics = liquid::runtime::block_on(fixture.repository.cache_stats());
    ASSERT_TRUE(statistics) << statistics.error().message;
    ASSERT_EQ(statistics->critical_alerts.size(), 1U);
    EXPECT_EQ(liquid::to_string(statistics->critical_alerts.front().action),
              liquid::to_string(value->action));
    ASSERT_FALSE(statistics->critical_alerts.front().differences.empty());
    EXPECT_EQ(statistics->critical_alerts.front().differences.front().output, "out/random.bin");

    fixture.repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(fixture.location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    fixture.repository = std::move(*reopened);
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(liquid::to_string(second->value), liquid::to_string(first->value));
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 1U);
}

TEST(BuildVerification, rate_one_samples_every_unverified_action) {
    action_fixture fixture{true, 1};
    fixture.repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(fixture.location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    fixture.repository = std::move(*reopened);
    auto first = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    auto second = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "env", "out/env.txt"}, {"out/env.txt"})));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    auto report = liquid::runtime::block_on(
        liquid::build::verify(fixture.repository, {.selection = liquid::verify_selection::sample}));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->considered, 2U);
    ASSERT_EQ(report->actions.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(report->actions, &liquid::verified_action::agreement));
}

TEST(BuildCache, unsigned_newest_candidate_is_never_served) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto raw = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(fixture.repository, first->value));
    ASSERT_TRUE(raw);
    auto value = liquid::objects::decode_result(*raw);
    ASSERT_TRUE(value);
    auto action_raw = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(fixture.repository, value->action));
    ASSERT_TRUE(action_raw);
    auto action = liquid::objects::decode_action(*action_raw);
    ASSERT_TRUE(action);
    value->sig.clear();
    auto unsigned_bytes = liquid::objects::encode(*value);
    ASSERT_TRUE(unsigned_bytes);
    auto unsigned_id =
        liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
            fixture.repository, std::move(*unsigned_bytes)));
    ASSERT_TRUE(unsigned_id);

    auto cache = liquid::build_detail::disk_cache{fixture.repository};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
    auto candidate = cache.candidate_key(input, action->sandbox);
    ASSERT_TRUE(candidate);
    auto index =
        liquid::build_detail::action_index{fixture.location.path() / ".liquid-store" / "actions"};
    ASSERT_TRUE(liquid::runtime::block_on(index.open()));
    ASSERT_TRUE(index.insert(*candidate, *unsigned_id));
    ASSERT_TRUE(liquid::runtime::block_on(index.persist(index.root())));

    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::to_string(second->value), liquid::to_string(first->value));
    EXPECT_NE(liquid::to_string(second->value), liquid::to_string(*unsigned_id));
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 0U);
}

TEST(BuildCache, action_index_hits_after_repository_reopen_and_index_rebuild) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    fixture.repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(fixture.location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    fixture.repository = std::move(*reopened);
    std::filesystem::remove(fixture.location.path() / ".liquid-store" / "actions" / "INDEX");
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(liquid::to_string(first->value), liquid::to_string(second->value));
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 0U);
}

TEST(Build, terminates_the_process_group_on_timeout) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "sleep"}, {});
    input.timeout = std::chrono::seconds{1};
    auto ran =
        liquid::runtime::block_on(liquid::build::execute(fixture.repository, std::move(input)));
    ASSERT_FALSE(ran);
    EXPECT_EQ(ran.error().code, liquid::errc::unavailable);
    EXPECT_EQ(ran.error().message, "action timed out");
}
#endif

} // namespace
