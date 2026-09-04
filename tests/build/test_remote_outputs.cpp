#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "remote_blob_source.hpp"

#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "liquid/vfs.hpp"
#include "repository/testing.hpp"
#include "vfs/repository_access.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-remote-output-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto make_bytes(std::size_t size) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(size);
    for (auto index = std::size_t{}; index != size; ++index) {
        output[index] = static_cast<std::byte>((index * 131U + index / 17U) & 0xffU);
    }
    return output;
}

auto host_platform() -> liquid::platform {
#if defined(__linux__)
    constexpr auto os = "linux";
#elif defined(__APPLE__)
    constexpr auto os = "macos";
#else
    constexpr auto os = "windows";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr auto arch = "x86_64";
#else
    constexpr auto arch = "arm64";
#endif
    return {os, arch};
}

struct remote_fixture {
    temporary_directory location;
    std::shared_ptr<liquid::test_support::on_disk_remote_source> remote;
    liquid::repository repository;
    liquid::blob value;
    liquid::digest record;
    std::vector<std::byte> contents;

    remote_fixture() : contents(make_bytes(768U * 1024U)) {
        auto remote_store =
            liquid::runtime::block_on(liquid::store::open(location.path() / "remote"));
        EXPECT_TRUE(remote_store) << remote_store.error().message;
        remote =
            std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*remote_store));
        auto stored = remote->put(contents);
        EXPECT_TRUE(stored) << stored.error().message;
        value = *stored;
        value.remote = liquid::remote_blob_location{"disk", "worker-a"};

        auto options = liquid::repository_options{};
        options.storage.remote_sources.emplace("disk", remote);
        options.cache.publish = false;
        auto opened = liquid::runtime::block_on(
            liquid::repository::create(location.path() / "local", std::move(options)));
        EXPECT_TRUE(opened) << opened.error().message;
        repository = std::move(*opened);
        auto registered = liquid::runtime::block_on(repository.store_remote_blob(value));
        EXPECT_TRUE(registered) << registered.error().message;
        record = *registered;
    }
};

TEST(RemoteOutputs, records_unused_result_without_transferring_bytes) {
    remote_fixture fixture;
    auto written =
        liquid::runtime::block_on(fixture.repository.write_remote("input.bin", fixture.value));
    ASSERT_TRUE(written) << written.error().message;
    auto cut = liquid::runtime::block_on(fixture.repository.cut("remote input"));
    ASSERT_TRUE(cut) << cut.error().message;
    auto log = liquid::runtime::block_on(fixture.repository.log());
    ASSERT_TRUE(log) << log.error().message;
    auto input = liquid::action_input{.command = {"remote-tool"},
                                      .inputs = log->front().root,
                                      .toolchain = log->front().root,
                                      .platform = host_platform()};
    auto value = liquid::action_result{.outputs = {{"out.bin", 0100644, fixture.record}},
                                       .stdout_output = fixture.record,
                                       .stderr_output = fixture.record};
    auto recorded = liquid::runtime::block_on(
        liquid::build::record_remote_result(fixture.repository, input, value));
    ASSERT_TRUE(recorded) << recorded.error().message << ": " << recorded.error().context;
    EXPECT_EQ(fixture.remote->reads(), 0U);
    EXPECT_EQ(fixture.remote->bytes_read(), 0U);
}

TEST(RemoteOutputs, read_hydrates_only_intersecting_chunks) {
    remote_fixture fixture;
    auto written =
        liquid::runtime::block_on(fixture.repository.write_remote("out.bin", fixture.value));
    ASSERT_TRUE(written) << written.error().message;
    auto status = fixture.repository.status();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->remote_bytes, fixture.contents.size());

    ASSERT_GE(fixture.value.chunks.size(), 3U);
    const auto offset = fixture.value.chunk_sizes.front() - 1U;
    const auto length = fixture.value.chunk_sizes[1] + 2U;
    const auto cache = fixture.location.path() / "hydration-cache";
    auto read = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        fixture.repository, "out.bin", cache, offset, length));
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read->bytes,
              std::vector<std::byte>(fixture.contents.begin() + offset,
                                     fixture.contents.begin() + offset + length));
    EXPECT_EQ(fixture.remote->reads(), 3U);
    EXPECT_EQ(fixture.remote->bytes_read(),
              fixture.value.chunk_sizes[0] + fixture.value.chunk_sizes[1] +
                  fixture.value.chunk_sizes[2]);

    auto repeated = liquid::runtime::block_on(liquid::repository_detail::testing::cached_vfs_read(
        fixture.repository, "out.bin", cache, offset, length));
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(fixture.remote->reads(), 3U);

    const auto checkout = fixture.location.path() / "checkout";
    auto materialised =
        liquid::runtime::block_on(liquid::vfs::materialise(fixture.repository, checkout));
    ASSERT_TRUE(materialised) << materialised.error().message;
    auto input = std::ifstream{checkout / "out.bin", std::ios::binary};
    auto materialised_bytes = std::vector<std::byte>(fixture.contents.size());
    input.read(reinterpret_cast<char*>(materialised_bytes.data()),
               static_cast<std::streamsize>(materialised_bytes.size()));
    ASSERT_TRUE(input);
    EXPECT_EQ(materialised_bytes, fixture.contents);
    EXPECT_EQ(fixture.remote->reads(), fixture.value.chunks.size());
    auto hydrated_status = fixture.repository.status();
    ASSERT_TRUE(hydrated_status) << hydrated_status.error().message;
    EXPECT_EQ(hydrated_status->remote_bytes, 0U);
}

TEST(RemoteOutputs, store_reports_absent_remote_and_local_availability) {
    remote_fixture fixture;
    auto options = liquid::store_options{};
    options.remote_sources.emplace("disk", fixture.remote);
    auto opened = liquid::runtime::block_on(
        liquid::store::open(fixture.location.path() / "availability", std::move(options)));
    ASSERT_TRUE(opened) << opened.error().message;
    auto local = std::move(*opened);
    auto absent =
        liquid::runtime::block_on(local.has(liquid::digest{liquid::algorithm::blake3, {}}));
    ASSERT_TRUE(absent) << absent.error().message;
    EXPECT_EQ(*absent, liquid::blob_availability::absent);
    auto registered = liquid::runtime::block_on(local.register_remote(fixture.value));
    ASSERT_TRUE(registered) << registered.error().message;
    auto remote = liquid::runtime::block_on(local.has(fixture.value.content));
    ASSERT_TRUE(remote) << remote.error().message;
    EXPECT_EQ(*remote, liquid::blob_availability::remote);
    auto contents = liquid::runtime::block_on(local.get(fixture.value.content));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(*contents, fixture.contents);
    auto local_status = liquid::runtime::block_on(local.has(fixture.value.content));
    ASSERT_TRUE(local_status) << local_status.error().message;
    EXPECT_EQ(*local_status, liquid::blob_availability::local);
}

TEST(RemoteOutputs, remote_registration_survives_store_reopen) {
    remote_fixture fixture;
    const auto path = fixture.location.path() / "reopen";
    {
        auto options = liquid::store_options{};
        options.remote_sources.emplace("disk", fixture.remote);
        auto opened = liquid::runtime::block_on(liquid::store::open(path, std::move(options)));
        ASSERT_TRUE(opened) << opened.error().message;
        auto registered = liquid::runtime::block_on(opened->register_remote(fixture.value));
        ASSERT_TRUE(registered) << registered.error().message;
    }
    auto options = liquid::store_options{};
    options.remote_sources.emplace("disk", fixture.remote);
    auto reopened = liquid::runtime::block_on(liquid::store::open(path, std::move(options)));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto availability = liquid::runtime::block_on(reopened->has(fixture.value.content));
    ASSERT_TRUE(availability) << availability.error().message;
    EXPECT_EQ(*availability, liquid::blob_availability::remote);
    auto contents = liquid::runtime::block_on(reopened->get(fixture.value.content));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ(*contents, fixture.contents);
}

TEST(RemoteOutputs, sha256_remote_chunks_hydrate_into_default_store) {
    temporary_directory location;
    auto remote_options = liquid::store_options{};
    remote_options.hash_algorithm = liquid::algorithm::sha256;
    auto remote_store = liquid::runtime::block_on(
        liquid::store::open(location.path() / "sha-remote", std::move(remote_options)));
    ASSERT_TRUE(remote_store) << remote_store.error().message;
    auto source =
        std::make_shared<liquid::test_support::on_disk_remote_source>(std::move(*remote_store));
    const auto expected = make_bytes(512U * 1024U);
    auto value = source->put(expected);
    ASSERT_TRUE(value) << value.error().message;
    value->remote = liquid::remote_blob_location{"sha", "worker-a"};

    auto local_options = liquid::store_options{};
    local_options.remote_sources.emplace("sha", source);
    auto local = liquid::runtime::block_on(
        liquid::store::open(location.path() / "default-local", std::move(local_options)));
    ASSERT_TRUE(local) << local.error().message;
    auto registered = liquid::runtime::block_on(local->register_remote(*value));
    ASSERT_TRUE(registered) << registered.error().message;
    auto actual = liquid::runtime::block_on(local->get(value->content));
    ASSERT_TRUE(actual) << actual.error().message;
    EXPECT_EQ(*actual, expected);
    auto availability = liquid::runtime::block_on(local->has(value->content));
    ASSERT_TRUE(availability) << availability.error().message;
    EXPECT_EQ(*availability, liquid::blob_availability::local);
}

TEST(RemoteOutputs, consecutive_remote_result_uses_metadata_without_client_read) {
    remote_fixture fixture;
    auto written =
        liquid::runtime::block_on(fixture.repository.write_remote("first.bin", fixture.value));
    ASSERT_TRUE(written) << written.error().message;
    auto cut = liquid::runtime::block_on(fixture.repository.cut("remote first output"));
    ASSERT_TRUE(cut) << cut.error().message;
    auto log = liquid::runtime::block_on(fixture.repository.log());
    ASSERT_TRUE(log) << log.error().message;

    auto input = liquid::action_input{.command = {"consume", "first.bin"},
                                      .inputs = log->front().root,
                                      .toolchain = log->front().root,
                                      .platform = host_platform()};
    auto value = liquid::action_result{.outputs = {{"second.bin", 0100644, fixture.record}},
                                       .stdout_output = fixture.record,
                                       .stderr_output = fixture.record};
    auto recorded = liquid::runtime::block_on(
        liquid::build::record_remote_result(fixture.repository, input, value));
    ASSERT_TRUE(recorded) << recorded.error().message << ": " << recorded.error().context;
    EXPECT_EQ(fixture.remote->reads(), 0U);
}

} // namespace
